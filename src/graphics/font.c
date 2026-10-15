/// font — graphics subsystem
#define _POSIX_C_SOURCE 200809L
#include "font.h"
#include "font_utf8.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fontconfig/fontconfig.h>
#include <time.h>

/// Implementation
/* #include "stb_image_write.h" */

FT_Library ft;

/// Profiling helpers
static uint64_t font_clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
void font_metrics_snapshot(const Font *font, ObsFontMetrics *out) {
    if (out) *out = font ? font->metrics : (ObsFontMetrics){0};
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
    if (!node) return;
    node->codepoint = codepoint;
    node->character = ch;
    node->next = font->char_table[hash];
    font->char_table[hash] = node;
}

// Grow the atlas to accommodate more glyphs
/// Atlas growth
/* The old allocation and descriptor stay valid until the replacement image
 * has been created. Existing GPU command buffers still require fence-based
 * retirement in the renderer; we never pretend that is solved here. */
bool grow_atlas(Font* font) {
    if (!font || !font->height || font->height >= 16384u) return false;
    unsigned new_height = font->height * 2u;
    if (new_height > 16384u) return false;
    unsigned char *replacement_pixels = calloc((size_t)font->width * new_height, 4u);
    if (!replacement_pixels) return false;
    for (unsigned y = 0; y < font->height; ++y)
        memcpy(replacement_pixels + (size_t)y * font->width * 4u,
               font->atlas_buffer + (size_t)y * font->width * 4u,
               (size_t)font->width * 4u);
    Texture2D replacement_texture = {0};
    if (!load_texture_from_rgba_with_format(&context, replacement_pixels,
            font->width, new_height, &replacement_texture,
            VK_FORMAT_R8G8B8A8_SRGB)) {
        free(replacement_pixels);
        return false;
    }
    unsigned old_height = font->height;
    unsigned char *old_pixels = font->atlas_buffer;
    Texture2D old_texture = font->texture;
    font->atlas_buffer = replacement_pixels;
    font->height = new_height;
    font->texture = replacement_texture;
    const float v_scale = (float)old_height / (float)new_height;
    for (unsigned i = 0; i < FONT_HASH_SIZE; ++i)
        for (CharNode *node = font->char_table[i]; node; node = node->next)
            node->character.ty *= v_scale;
    free(old_pixels);
    destroy_texture(&context, &old_texture);
    font->needs_update = false;
    if (font->profile_enabled) ++font->metrics.atlas_grows;
    return true;
}

struct FontFallback {
    FT_Face face;
    char *file;
    int index;
    struct FontFallback *next;
};

void font_clear_fallbacks(Font *font) {
    while (font && font->fallbacks) {
        FontFallback *node = font->fallbacks;
        font->fallbacks = node->next;
        FT_Done_Face(node->face);
        free(node->file);
        free(node);
    }
}

static FT_Face font_face_for_character(Font *font, uint32_t cp) {
    if (FT_Get_Char_Index(font->face, cp)) return font->face;
    for (FontFallback *node = font->fallbacks; node; node = node->next)
        if (FT_Get_Char_Index(node->face, cp)) return node->face;
    FcCharSet *charset = FcCharSetCreate();
    FcPattern *pattern = FcPatternCreate();
    if (!charset || !pattern) {
        if (charset) FcCharSetDestroy(charset);
        if (pattern) FcPatternDestroy(pattern);
        return NULL;
    }
    FcCharSetAddChar(charset, cp);
    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)"monospace");
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcChar8 *file = NULL;
    int index = 0;
    FT_Face face = NULL;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        bool existing = false;
        for (FontFallback *node = font->fallbacks; node; node = node->next)
            if (node->index == index && strcmp(node->file, (const char *)file) == 0)
                existing = true;
        if (!existing && !FT_New_Face(font->face->glyph->library,
                                     (const char *)file, index, &face)) {
            if (FT_Set_Pixel_Sizes(face, 0, font->display_size) ||
                !FT_Get_Char_Index(face, cp)) {
                FT_Done_Face(face);
                face = NULL;
            } else {
                FontFallback *node = calloc(1, sizeof(*node));
                char *path = strdup((const char *)file);
                if (!node || !path) {
                    free(node); free(path); FT_Done_Face(face); face = NULL;
                } else {
                    *node = (FontFallback){face, path, index, font->fallbacks};
                    font->fallbacks = node;
                }
            }
        }
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    FcCharSetDestroy(charset);
    return face;
}

static bool load_glyph(Font* font, uint32_t codepoint, Character *out_char) {
    FT_Face face = font_face_for_character(font, codepoint);
    if (!face || FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) {
        fprintf(stderr, "Failed to load glyph U+%04X\n", codepoint);
        return false;
    }

    FT_GlyphSlot glyph = face->glyph;

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

    if (final_width >= font->width || final_height >= 16384u) {
        fprintf(stderr, "Glyph exceeds font atlas size\n");
        return false;
    }
    if ((unsigned)font->atlas_x + final_width + 1 >= font->width) {
        font->atlas_y += font->row_height;
        font->row_height = 0;
        font->atlas_x = 0;
    }

    while ((unsigned)font->atlas_y + final_height >= font->height) {
        if (!grow_atlas(font)) return false;
    }

    for (unsigned int y = 0; y < final_height; y++) {
        for (unsigned int x = 0; x < final_width; x++) {
            int atlas_y = font->atlas_y + y;
            int glyph_y = final_height - 1 - y;
            /* FreeType rows may be padded or use a negative pitch. */
            int pitch = glyph->bitmap.pitch;
            unsigned int row = pitch < 0 ? y : (unsigned int)glyph_y;
            const unsigned char *pixels = final_bitmap + row * (size_t)abs(pitch);
            unsigned char value;
            if (glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                value = (pixels[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
            else if (glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA)
                value = pixels[x * 4 + 3];
            else
                value = pixels[x];

            int atlas_idx = ((font->atlas_x + x) + (atlas_y * font->width)) * 4;
            font->atlas_buffer[atlas_idx + 0] = 255;
            font->atlas_buffer[atlas_idx + 1] = 255;
            font->atlas_buffer[atlas_idx + 2] = 255;
            font->atlas_buffer[atlas_idx + 3] = value;
        }
    }

    if (font->profile_enabled) ++font->metrics.glyph_rasterizations;
    out_char->glyph_index = glyph->glyph_index;
    out_char->ax = glyph->advance.x / 64.0f;
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

/// Character cache and fallback
Character* font_get_character(Font* font, uint32_t codepoint) {
    if (!font) return NULL;
    // Check direct-mapped L1 cache first
    uint32_t slot = codepoint & (CHAR_CACHE_SIZE - 1);
    CharCacheEntry *entry = &font->char_cache[slot];
    if (entry->codepoint == codepoint && entry->ch) {
        if (font->profile_enabled) ++font->metrics.glyph_l1_hits;
        return entry->ch;
    }

    Character *cached = get_cached_character(font, codepoint);
    if (cached) {
        if (font->profile_enabled) ++font->metrics.glyph_hash_hits;
        entry->codepoint = codepoint;
        entry->ch = cached;
        return cached;
    }

    if (font->profile_enabled) ++font->metrics.glyph_misses;
    uint32_t requested = codepoint;
    Character new_char;
    if (!load_glyph(font, codepoint, &new_char)) {
        codepoint = 0xFFFD;
        cached = get_cached_character(font, codepoint);
        if (cached) new_char = *cached;
        else if (!load_glyph(font, codepoint, &new_char)) {
            codepoint = '?';
            cached = get_cached_character(font, codepoint);
            if (cached) new_char = *cached;
            else if (!load_glyph(font, codepoint, &new_char)) return NULL;
        }
    }

    cache_character(font, requested, new_char);
    cached = get_cached_character(font, requested);
    entry->codepoint = requested;
    entry->ch = cached;
    return cached;
}

// Internal font creation helper
/// create_font_internal — implementation group
static Font* create_font_internal(FT_Face face, int fontSize) {
    Font* font = (Font*)calloc(1, sizeof(Font));
    if (!font) {
        FT_Done_Face(face);
        return NULL;
    }

    font->face = face;
    const char *profile_setting = getenv("OBSIDIAN_FONT_PROFILE");
    font->profile_enabled = profile_setting && strcmp(profile_setting, "1") == 0;
    font->ascent = face->size->metrics.ascender >> 6;
    font->descent = -(face->size->metrics.descender >> 6);
    // Most editor fonts at the supported zoom range fit comfortably in a
    // 1024² atlas.  Starting here cuts the resident allocation from 16 MiB
    // to 4 MiB per font; the existing growth path handles larger glyph sets.
    font->width = 1024;
    font->height = 1024;
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

    printf("[LOADED FONT] %s %i (Normal)\n", fontPath, fontSize);
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    Font* font = create_font_internal(face, fontSize);
    if (font) {
        font->display_size = fontSize;
    }
    return font;
}

/// Editor font selection
/* The old generated MapleMono header was absent from the distributed source.
 * Prefer a local user-provided font when available, then Fontconfig. Fontconfig
 * selects a real, scalable file: it does not synthesize font bytes. */
Font *load_editor_font(int fontSize) {
    const char *custom = getenv("OBSIDIAN_EDITOR_FONT");
    if (custom && *custom) {
        Font *font = load_font(custom, fontSize);
        if (font) return font;
        fprintf(stderr, "Obsidian: configured font could not be opened: %s\n", custom);
    }
    const char *bundled = "assets/fonts/MapleMono-NF-Regular.ttf";
    FILE *file = fopen(bundled, "rb");
    if (file) {
        fclose(file);
        Font *font = load_font(bundled, fontSize);
        if (font) return font;
    }
    if (!FcInit()) return NULL;
    FcPattern *pattern = FcPatternCreate();
    if (!pattern) return NULL;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)"monospace");
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return NULL;
    FcChar8 *path = NULL;
    Font *font = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch && path)
        font = load_font((const char *)path, fontSize);
    FcPatternDestroy(match);
    return font;
}

Font* load_font_from_memory(const unsigned char* font_data, size_t data_size, int fontSize) {
    FT_Face face;
    // Load the font directly from the compiled-in byte array
    if (FT_New_Memory_Face(ft, font_data, (FT_Long)data_size, 0, &face)) {
        fprintf(stderr, "Failed to load embedded font from memory\n");
        return NULL;
    }

    printf("[LOADED FONT] (Embedded Memory) %i (Normal)\n", fontSize);
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    Font* font = create_font_internal(face, fontSize);
    if (font) {
        font->display_size = fontSize;
    }
    return font;
}

static void update_texture_if_needed(Font* font) {
    if (!font->needs_update) return;

    uint64_t start = font->profile_enabled ? font_clock_ns() : 0;
    if (update_texture_from_rgba(&context, &font->texture,
                                 font->atlas_buffer, font->width, font->height)) {
        font->needs_update = false;
        if (font->profile_enabled) ++font->metrics.atlas_uploads;
    }
    if (start) font->metrics.upload_cpu_ns += font_clock_ns() - start;
}

void font_flush_updates(Font* font) {
    update_texture_if_needed(font);
}

float character(Font* font, uint32_t codepoint, float x, float y, Color color) {
    return character_ex(font, codepoint, x, y, color, NULL);
}

float character_ex(Font* font, uint32_t codepoint, float x, float y,
                   Color color, const ObsFaceShaderData *face_data) {
    return character_ex_clipped(font, codepoint, x, y, color, face_data,
                                renderer2D_clip_rect);
}

float character_ex_clipped(Font* font, uint32_t codepoint, float x, float y,
                           Color color, const ObsFaceShaderData *face_data,
                           vec4 clip_rect) {
    if (!font) return 0.0f;

    if (codepoint == '\n') {
        return 0.0f;
    }

    if (codepoint < 32) {
        return 0.0f;
    }

    Character *ch = font_get_character(font, codepoint);
    if (!ch) return font->ascent;
    /* Individual glyph rendering must not reference an unuploaded atlas. */
    if (font->needs_update) font_flush_updates(font);
    if (font->needs_update) return ch->ax;

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

    if (!renderer2D_reserve(6)) {
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

        for (size_t i = 0; i < 6; i++)
            memcpy(quad[i].clipRect, clip_rect, sizeof(quad[i].clipRect));

        if (obs_face_shader_data_requires_payload(face_data)) {
            for (size_t i = 0; i < 6; i++) {
                memcpy(quad[i].faceData0, face_data, sizeof(face_data->shader_id) * 4);
                memcpy(quad[i].faceData1, &face_data->run_offset, sizeof(face_data->run_offset) * 4);
                memcpy(quad[i].faceEventData, &face_data->event_offset,
                       sizeof(face_data->event_offset) * 4);
                memcpy(quad[i].faceParams, face_data->params, sizeof(face_data->params));
                memcpy(quad[i].faceEvent, face_data->event, sizeof(face_data->event));
            }
        }

        memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
        vertexCount2D += 6;
        if (font->profile_enabled) ++font->metrics.emitted_glyphs;
        return ch->ax;
}

/// Text preflight
/* Resolve the complete string before calculating normalized UVs: a missing
 * glyph can expand the atlas. One batched upload follows, never an upload
 * per glyph. This is still a synchronous legacy upload; asynchronous dirty
 * rectangles are tracked separately in TODO.org. */
static bool font_prepare_text(Font *font, const char *text) {
    if (!font || !text) return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = obs_font_utf8_next(&p);
        if (cp >= 32) (void)font_get_character(font, cp);
    }
    if (font->needs_update) font_flush_updates(font);
    return !font->needs_update;
}

void text(Font* font, const char* text_str, float x, float y, Color color) {
    if (!font || !text_str) return;
    uint64_t start = font->profile_enabled ? font_clock_ns() : 0;
    if (!font_prepare_text(font, text_str)) return;
    if (font->profile_enabled) ++font->metrics.text_calls;

    const unsigned char* p = (const unsigned char*)text_str;
    float initialX = x;

    const float scale = (font->display_size > 0 && font->ascent > 0)
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

                uint32_t codepoint = obs_font_utf8_next(&p);

        if (codepoint < 32) continue;

        Character *ch = font_get_character(font, codepoint);
        if (!ch) { x += font->ascent * scale; continue; }

        float xpos = x + ch->bl * scale;
        float ypos = y - (ch->bh - ch->bt + descent_f) * scale;
        float w    = ch->bw * scale;
        float h    = ch->bh * scale;

        if (w > 0 && h > 0 && renderer2D_reserve(6)) {
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
            if (font->profile_enabled) ++font->metrics.emitted_glyphs;
        }

        x += ch->ax * scale;
    }
    if (start) font->metrics.text_cpu_ns += font_clock_ns() - start;
}

void text3D(Font* font, const char* text_str, vec3 position, float size, Color color) {
    if (!font || !text_str) return;

    if (!font_prepare_text(font, text_str)) return;

    const float inv_h  = 1.0f / (float)font->height;
    const float inv_w  = 1.0f / (float)font->width;
    const float scale  = size * inv_h;

    /* ── pass 1: measure total width so we can centre the string ─────── */
    float totalWidth = 0.0f;
    {
        const unsigned char* p = (const unsigned char*)text_str;
        while (*p && *p != '\n') {
                        uint32_t cp = obs_font_utf8_next(&p);
            Character* ch = font_get_character(font, cp);
            if (ch) totalWidth += ch->ax * scale;
        }
    }

    /* ── set material once for the whole string ──────────────────────── */
    Material mat = {
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
    set_material(&mat);

    /* ── allocate ONE SSBO slot for the entire string ────────────────── */
    mat4 identity;
    glm_mat4_identity(identity);
    int shared_slot = alloc_slot(identity);

    /* ── pass 2: build all glyph quads into a contiguous vertex range ── */
    float x = -totalWidth * 0.5f;
    uint32_t string_first = UINT32_MAX;
    uint32_t string_count = 0;

    const unsigned char* p = (const unsigned char*)text_str;
    while (*p && *p != '\n') {
                uint32_t cp = obs_font_utf8_next(&p);

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

            uint32_t first = append_vertices(verts, 6);
            if (first != UINT32_MAX) {
                if (string_first == UINT32_MAX) string_first = first;
                string_count += 6;
            }
        }

        x += ch->ax * scale;
    }

    /* ── one draw call for the whole string ─────────────────────────── */
    if (string_first != UINT32_MAX && string_count > 0)
        emit_draw_with_slot(string_first, string_count, shared_slot);

    reset_material();
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
    if (font->profile_enabled) {
        const ObsFontMetrics *m = &font->metrics;
        fprintf(stderr,
            "[obsidian/font/profile] size=%d text=%llu glyphs=%llu "
            "l1=%llu hash=%llu miss=%llu raster=%llu "
            "uploads=%llu grows=%llu text_cpu_ms=%.3f upload_cpu_ms=%.3f\n",
            font->display_size,
            (unsigned long long)m->text_calls,
            (unsigned long long)m->emitted_glyphs,
            (unsigned long long)m->glyph_l1_hits,
            (unsigned long long)m->glyph_hash_hits,
            (unsigned long long)m->glyph_misses,
            (unsigned long long)m->glyph_rasterizations,
            (unsigned long long)m->atlas_uploads,
            (unsigned long long)m->atlas_grows,
            m->text_cpu_ns/1.0e6, m->upload_cpu_ns/1.0e6);
    }

    for (int i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font->char_table[i];
        while (node) {
            CharNode *next = node->next;
            free(node);
            node = next;
        }
    }

    free(font->atlas_buffer);
    font_clear_fallbacks(font);

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

                uint32_t codepoint = obs_font_utf8_next(&p);
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

                uint32_t codepoint = obs_font_utf8_next(&p);
        Character *ch = font_get_character(font, codepoint);
        if (ch) total_width += ch->ax * scale;
    }

    return total_width;
}
