#include "font.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* #include "stb_image_write.h" */

FT_Library ft;

static inline uint32_t utf8_decode(const unsigned char **p) {
    uint32_t cp;
    if      ((**p & 0x80) == 0)    { cp = **p;                                                                                    *p += 1; }
    else if ((**p & 0xE0) == 0xC0) { cp = (**p & 0x1F) << 6  | (*(*p+1) & 0x3F);                                                  *p += 2; }
    else if ((**p & 0xF0) == 0xE0) { cp = (**p & 0x0F) << 12 | (*(*p+1) & 0x3F) << 6  | (*(*p+2) & 0x3F);                         *p += 3; }
    else                           { cp = (**p & 0x07) << 18 | (*(*p+1) & 0x3F) << 12 | (*(*p+2) & 0x3F) << 6 | (*(*p+3) & 0x3F); *p += 4; }
    return cp;
}

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

static bool load_glyph(Font* font, uint32_t codepoint, Character *out_char) {
    if (FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER)) {
        fprintf(stderr, "Failed to load glyph U+%04X\n", codepoint);
        return false;
    }

    FT_GlyphSlot glyph = font->face->glyph;

    if (glyph->format != FT_GLYPH_FORMAT_BITMAP) {
        if (FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL)) {
            fprintf(stderr, "Failed to render glyph\n");
            return false;
        }
    }

    unsigned int final_width = glyph->bitmap.width;
    unsigned int final_height = glyph->bitmap.rows;
    unsigned char* final_bitmap = glyph->bitmap.buffer;

    int original_bitmap_left = glyph->bitmap_left;
    int original_bitmap_top = glyph->bitmap_top;

    if (font->atlas_x + final_width + 1 >= font->width) {
        font->atlas_y += font->row_height;
        font->row_height = 0;
        font->atlas_x = 0;
    }

    if (font->atlas_y + final_height >= font->height) {
        if (!grow_atlas(font)) {
            return false;
        }
    }

    for (unsigned int y = 0; y < final_height; y++) {
        for (unsigned int x = 0; x < final_width; x++) {
            int atlas_y = font->atlas_y + y;
            int glyph_y = final_height - 1 - y;
            unsigned char value = final_bitmap[x + glyph_y * final_width];

            int atlas_idx = ((font->atlas_x + x) + (atlas_y * font->width)) * 4;
            font->atlas_buffer[atlas_idx + 0] = 255;
            font->atlas_buffer[atlas_idx + 1] = 255;
            font->atlas_buffer[atlas_idx + 2] = 255;
            font->atlas_buffer[atlas_idx + 3] = value;
        }
    }

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

    return true;
}

// Get or load a character
// Get or load a character
Character* font_get_character(Font* font, uint32_t codepoint) {
    // Check direct-mapped L1 cache first
    uint32_t slot = codepoint & (CHAR_CACHE_SIZE - 1);
    CharCacheEntry *entry = &font->char_cache[slot];
    if (entry->codepoint == codepoint && entry->ch)
        return entry->ch;

    Character *cached = get_cached_character(font, codepoint);
    if (cached) {
        entry->codepoint = codepoint;
        entry->ch = cached;
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
    cached = get_cached_character(font, codepoint);
    entry->codepoint = codepoint;
    entry->ch = cached;
    return cached;
}

// Internal font creation helper
static Font* create_font_internal(FT_Face face, int fontSize) {
    Font* font = (Font*)calloc(1, sizeof(Font));
    if (!font) {
        FT_Done_Face(face);
        return NULL;
    }

    font->face = face;
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

    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

#ifdef DEBUG
    printf("Creating Normal font atlas with SRGB format\n");
#endif

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

#ifdef DEBUG
    printf("[LOADED FONT] %s %i (Normal)\n", fontPath, fontSize);
#endif
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    Font* font = create_font_internal(face, fontSize);
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

        int32_t slot = (int32_t)font->texture.bindlessSlot;
        Vertex2D quad[6] = {
            {{xpos, ypos + h}, color, {u1, v1}, slot},
            {{xpos, ypos}, color, {u1, v2}, slot},
            {{xpos + w, ypos}, color, {u2, v2}, slot},

            {{xpos, ypos + h}, color, {u1, v1}, slot},
            {{xpos + w, ypos}, color, {u2, v2}, slot},
            {{xpos + w, ypos + h}, color, {u2, v1}, slot}
        };

        memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
        vertexCount2D += 6;

        return ch->ax;
}

void text(Font* font, const char* text_str, float x, float y, Color color) {
    if (!font || !text_str) return;

    const unsigned char* p = (const unsigned char*)text_str;
    float initialX = x;

    const float scale = (font->display_size > 0)
                        ? font->display_size / (float)font->ascent
                        : 1.0f;
    const float lineHeight = (font->ascent + font->descent) * scale;
    const float inv_tw    = 1.0f / (float)font->width;
    const float inv_th    = 1.0f / (float)font->height;
    const float descent_f = (float)font->descent;

    int32_t slot = (int32_t)font->texture.bindlessSlot;

    while (*p) {
        if (*p == '\n') {
            x = initialX;
            y -= lineHeight;
            p++;
            continue;
        }

        if ((*p & 0xC0) == 0x80) { p++; continue; }
        uint32_t codepoint = utf8_decode(&p);

        if (codepoint < 32) continue;

        Character *ch = font_get_character(font, codepoint);
        if (!ch) { x += font->ascent * scale; continue; }

        float xpos = x + ch->bl * scale;
        float ypos = y - (ch->bh - ch->bt + descent_f) * scale;
        float w    = ch->bw * scale;
        float h    = ch->bh * scale;

        if (w > 0 && h > 0 && vertexCount2D + 6 <= MAX_VERTICES) {
            float u1 = ch->tx;
            float v1 = ch->ty + ch->bh * inv_th;
            float u2 = ch->tx + ch->bw * inv_tw;
            float v2 = ch->ty;

            Vertex2D *v = &vertices2D[vertexCount2D];
            v[0] = (Vertex2D){{xpos,     ypos + h}, color, {u1, v1}, slot};
            v[1] = (Vertex2D){{xpos,     ypos    }, color, {u1, v2}, slot};
            v[2] = (Vertex2D){{xpos + w, ypos    }, color, {u2, v2}, slot};
            v[3] = (Vertex2D){{xpos,     ypos + h}, color, {u1, v1}, slot};
            v[4] = (Vertex2D){{xpos + w, ypos    }, color, {u2, v2}, slot};
            v[5] = (Vertex2D){{xpos + w, ypos + h}, color, {u2, v1}, slot};
            vertexCount2D += 6;
        }

        x += ch->ax * scale;
    }
}

void text3D(Font* font, const char* text_str, vec3 position, float size, Color color) {
    if (!font || !text_str) return;

    update_texture_if_needed(font);

    const float inv_h  = 1.0f / (float)font->height;
    const float inv_w  = 1.0f / (float)font->width;
    const float scale  = size * inv_h;

    /* ── pass 1: measure total width so we can centre the string ─────── */
    float totalWidth = 0.0f;
    {
        const unsigned char* p = (const unsigned char*)text_str;
        while (*p && *p != '\n') {
            if ((*p & 0xC0) == 0x80) { p++; continue; }
            uint32_t cp = utf8_decode(&p);
            Character* ch = font_get_character(font, cp);
            if (ch) totalWidth += ch->ax * scale;
        }
    }

    /* ── set material once for the whole string ──────────────────────── */
    ImmMaterial mat = {
        .baseColorFactor    = {color.r, color.g, color.b, color.a},
        .metallicFactor     = 0.0f,
        .roughnessFactor    = 1.0f,
        .emissiveStrength   = 1.0f,
        .isUnlit            = 1,
        .emissiveFactor     = {0.0f, 0.0f, 0.0f},
        .albedoIndex        = (int)font->texture.bindlessSlot,
        .normalMapIndex     = -1,
        .metallicRoughIndex = -1,
        .aoIndex            = -1,
        .emissiveIndex      = -1,
    };
    imm_set_material(&mat);

    /* ── allocate ONE SSBO slot for the entire string ────────────────── */
    mat4 identity;
    glm_mat4_identity(identity);
    int shared_slot = imm_alloc_slot(identity);

    /* ── pass 2: build all glyph quads into a contiguous vertex range ── */
    float x = -totalWidth * 0.5f;
    uint32_t string_first = UINT32_MAX;
    uint32_t string_count = 0;

    const unsigned char* p = (const unsigned char*)text_str;
    while (*p && *p != '\n') {
        if ((*p & 0xC0) == 0x80) { p++; continue; }
        uint32_t cp = utf8_decode(&p);

        Character* ch = font_get_character(font, cp);
        if (!ch) { x += font->ascent * scale; continue; }

        float cw   = ch->bw * scale;
        float ch_h = ch->bh * scale;

        if (cw > 0.0f && ch_h > 0.0f) {
            float xpos = position[0] + x + ch->bl * scale;
            float ypos = position[1]
                         - (ch->bh - ch->bt) * scale
                         - font->descent      * scale;
            float zpos = position[2];

            /* UV: atlas stores glyphs top-down; flip V so text reads correctly */
            float u1 = ch->tx;
            float v1 = ch->ty;
            float u2 = ch->tx + ch->bw * inv_w;
            float v2 = ch->ty + ch->bh * inv_h;

            Vertex verts[6] = {
                /* tri 0 */
                {.pos={xpos,      ypos+ch_h, zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u1,v1}, .tangent={1,0,0,1}},
                {.pos={xpos,      ypos,      zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u1,v2}, .tangent={1,0,0,1}},
                {.pos={xpos+cw,   ypos,      zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u2,v2}, .tangent={1,0,0,1}},
                /* tri 1 */
                {.pos={xpos,      ypos+ch_h, zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u1,v1}, .tangent={1,0,0,1}},
                {.pos={xpos+cw,   ypos,      zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u2,v2}, .tangent={1,0,0,1}},
                {.pos={xpos+cw,   ypos+ch_h, zpos}, .color={color.r,color.g,color.b,color.a}, .normal={0,0,1}, .texCoord={u2,v1}, .tangent={1,0,0,1}},
            };

            uint32_t first = imm_append_vertices(verts, 6);
            if (first != UINT32_MAX) {
                if (string_first == UINT32_MAX) string_first = first;
                string_count += 6;
            }
        }

        x += ch->ax * scale;
    }

    /* ── one draw call for the whole string ─────────────────────────── */
    if (string_first != UINT32_MAX && string_count > 0)
        imm_emit_with_slot(string_first, string_count, shared_slot);

    imm_reset_material();
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

        if ((*p & 0xC0) == 0x80) { p++; continue; }
        uint32_t codepoint = utf8_decode(&p);
        x += character(font, codepoint, x, y, color);
    }
}

// Function to measure text width at a specific scale
float measure_text_width(Font* font, const char* text_str, float scale) {
    if (!font || !text_str) return 0.0f;

    float total_width = 0.0f;
    const unsigned char* p = (const unsigned char*)text_str;

    while (*p) {
        if (*p == '\n') break; // Stop at newline

        if ((*p & 0xC0) == 0x80) { p++; continue; }
        uint32_t codepoint = utf8_decode(&p);
        Character *ch = font_get_character(font, codepoint);
        if (ch) total_width += ch->ax * scale;
    }

    return total_width;
}
