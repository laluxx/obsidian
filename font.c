#include "font.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* #include "stb_image_write.h" */

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

// Edge point for distance calculation
typedef struct {
    float x, y;
} EdgePoint;

// Grid cell for spatial partitioning
typedef struct {
    int* edge_indices;
    int count;
    int capacity;
} GridCell;

// Fast, high-quality SDF using distance transform
// This gives the distinctive smooth SDF look efficiently

static void generate_sdf(unsigned char* bitmap, int width, int height,
                        unsigned char* sdf_output, int sdf_width, int sdf_height,
                        int spread) {

    printf("  [SDF] Generating %dx%d -> %dx%d, spread=%d\n",
           width, height, sdf_width, sdf_height, spread);

    float radius = (float)spread;

    // Step 1: For each output pixel, compute EXACT distance to nearest edge
    // This is the key: we check EVERY pixel, not just edges

    for (int out_y = 0; out_y < sdf_height; out_y++) {
        for (int out_x = 0; out_x < sdf_width; out_x++) {
            // Map to input coordinates
            float center_x = (float)(out_x - spread) + 0.5f;
            float center_y = (float)(out_y - spread) + 0.5f;

            // Sample the center point to determine inside/outside
            bool is_inside = false;
            if (center_x >= 0 && center_x < width && center_y >= 0 && center_y < height) {
                int cx = (int)center_x;
                int cy = (int)center_y;
                if (cx >= 0 && cx < width && cy >= 0 && cy < height) {
                    is_inside = bitmap[cx + cy * width] > 127;
                }
            }

            // Find minimum distance to opposite value
            // This is the CRITICAL part: we search ALL nearby pixels
            float min_distance = radius * 2.0f; // Start with max possible

            // Search in a square region around this pixel
            int search_radius = (int)ceilf(radius) + 2;

            for (int sy = -search_radius; sy <= search_radius; sy++) {
                for (int sx = -search_radius; sx <= search_radius; sx++) {
                    int sample_x = (int)center_x + sx;
                    int sample_y = (int)center_y + sy;

                    // Skip out of bounds
                    if (sample_x < 0 || sample_x >= width ||
                        sample_y < 0 || sample_y >= height) {
                        // Treat out of bounds as "outside"
                        if (is_inside) {
                            float dx = sx;
                            float dy = sy;
                            float dist = sqrtf(dx * dx + dy * dy);
                            if (dist < min_distance) {
                                min_distance = dist;
                            }
                        }
                        continue;
                    }

                    bool sample_inside = bitmap[sample_x + sample_y * width] > 127;

                    // We only care about pixels with OPPOSITE state
                    if (sample_inside != is_inside) {
                        float dx = sx;
                        float dy = sy;
                        float dist = sqrtf(dx * dx + dy * dy);

                        if (dist < min_distance) {
                            min_distance = dist;
                        }
                    }
                }
            }

            // Clamp distance to radius
            min_distance = fminf(min_distance, radius);

            // Normalize to 0-1 range
            float normalized = min_distance / radius;

            // Encode as SDF:
            // Inside: 0.5 to 1.0 (0.5 at edge, 1.0 at center)
            // Outside: 0.0 to 0.5 (0.0 far away, 0.5 at edge)
            float alpha;
            if (is_inside) {
                alpha = 0.5f + (normalized * 0.5f);
            } else {
                alpha = 0.5f - (normalized * 0.5f);
            }

            alpha = fmaxf(0.0f, fminf(1.0f, alpha));
            sdf_output[out_x + out_y * sdf_width] = (unsigned char)(alpha * 255.0f);
        }
    }

    printf("  [SDF] Complete\n");
}

// Grow the atlas to accommodate more glyphs
bool grow_atlas(Font* font) {
    unsigned int new_width = font->width;
    unsigned int new_height = font->height * 2;
    unsigned int old_height = font->height;

    if (new_height > 16384) {
        fprintf(stderr, "Font atlas reached maximum size (16384x16384)!\n");
        return false;
    }

    if (new_height >= 8192) {
        fprintf(stderr, "Warning: Font atlas is getting very large (%ux%u)\n",
                new_width, new_height);
    }

    fprintf(stderr, "Growing font atlas from %ux%u to %ux%u\n",
            font->width, font->height, new_width, new_height);

    unsigned char *new_buffer = (unsigned char*)calloc(new_width * new_height * 4, sizeof(unsigned char));
    if (!new_buffer) {
        fprintf(stderr, "Failed to allocate memory for larger atlas\n");
        return false;
    }

    for (unsigned int y = 0; y < font->height; y++) {
        memcpy(new_buffer + (y * new_width * 4),
               font->atlas_buffer + (y * font->width * 4),
               font->width * 4);
    }

    free(font->atlas_buffer);
    font->atlas_buffer = new_buffer;
    font->height = new_height;

    float height_scale = (float)old_height / (float)new_height;

    for (int i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font->char_table[i];
        while (node) {
            node->character.ty *= height_scale;
            node = node->next;
        }
    }

    destroy_texture(&context, &font->texture);
    if (!load_texture_from_rgba(&context, font->atlas_buffer, font->width, font->height, &font->texture)) {
        fprintf(stderr, "Failed to recreate texture after atlas growth\n");
        return false;
    }

    font->needs_update = false;
    return true;
}

/* bool save_font_atlas_png(Font* font, const char* filename) { */
/*     if (!font || !font->atlas_buffer) { */
/*         fprintf(stderr, "Cannot save atlas: invalid font\n"); */
/*         return false; */
/*     } */

/*     // Convert RGBA to grayscale (just use alpha channel for SDF) */
/*     unsigned char* grayscale = malloc(font->width * font->height); */

/*     for (unsigned int i = 0; i < font->width * font->height; i++) { */
/*         grayscale[i] = font->atlas_buffer[i * 4 + 3]; // Alpha channel */
/*     } */

/*     int result = stbi_write_png(filename, font->width, font->height, 1, */
/*                                  grayscale, font->width); */

/*     free(grayscale); */

/*     if (result) { */
/*         printf("Saved font atlas to %s (%ux%u)\n", filename, font->width, font->height); */
/*         return true; */
/*     } else { */
/*         fprintf(stderr, "Failed to save atlas to %s\n", filename); */
/*         return false; */
/*     } */
/* } */

// Add debug output to load_glyph to verify SDF generation

// Fix 2: Proper monochrome loading for SDF, grayscale for normal
static bool load_glyph(Font* font, uint32_t codepoint, Character *out_char) {
    FT_Int32 load_flags;

    if (font->render_mode == FONT_RENDER_SDF) {
        // SDF: Load as 1-bit monochrome for clean edges
        load_flags = FT_LOAD_TARGET_MONO;
    } else {
        // Normal: Load with antialiasing
        load_flags = FT_LOAD_RENDER;
    }

    if (FT_Load_Char(font->face, codepoint, load_flags)) {
        fprintf(stderr, "Failed to load glyph U+%04X\n", codepoint);
        return false;
    }

    FT_GlyphSlot glyph = font->face->glyph;

    // Render if needed
    if (glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        FT_Render_Mode render_mode = (font->render_mode == FONT_RENDER_SDF)
                                      ? FT_RENDER_MODE_MONO
                                      : FT_RENDER_MODE_NORMAL;
        if (FT_Render_Glyph(glyph, render_mode)) {
            fprintf(stderr, "Failed to render glyph\n");
            return false;
        }
    }

    unsigned char* temp_bitmap = NULL;
    int bitmap_width = glyph->bitmap.width;
    int bitmap_height = glyph->bitmap.rows;
    unsigned char* source_bitmap = glyph->bitmap.buffer;

    // Convert monochrome to 8-bit for SDF processing
    if (font->render_mode == FONT_RENDER_SDF && glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        int pitch = abs(glyph->bitmap.pitch);
        temp_bitmap = (unsigned char*)malloc(bitmap_width * bitmap_height);

        for (int y = 0; y < bitmap_height; y++) {
            for (int x = 0; x < bitmap_width; x++) {
                int byte_index = y * pitch + (x >> 3);
                int bit_index = 7 - (x & 7);
                unsigned char bit = (glyph->bitmap.buffer[byte_index] >> bit_index) & 1;
                temp_bitmap[x + y * bitmap_width] = bit ? 255 : 0;
            }
        }
        source_bitmap = temp_bitmap;
    }

    unsigned int final_width = bitmap_width;
    unsigned int final_height = bitmap_height;
    unsigned char* final_bitmap = source_bitmap;
    unsigned char* sdf_bitmap = NULL;

    int original_bitmap_left = glyph->bitmap_left;
    int original_bitmap_top = glyph->bitmap_top;

    // Generate SDF if needed
    if (font->render_mode == FONT_RENDER_SDF && bitmap_width > 0 && bitmap_height > 0) {
        final_width = bitmap_width + font->sdf_spread * 2;
        final_height = bitmap_height + font->sdf_spread * 2;

        sdf_bitmap = (unsigned char*)malloc(final_width * final_height);
        generate_sdf(source_bitmap, bitmap_width, bitmap_height, sdf_bitmap,
                     final_width, final_height, font->sdf_spread);

        final_bitmap = sdf_bitmap;

        // Adjust metrics for padding
        original_bitmap_left -= font->sdf_spread;
        original_bitmap_top += font->sdf_spread;
    }

    // Check atlas space
    if (font->atlas_x + final_width + 1 >= font->width) {
        font->atlas_y += font->row_height;
        font->row_height = 0;
        font->atlas_x = 0;
    }

    if (font->atlas_y + final_height >= font->height) {
        if (!grow_atlas(font)) {
            if (sdf_bitmap) free(sdf_bitmap);
            if (temp_bitmap) free(temp_bitmap);
            return false;
        }
    }

    // Copy to atlas
    for (unsigned int y = 0; y < final_height; y++) {
        for (unsigned int x = 0; x < final_width; x++) {
            int atlas_y = font->atlas_y + y;
            int glyph_y = final_height - 1 - y; // Flip vertically
            unsigned char value = final_bitmap[x + glyph_y * final_width];

            int atlas_idx = ((font->atlas_x + x) + (atlas_y * font->width)) * 4;
            font->atlas_buffer[atlas_idx + 0] = 255;
            font->atlas_buffer[atlas_idx + 1] = 255;
            font->atlas_buffer[atlas_idx + 2] = 255;
            font->atlas_buffer[atlas_idx + 3] = value;
        }
    }

    // Store character metrics
    out_char->ax = glyph->advance.x >> 6;
    out_char->ay = glyph->advance.y >> 6;
    out_char->bw = final_width;
    out_char->bh = final_height;
    out_char->bl = original_bitmap_left;
    out_char->bt = original_bitmap_top;
    out_char->tx = font->atlas_x / (float)font->width;
    out_char->ty = font->atlas_y / (float)font->height;

    font->row_height = fmax(font->row_height, final_height);
    font->atlas_x += final_width + 1;
    font->needs_update = true;

    if (sdf_bitmap) free(sdf_bitmap);
    if (temp_bitmap) free(temp_bitmap);

    return true;
}

// Get or load a character
Character* font_get_character(Font* font, uint32_t codepoint) {
    Character *cached = get_cached_character(font, codepoint);
    if (cached) {
        return cached;
    }

    Character new_char;
    if (!load_glyph(font, codepoint, &new_char)) {
        codepoint = 0xFFFD;
        cached = get_cached_character(font, codepoint);
        if (cached) return cached;

        if (!load_glyph(font, codepoint, &new_char)) {
            codepoint = ' ';
            cached = get_cached_character(font, codepoint);
            if (cached) return cached;
            return NULL;
        }
    }

    cache_character(font, codepoint, new_char);
    return get_cached_character(font, codepoint);
}

// Internal font creation helper
static Font* create_font_internal(FT_Face face, int fontSize, FontRenderMode mode, int sdf_spread) {
    Font* font = (Font*)calloc(1, sizeof(Font));
    if (!font) {
        FT_Done_Face(face);
        return NULL;
    }

    font->face = face;
    font->render_mode = mode;
    font->sdf_spread = sdf_spread;
    font->ascent = face->size->metrics.ascender >> 6;
    font->descent = -(face->size->metrics.descender >> 6);
    font->width = 2048;
    font->height = 2048;
    font->atlas_x = 0;
    font->atlas_y = 0;
    font->row_height = 0;
    font->needs_update = false;

    if (face->underline_position && face->underline_thickness) {
        font->underline_position = -(face->underline_position * fontSize) / (float)face->units_per_EM;
        if (font->underline_position < 0) font->underline_position = -font->underline_position;
        font->underline_thickness = (face->underline_thickness * fontSize) / (float)face->units_per_EM;
        if (font->underline_thickness < 1.0f) font->underline_thickness = 1.0f;
    } else {
        font->underline_position = 0;
        font->underline_thickness = 0;
    }

    memset(font->char_table, 0, sizeof(font->char_table));

    font->atlas_buffer = (unsigned char*)calloc(font->width * font->height * 4, sizeof(unsigned char));
    if (!font->atlas_buffer) {
        free(font);
        FT_Done_Face(face);
        return NULL;
    }

    // Pre-load ASCII
    for (uint32_t i = 32; i < 127; i++) {
        Character ch;
        if (load_glyph(font, i, &ch)) {
            cache_character(font, i, ch);
        }
    }

    // Load common Unicode
    uint32_t common_chars[] = {
        0xFFFD, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
    };
    for (size_t i = 0; i < sizeof(common_chars) / sizeof(common_chars[0]); i++) {
        Character ch;
        if (load_glyph(font, common_chars[i], &ch)) {
            cache_character(font, common_chars[i], ch);
        }
    }

    // CRITICAL: Different formats for different font types
    // SDF fonts MUST use UNORM (linear), normal fonts can use SRGB
    VkFormat format = (mode == FONT_RENDER_SDF)
                      ? VK_FORMAT_R8G8B8A8_UNORM   // Linear for distance data
                      : VK_FORMAT_R8G8B8A8_SRGB;   // sRGB for color data

    printf("Creating %s font atlas with %s format\n",
           mode == FONT_RENDER_SDF ? "SDF" : "Normal",
           format == VK_FORMAT_R8G8B8A8_UNORM ? "UNORM" : "SRGB");

    if (!load_texture_from_rgba_with_format(&context, font->atlas_buffer,
                                            font->width, font->height,
                                            &font->texture, format)) {
        fprintf(stderr, "Failed to create font atlas texture\n");
        free(font->atlas_buffer);
        free(font);
        FT_Done_Face(face);
        return NULL;
    }

    font->needs_update = false;
    return font;
}

Font* load_font(const char* fontPath, int fontSize) {
    FT_Face face;
    if (FT_New_Face(ft, fontPath, 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", fontPath);
        return NULL;
    }

    printf("[LOADED FONT] %s %i (Normal)\n", fontPath, fontSize);
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    return create_font_internal(face, fontSize, FONT_RENDER_NORMAL, 0);
}

Font* load_font_sdf(const char* fontPath, int fontSize, int spread) {
    FT_Face face;
    if (FT_New_Face(ft, fontPath, 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", fontPath);
        return NULL;
    }

    // Render at HIGHER resolution for better SDF quality
    // But not TOO high or you lose the benefit
    int render_size = fontSize * 3;  // 3x is a good balance

    printf("[LOADED FONT] %s render_size=%d (display=%d) (SDF, spread=%d)\n",
           fontPath, render_size, fontSize, spread);

    FT_Set_Pixel_Sizes(face, 0, render_size);

    // DON'T multiply spread here - use it as-is
    Font* font = create_font_internal(face, render_size, FONT_RENDER_SDF, spread);

    if (font) {
        font->display_size = fontSize;
    }

    return font;
}

static void update_texture_if_needed(Font* font) {
    if (!font->needs_update) return;

    update_texture_from_rgba(&context, &font->texture, font->atlas_buffer,
                            font->width, font->height);

    font->needs_update = false;
}

void font_flush_updates(Font* font) {
    update_texture_if_needed(font);
}

float character(Font* font, uint32_t codepoint, float x, float y, Color color) {
    if (!font) return 0.0f;

    if (codepoint == '\n') {
        return 0.0f;
    }

    if (codepoint < 32) {
        return 0.0f;
    }

    Character *ch = font_get_character(font, codepoint);
    if (!ch) {
        return font->ascent;
    }

    float xpos = x + ch->bl;
    float ypos = y - (ch->bh - ch->bt + font->descent);

    float w = ch->bw;
    float h = ch->bh;

    if (w == 0 || h == 0) {
        return ch->ax;
    }

    float u1 = ch->tx;
    float v1 = ch->ty + ch->bh / (float)font->height;
    float u2 = ch->tx + ch->bw / (float)font->width;
    float v2 = ch->ty;

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

    int batchIndex = -1;

    if (textureBatchCount > 0) {
        TextureBatch* lastBatch = &textureBatches[textureBatchCount - 1];
        bool same_texture = (lastBatch->texture == &font->texture);
        bool same_mode = (lastBatch->is_sdf == (font->render_mode == FONT_RENDER_SDF));

        if (same_texture && same_mode) {
            batchIndex = textureBatchCount - 1;
        }
    }

    if (batchIndex == -1) {
        if (textureBatchCount >= MAX_TEXTURES) {
            fprintf(stderr, "Too many texture batches!\n");
            return ch->ax;
        }
        batchIndex = textureBatchCount++;
        textureBatches[batchIndex].texture = &font->texture;
        textureBatches[batchIndex].startVertex = coloredVertexCount + (vertexCount2D - coloredVertexCount);
        textureBatches[batchIndex].vertexCount = 0;
        textureBatches[batchIndex].is_sdf = (font->render_mode == FONT_RENDER_SDF);
    }

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
    textureBatches[batchIndex].vertexCount += 6;

    return ch->ax;
}

/* float character(Font* font, uint32_t codepoint, float x, float y, Color color, float scale) { */
/*     if (!font) return 0.0f; */

/*     if (codepoint == '\n') { */
/*         return 0.0f; */
/*     } */

/*     if (codepoint < 32) { */
/*         return 0.0f; */
/*     } */

/*     Character *ch = font_get_character(font, codepoint); */
/*     if (!ch) { */
/*         return font->ascent * scale;  // SCALE THE FALLBACK TOO */
/*     } */

/*     // APPLY SCALE TO ALL METRICS */
/*     float xpos = x + ch->bl * scale; */
/*     float ypos = y - (ch->bh - ch->bt + font->descent) * scale; */

/*     float w = ch->bw * scale;  // SCALED WIDTH */
/*     float h = ch->bh * scale;  // SCALED HEIGHT */

/*     if (w == 0 || h == 0) { */
/*         return ch->ax * scale;  // SCALED ADVANCE */
/*     } */

/*     // Texture coordinates stay the same - they reference the atlas */
/*     float u1 = ch->tx; */
/*     float v1 = ch->ty + ch->bh / (float)font->height; */
/*     float u2 = ch->tx + ch->bw / (float)font->width; */
/*     float v2 = ch->ty; */

/*     if (vertexCount2D + 6 > MAX_VERTICES) { */
/*         fprintf(stderr, "Vertex buffer full, cannot render character\n"); */
/*         return ch->ax * scale; */
/*     } */

/*     // Render quad at SCALED size */
/*     Vertex2D quad[6] = { */
/*         {{xpos, ypos + h}, color, {u1, v1}, 0}, */
/*         {{xpos, ypos}, color, {u1, v2}, 0}, */
/*         {{xpos + w, ypos}, color, {u2, v2}, 0}, */

/*         {{xpos, ypos + h}, color, {u1, v1}, 0}, */
/*         {{xpos + w, ypos}, color, {u2, v2}, 0}, */
/*         {{xpos + w, ypos + h}, color, {u2, v1}, 0} */
/*     }; */

/*     int batchIndex = -1; */

/*     if (textureBatchCount > 0) { */
/*         TextureBatch* lastBatch = &textureBatches[textureBatchCount - 1]; */
/*         bool same_texture = (lastBatch->texture == &font->texture); */
/*         bool same_mode = (lastBatch->is_sdf == (font->render_mode == FONT_RENDER_SDF)); */

/*         if (same_texture && same_mode) { */
/*             batchIndex = textureBatchCount - 1; */
/*         } */
/*     } */

/*     if (batchIndex == -1) { */
/*         if (textureBatchCount >= MAX_TEXTURES) { */
/*             fprintf(stderr, "Too many texture batches!\n"); */
/*             return ch->ax * scale; */
/*         } */
/*         batchIndex = textureBatchCount++; */
/*         textureBatches[batchIndex].texture = &font->texture; */
/*         textureBatches[batchIndex].startVertex = coloredVertexCount + (vertexCount2D - coloredVertexCount); */
/*         textureBatches[batchIndex].vertexCount = 0; */
/*         textureBatches[batchIndex].is_sdf = (font->render_mode == FONT_RENDER_SDF); */
/*     } */

/*     memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad)); */
/*     vertexCount2D += 6; */
/*     textureBatches[batchIndex].vertexCount += 6; */

/*     return ch->ax * scale;  // RETURN SCALED ADVANCE */
/* } */

/* float character(Font* font, uint32_t codepoint, float x, float y, Color color) { */
/*     if (!font) return 0.0f; */

/*     if (codepoint == '\n') { */
/*         return 0.0f; */
/*     } */

/*     if (codepoint < 32) { */
/*         return 0.0f; */
/*     } */

/*     Character *ch = font_get_character(font, codepoint); */
/*     if (!ch) { */
/*         return font->ascent; */
/*     } */

/*     // Calculate position using the character metrics */
/*     float xpos = x + ch->bl; */
/*     float ypos = y - (ch->bh - ch->bt + font->descent); */

/*     float w = ch->bw; */
/*     float h = ch->bh; */

/*     if (w == 0 || h == 0) { */
/*         return ch->ax; */
/*     } */

/*     // Texture coordinates - use the actual stored dimensions */
/*     float u1 = ch->tx; */
/*     float v1 = ch->ty + ch->bh / (float)font->height; */
/*     float u2 = ch->tx + ch->bw / (float)font->width; */
/*     float v2 = ch->ty; */

/*     if (vertexCount2D + 6 > MAX_VERTICES) { */
/*         fprintf(stderr, "Vertex buffer full, cannot render character\n"); */
/*         return ch->ax; */
/*     } */

/*     Vertex2D quad[6] = { */
/*         {{xpos, ypos + h}, color, {u1, v1}, 0}, */
/*         {{xpos, ypos}, color, {u1, v2}, 0}, */
/*         {{xpos + w, ypos}, color, {u2, v2}, 0}, */

/*         {{xpos, ypos + h}, color, {u1, v1}, 0}, */
/*         {{xpos + w, ypos}, color, {u2, v2}, 0}, */
/*         {{xpos + w, ypos + h}, color, {u2, v1}, 0} */
/*     }; */

/*     int batchIndex = -1; */

/*     // CRITICAL: Check if we can reuse the last batch */
/*     // Must match BOTH texture AND sdf mode */
/*     if (textureBatchCount > 0) { */
/*         TextureBatch* lastBatch = &textureBatches[textureBatchCount - 1]; */
/*         bool same_texture = (lastBatch->texture == &font->texture); */
/*         bool same_mode = (lastBatch->is_sdf == (font->render_mode == FONT_RENDER_SDF)); */

/*         if (same_texture && same_mode) { */
/*             batchIndex = textureBatchCount - 1; */
/*         } */
/*     } */

/*     // Create new batch if needed */
/*     if (batchIndex == -1) { */
/*         if (textureBatchCount >= MAX_TEXTURES) { */
/*             fprintf(stderr, "Too many texture batches!\n"); */
/*             return ch->ax; */
/*         } */
/*         batchIndex = textureBatchCount++; */
/*         textureBatches[batchIndex].texture = &font->texture; */
/*         textureBatches[batchIndex].startVertex = coloredVertexCount + (vertexCount2D - coloredVertexCount); */
/*         textureBatches[batchIndex].vertexCount = 0; */

/*         // CRITICAL: Set the SDF flag based on font render mode */
/*         textureBatches[batchIndex].is_sdf = (font->render_mode == FONT_RENDER_SDF); */

/*         // Debug output */
/*         printf("Created 2D batch #%d for font: texture=%p, is_sdf=%d\n", */
/*                batchIndex, (void*)&font->texture, textureBatches[batchIndex].is_sdf); */
/*     } */

/*     memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad)); */
/*     vertexCount2D += 6; */
/*     textureBatches[batchIndex].vertexCount += 6; */

/*     return ch->ax; */
/* } */



void text(Font* font, const char* text_str, float x, float y, Color color) {
    if (!font || !text_str) return;

    const unsigned char* p = (const unsigned char*)text_str;
    float initialX = x;

    // Scale factor if using high-res rendering
    float scale = 1.0f;
    if (font->display_size > 0) {
        // If we rendered at higher res, scale down when drawing
        /* scale = (float)font->display_size / (float)font->ascent; */
        scale = font->display_size / (float)font->ascent;
    }

    float lineHeight = (font->ascent + font->descent) * scale;

    while (*p) {
        if (*p == '\n') {
            x = initialX;
            y -= lineHeight;
            p++;
            continue;
        }

        uint32_t codepoint;
        size_t bytes_read;

        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6;
            codepoint |= (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12;
            codepoint |= (*(p+1) & 0x3F) << 6;
            codepoint |= (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18;
            codepoint |= (*(p+1) & 0x3F) << 12;
            codepoint |= (*(p+2) & 0x3F) << 6;
            codepoint |= (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            p++;
            continue;
        }

        x += character(font, codepoint, x, y, color) * scale;
        p += bytes_read;
    }

    update_texture_if_needed(font);
}



void text3D(Font* font, const char* text_str, vec3 position, float size, Color color) {
    if (!font || !text_str) return;

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

            // CRITICAL: Check both texture AND sdf mode
            if (texture3DBatchCount > 0) {
                Texture3DBatch* lastBatch = &texture3DBatches[texture3DBatchCount - 1];
                bool same_texture = (lastBatch->texture == &font->texture);
                bool same_mode = (lastBatch->is_sdf == (font->render_mode == FONT_RENDER_SDF));

                if (same_texture && same_mode) {
                    batchIndex = texture3DBatchCount - 1;
                }
            }

            // Create new batch if needed
            if (batchIndex == -1) {
                if (texture3DBatchCount >= MAX_TEXTURES) {
                    fprintf(stderr, "Too many 3D texture batches!\n");
                    return;
                }
                batchIndex = texture3DBatchCount++;
                texture3DBatches[batchIndex].texture = &font->texture;
                texture3DBatches[batchIndex].startVertex = vertex_count_3D_textured;
                texture3DBatches[batchIndex].vertexCount = 0;

                // CRITICAL: Set based on font's render mode
                texture3DBatches[batchIndex].is_sdf = (font->render_mode == FONT_RENDER_SDF);

                // Debug output
                /* printf("Created 3D batch #%d for font: texture=%p, is_sdf=%d\n", */
                /*        batchIndex, (void*)&font->texture, texture3DBatches[batchIndex].is_sdf); */
            }

            memcpy(&vertices3D_textured[vertex_count_3D_textured], quad, sizeof(quad));
            vertex_count_3D_textured += 6;
            texture3DBatches[batchIndex].vertexCount += 6;
        }

        x += ch->ax * size / (float)font->height;
        p += bytes_read;
    }

    update_texture_if_needed(font);
}

static double last_fps_time = 0.0;
static uint32_t frame_count = 0;
static float current_fps = 0.0f;
static char fps_text[32] = "FPS: 0";

void fps(Font* font, float x, float y, Color color) {
    if (!font) return;

    double current_time = glfwGetTime();

    if (last_fps_time == 0.0) {
        last_fps_time = current_time;
        return;
    }

    frame_count++;

    double elapsed = current_time - last_fps_time;
    if (elapsed >= 1.0) {
        current_fps = (float)frame_count / (float)elapsed;
        frame_count = 0;
        last_fps_time = current_time;

        snprintf(fps_text, sizeof(fps_text), "FPS: %.0f", current_fps);
    }

    text(font, fps_text, x, y, color);
}

void destroy_font(Font* font) {
    if (!font) return;

    for (int i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font->char_table[i];
        while (node) {
            CharNode *next = node->next;
            free(node);
            node = next;
        }
    }

    free(font->atlas_buffer);

    if (font->face) {
        FT_Done_Face(font->face);
    }

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

void text_with_size(Font* font, const char* text_str, float x, float y, Color color, float size) {
    if (!font || !text_str) return;

    const unsigned char* p = (const unsigned char*)text_str;
    float initialX = x;

    // Calculate scale factor to achieve desired size
    float scale = 1.0f;
    if (font->ascent > 0) {
        scale = size / (float)font->ascent;
    } else if (font->display_size > 0) {
        // Fallback if ascent is not available, though it should be.
        scale = size / (float)font->display_size;
    }

    float lineHeight = (font->ascent + font->descent) * scale;

    while (*p) {
        if (*p == '\n') {
            x = initialX;
            y -= lineHeight;
            p++;
            continue;
        }

        uint32_t codepoint;
        size_t bytes_read;

        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6;
            codepoint |= (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12;
            codepoint |= (*(p+1) & 0x3F) << 6;
            codepoint |= (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18;
            codepoint |= (*(p+1) & 0x3F) << 12;
            codepoint |= (*(p+2) & 0x3F) << 6;
            codepoint |= (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            p++;
            continue;
        }

        x += character(font, codepoint, x, y, color);
        p += bytes_read;
    }

    update_texture_if_needed(font);
}

// Function to measure text width at a specific scale
float measure_text_width(Font* font, const char* text_str, float scale) {
    if (!font || !text_str) return 0.0f;

    float total_width = 0.0f;
    const unsigned char* p = (const unsigned char*)text_str;

    while (*p) {
        if (*p == '\n') break; // Stop at newline

        uint32_t codepoint;
        size_t bytes_read;

        if ((*p & 0x80) == 0) {
            codepoint = *p;
            bytes_read = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6;
            codepoint |= (*(p+1) & 0x3F);
            bytes_read = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12;
            codepoint |= (*(p+1) & 0x3F) << 6;
            codepoint |= (*(p+2) & 0x3F);
            bytes_read = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = (*p & 0x07) << 18;
            codepoint |= (*(p+1) & 0x3F) << 12;
            codepoint |= (*(p+2) & 0x3F) << 6;
            codepoint |= (*(p+3) & 0x3F);
            bytes_read = 4;
        } else {
            p++;
            continue;
        }

        Character *ch = font_get_character(font, codepoint);
        if (ch) {
            total_width += ch->ax * scale;
        }

        p += bytes_read;
    }

    return total_width;
}
