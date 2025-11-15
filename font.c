#include "font.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Sparse texture extensions - Use Vulkan sparse textures (only allocate used regions)
// Load only codepoints visible in kink window

FT_Library ft;

void init_free_type() {
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "Could not init FreeType Library\n");
        exit(1);
    }
}

// Simple hash function for codepoints
static inline unsigned int hash_codepoint(uint32_t codepoint) {
    return (codepoint * 2654435761u) % FONT_HASH_SIZE;
}

// Get character from hash table
static Character* get_cached_character(Font* font, uint32_t codepoint) {
    unsigned int hash = hash_codepoint(codepoint);
    CharNode *node = font->char_table[hash];
    
    while (node) {
        if (node->codepoint == codepoint) {
            return &node->character;
        }
        node = node->next;
    }
    
    return NULL;
}

// Add character to hash table
static void cache_character(Font* font, uint32_t codepoint, Character ch) {
    unsigned int hash = hash_codepoint(codepoint);
    
    CharNode *node = (CharNode*)malloc(sizeof(CharNode));
    node->codepoint = codepoint;
    node->character = ch;
    node->next = font->char_table[hash];
    font->char_table[hash] = node;
}

// Grow the atlas to accommodate more glyphs
static bool grow_atlas(Font* font) {
    unsigned int new_width = font->width;
    unsigned int new_height = font->height * 2; // Double the height
    unsigned int old_height = font->height;
    
    // Cap at 16384x16384 (256MB RGBA) - most modern GPUs support this
    // If you need more, consider multiple atlases or a different approach
    if (new_height > 16384) {
        fprintf(stderr, "Font atlas reached maximum size (16384x16384)!\n");
        fprintf(stderr, "Consider using a smaller font size or fewer unique glyphs.\n");
        return false;
    }
    
    // Warn when getting large
    if (new_height >= 8192) {
        fprintf(stderr, "Warning: Font atlas is getting very large (%ux%u)\n", 
                new_width, new_height);
    }
    
    fprintf(stderr, "Growing font atlas from %ux%u to %ux%u\n", 
            font->width, font->height, new_width, new_height);
    
    // Allocate new larger buffer
    unsigned char *new_buffer = (unsigned char*)calloc(new_width * new_height * 4, sizeof(unsigned char));
    if (!new_buffer) {
        fprintf(stderr, "Failed to allocate memory for larger atlas\n");
        return false;
    }
    
    // Copy old content to new buffer
    for (unsigned int y = 0; y < font->height; y++) {
        memcpy(new_buffer + (y * new_width * 4),
               font->atlas_buffer + (y * font->width * 4),
               font->width * 4);
    }
    
    // Free old buffer and update
    free(font->atlas_buffer);
    font->atlas_buffer = new_buffer;
    font->height = new_height;
    
    // FIX: Update all cached character UV coordinates for new atlas dimensions
    // When height doubles, all ty coordinates need to be scaled by old_height/new_height
    float height_scale = (float)old_height / (float)new_height;
    
    for (int i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font->char_table[i];
        while (node) {
            // Scale ty coordinate proportionally
            node->character.ty *= height_scale;
            node = node->next;
        }
    }
    
    // Recreate Vulkan texture with new size
    destroy_texture(&context, &font->texture);
    if (!load_texture_from_rgba(&context, font->atlas_buffer, font->width, font->height, &font->texture)) {
        fprintf(stderr, "Failed to recreate texture after atlas growth\n");
        return false;
    }
    
    font->needs_update = false;
    return true;
}

// Load a single glyph into the atlas
static bool load_glyph(Font* font, uint32_t codepoint, Character *out_char) {
    if (FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER)) {
        fprintf(stderr, "Failed to load glyph for codepoint U+%04X\n", codepoint);
        return false;
    }
    
    FT_GlyphSlot glyph = font->face->glyph;
    
    // Check if we need to move to next row
    if (font->atlas_x + glyph->bitmap.width + 1 >= font->width) {
        font->atlas_y += font->row_height;
        font->row_height = 0;
        font->atlas_x = 0;
    }
    
    // Check if atlas is full - try to grow it
    if (font->atlas_y + glyph->bitmap.rows >= font->height) {
        if (!grow_atlas(font)) {
            fprintf(stderr, "Cannot load glyph U+%04X - atlas full and cannot grow\n", codepoint);
            return false;
        }
        // After growing, we have more space, continue with current position
    }
    
    // Copy glyph to atlas (convert grayscale to RGBA)
    for (unsigned int y = 0; y < glyph->bitmap.rows; y++) {
        for (unsigned int x = 0; x < glyph->bitmap.width; x++) {
            int atlas_y = font->atlas_y + y;
            int glyph_y = glyph->bitmap.rows - 1 - y; // Flip vertically
            unsigned char value = glyph->bitmap.buffer[x + glyph_y * glyph->bitmap.pitch];
            
            int atlas_idx = ((font->atlas_x + x) + (atlas_y * font->width)) * 4;
            font->atlas_buffer[atlas_idx + 0] = 255;      // R
            font->atlas_buffer[atlas_idx + 1] = 255;      // G
            font->atlas_buffer[atlas_idx + 2] = 255;      // B
            font->atlas_buffer[atlas_idx + 3] = value;    // A
        }
    }
    
    // Store character info
    out_char->ax = glyph->advance.x >> 6;
    out_char->ay = glyph->advance.y >> 6;
    out_char->bw = glyph->bitmap.width;
    out_char->bh = glyph->bitmap.rows;
    out_char->bl = glyph->bitmap_left;
    out_char->bt = glyph->bitmap_top;
    out_char->tx = font->atlas_x / (float)font->width;
    out_char->ty = font->atlas_y / (float)font->height;
    
    font->row_height = fmax(font->row_height, glyph->bitmap.rows);
    font->atlas_x += glyph->bitmap.width + 1;
    
    font->needs_update = true;
    
    return true;
}

// Get or load a character
Character* font_get_character(Font* font, uint32_t codepoint) {
    // Check cache first
    Character *cached = get_cached_character(font, codepoint);
    if (cached) {
        return cached;
    }
    
    // Load new glyph
    Character new_char;
    if (!load_glyph(font, codepoint, &new_char)) {
        // Fall back to replacement character (box)
        codepoint = 0xFFFD;
        cached = get_cached_character(font, codepoint);
        if (cached) return cached;
        
        // If even replacement char isn't loaded, try to load it
        if (!load_glyph(font, codepoint, &new_char)) {
            // Ultimate fallback: space character
            codepoint = ' ';
            cached = get_cached_character(font, codepoint);
            if (cached) return cached;
            return NULL;
        }
    }
    
    // Cache and return
    cache_character(font, codepoint, new_char);
    return get_cached_character(font, codepoint);
}

Font* load_font(const char* fontPath, int fontSize) {
    FT_Face face;
    if (FT_New_Face(ft, fontPath, 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", fontPath);
        return NULL;
    }

    printf("[LOADED FONT] %s %i\n", fontPath, fontSize);
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    Font* font = (Font*)calloc(1, sizeof(Font));
    if (!font) {
        FT_Done_Face(face);
        fprintf(stderr, "Memory allocation failed for font structure\n");
        return NULL;
    }

    // Initialize font
    font->face = face;
    font->ascent = face->size->metrics.ascender >> 6;
    font->descent = -(face->size->metrics.descender >> 6);
    font->width = 2048;   // Larger atlas for Unicode
    font->height = 2048;
    font->atlas_x = 0;
    font->atlas_y = 0;
    font->row_height = 0;
    font->needs_update = false;
    
    // Initialize hash table
    memset(font->char_table, 0, sizeof(font->char_table));

    // Create atlas buffer (RGBA for Vulkan)
    font->atlas_buffer = (unsigned char*)calloc(font->width * font->height * 4, sizeof(unsigned char));
    if (!font->atlas_buffer) {
        free(font);
        FT_Done_Face(face);
        fprintf(stderr, "Memory allocation failed for texture atlas\n");
        return NULL;
    }

    // Pre-load ASCII characters (32-126)
    for (uint32_t i = 32; i < 127; i++) {
        Character ch;
        if (load_glyph(font, i, &ch)) {
            cache_character(font, i, ch);
        }
    }
    
    // Load common Unicode characters
    uint32_t common_chars[] = {
        0xFFFD,  // Replacement character
        0x2013, 0x2014,  // En dash, em dash
        0x2018, 0x2019, 0x201C, 0x201D,  // Smart quotes
        0x2026,  // Ellipsis
    };
    
    for (size_t i = 0; i < sizeof(common_chars) / sizeof(common_chars[0]); i++) {
        Character ch;
        if (load_glyph(font, common_chars[i], &ch)) {
            cache_character(font, common_chars[i], ch);
        }
    }

    // Create Vulkan texture
    if (!load_texture_from_rgba(&context, font->atlas_buffer, font->width, font->height, &font->texture)) {
        fprintf(stderr, "Failed to create Vulkan texture from font atlas\n");
        free(font->atlas_buffer);
        free(font);
        FT_Done_Face(face);
        return NULL;
    }

    font->needs_update = false;
    return font;
}

// FIX: Batch texture updates - only update once per frame
static void update_texture_if_needed(Font* font) {
    if (!font->needs_update) return;
    
    // Update the Vulkan texture with new atlas data
    update_texture_from_rgba(&context, &font->texture, font->atlas_buffer, 
                            font->width, font->height);
    
    font->needs_update = false;
}

// NEW: Force immediate texture update (call this after batch character loading)
void font_flush_updates(Font* font) {
    update_texture_if_needed(font);
}

float character(Font* font, uint32_t codepoint, float x, float y, Color color) {
    if (!font) return 0.0f;
    
    // Handle newline (don't draw, just return advance)
    if (codepoint == '\n') {
        return 0.0f;
    }
    
    // Skip control characters
    if (codepoint < 32) {
        return 0.0f;
    }
    
    // Get or load character (DOESN'T update texture yet)
    Character *ch = font_get_character(font, codepoint);
    if (!ch) {
        return font->ascent; // Return some default width
    }
    
    // Calculate position (align baseline)
    float xpos = x + ch->bl;
    float ypos = y - (ch->bh - ch->bt + font->descent);

    
    float w = ch->bw;
    float h = ch->bh;
    
    // Skip if no visible pixels, but still return advance
    if (w == 0 || h == 0) {
        return ch->ax;
    }
    
    // Calculate UV coordinates
    float u1 = ch->tx;
    float v1 = ch->ty + ch->bh / (float)font->height;
    float u2 = ch->tx + ch->bw / (float)font->width;
    float v2 = ch->ty;
    
    // Check vertex buffer space
    if (vertexCount2D + 6 > MAX_VERTICES) {
        fprintf(stderr, "Vertex buffer full, cannot render character\n");
        return ch->ax;
    }
    
    Vertex2D quad[6] = {
        {{xpos, ypos + h}, color, {u1, v1}, 0},
        {{xpos, ypos}, color, {u1, v2}, 0},
        {{xpos + w, ypos}, color, {u2, v2}, 0},
        
        {{xpos, ypos + h}, color, {u1, v1}, 0},
        {{xpos + w, ypos}, color, {u2, v2}, 0},
        {{xpos + w, ypos + h}, color, {u2, v1}, 0}
    };
    
    // Find or create batch for this texture
    int batchIndex = -1;
    if (textureBatchCount > 0 && textureBatches[textureBatchCount - 1].texture == &font->texture) {
        batchIndex = textureBatchCount - 1;
    } else {
        if (textureBatchCount >= MAX_TEXTURES) {
            fprintf(stderr, "Too many texture batches!\n");
            return ch->ax;
        }
        batchIndex = textureBatchCount++;
        textureBatches[batchIndex].texture = &font->texture;
        textureBatches[batchIndex].startVertex = coloredVertexCount + (vertexCount2D - coloredVertexCount);
        textureBatches[batchIndex].vertexCount = 0;
    }
    
    // Add vertices at the end of the buffer
    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
    textureBatches[batchIndex].vertexCount += 6;
    
    return ch->ax;
}

void text(Font* font, const char* text_str, float x, float y, Color color) {
    if (!font || !text_str) return;

    const unsigned char* p = (const unsigned char*)text_str;
    float initialX = x;
    float lineHeight = (font->ascent + font->descent);

    while (*p) {
        // Handle newline
        if (*p == '\n') {
            x = initialX;
            y -= lineHeight;
            p++;
            continue;
        }
        
        // Decode UTF-8 character
        uint32_t codepoint;
        size_t bytes_read;
        
        if ((*p & 0x80) == 0) {
            // 1-byte character
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            // 2-byte character
            codepoint = (*p & 0x1F) << 6;
            codepoint |= (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            // 3-byte character
            codepoint = (*p & 0x0F) << 12;
            codepoint |= (*(p+1) & 0x3F) << 6;
            codepoint |= (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            // 4-byte character
            codepoint = (*p & 0x07) << 18;
            codepoint |= (*(p+1) & 0x3F) << 12;
            codepoint |= (*(p+2) & 0x3F) << 6;
            codepoint |= (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            // Invalid UTF-8, skip
            p++;
            continue;
        }
        
        // Render character (doesn't update texture)
        x += character(font, codepoint, x, y, color);
        p += bytes_read;
    }
    
    // FIX: Update texture once after rendering all characters
    update_texture_if_needed(font);
}


void text3D(Font* font, const char* text_str, vec3 position, float size, Color color) {
    if (!font || !text_str) return;

    // Calculate total text width for centering (UTF-8 aware)
    float totalWidth = 0.0f;
    const unsigned char* p = (const unsigned char*)text_str;
    
    while (*p && *p != '\n') {
        uint32_t codepoint;
        size_t bytes_read;
        
        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | 
                       (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            p++;
            continue;
        }
        
        Character *ch = font_get_character(font, codepoint);
        if (ch) {
            totalWidth += ch->ax * size / (float)font->height;
        }
        p += bytes_read;
    }

    float startX = -totalWidth / 2.0f;
    float x = startX;
    
    p = (const unsigned char*)text_str;
    
    while (*p && *p != '\n') {
        uint32_t codepoint;
        size_t bytes_read;
        
        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | 
                       (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            p++;
            continue;
        }
        
        Character *ch = font_get_character(font, codepoint);
        if (!ch) {
            p += bytes_read;
            continue;
        }

        float charWidth = ch->bw * size / (float)font->height;
        float charHeight = ch->bh * size / (float)font->height;
        
        float xpos = x + ch->bl * size / (float)font->height;
        float ypos = -(ch->bh - ch->bt) * size / (float)font->height - 
                     font->descent * size / (float)font->height;

        if (ch->bw > 0 && ch->bh > 0) {
            if (vertex_count_3D_textured + 6 > MAX_VERTICES) {
                fprintf(stderr, "3D textured vertex buffer full\n");
                return;
            }

            float u1 = ch->tx;
            float v1 = ch->ty + ch->bh / (float)font->height;
            float u2 = ch->tx + ch->bw / (float)font->width;
            float v2 = ch->ty;

            Vertex quad[6] = {
                {.pos = {position[0] + xpos, position[1] + ypos + charHeight, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u2, v1}},
                {.pos = {position[0] + xpos, position[1] + ypos, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u2, v2}},
                {.pos = {position[0] + xpos + charWidth, position[1] + ypos, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u1, v2}},
                {.pos = {position[0] + xpos, position[1] + ypos + charHeight, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u2, v1}},
                {.pos = {position[0] + xpos + charWidth, position[1] + ypos, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u1, v2}},
                {.pos = {position[0] + xpos + charWidth, position[1] + ypos + charHeight, position[2]},
                 .color = {color.r, color.g, color.b, color.a},
                 .normal = {0.0f, 0.0f, 1.0f}, .texCoord = {u1, v1}}
            };
            
            int batchIndex = -1;
            if (texture3DBatchCount > 0 && 
                texture3DBatches[texture3DBatchCount - 1].texture == &font->texture) {
                batchIndex = texture3DBatchCount - 1;
            } else {
                if (texture3DBatchCount >= MAX_TEXTURES) {
                    fprintf(stderr, "Too many 3D texture batches!\n");
                    return;
                }
                batchIndex = texture3DBatchCount++;
                texture3DBatches[batchIndex].texture = &font->texture;
                texture3DBatches[batchIndex].startVertex = vertex_count_3D_textured;
                texture3DBatches[batchIndex].vertexCount = 0;
            }
            
            memcpy(&vertices3D_textured[vertex_count_3D_textured], quad, sizeof(quad));
            vertex_count_3D_textured += 6;
            texture3DBatches[batchIndex].vertexCount += 6;
        }
        
        x += ch->ax * size / (float)font->height;
        p += bytes_read;
    }
    
    // FIX: Update texture once after rendering all characters
    update_texture_if_needed(font);
}

// Static variables for FPS calculation
static double last_fps_time = 0.0;
static uint32_t frame_count = 0;
static float current_fps = 0.0f;
static char fps_text[32] = "FPS: 0";

void fps(Font* font, float x, float y, Color color) {
    if (!font) return;
    
    double current_time = glfwGetTime();
    
    // Initialize on first call
    if (last_fps_time == 0.0) {
        last_fps_time = current_time;
        return;
    }
    
    frame_count++;
    
    // Calculate FPS every second
    double elapsed = current_time - last_fps_time;
    if (elapsed >= 1.0) {
        current_fps = (float)frame_count / (float)elapsed;
        frame_count = 0;
        last_fps_time = current_time;
        
        // Only update string when FPS changes
        snprintf(fps_text, sizeof(fps_text), "FPS: %.0f", current_fps);
    }
    
    // Draw with cached string
    text(font, fps_text, x, y, color);
}


void destroy_font(Font* font) {
    if (!font) return;
    
    // Free hash table
    for (int i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font->char_table[i];
        while (node) {
            CharNode *next = node->next;
            free(node);
            node = next;
        }
    }
    
    // Free atlas buffer
    free(font->atlas_buffer);
    
    // Free FreeType face
    if (font->face) {
        FT_Done_Face(font->face);
    }
    
    // Free texture
    destroy_texture(&context, &font->texture);
    free(font);
}

float font_height(Font* font) {
    if (!font) return 0;
    return font->ascent + font->descent;
}

float font_width(Font* font) {
    if (!font) return 0;
    Character *space = font_get_character(font, ' ');
    return space ? space->ax : 0;
}

float character_width(Font* font, uint32_t codepoint) {
    if (!font) return 0;
    Character *ch = font_get_character(font, codepoint);
    return ch ? ch->ax : 0;
}


