/// font — graphics subsystem
#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "renderer.h"
#include "common.h"

/// Public interface

typedef struct {
    uint32_t glyph_index; // actual FreeType glyph, never a Unicode scalar
    float ax;  // advance.x
    float ay;  // advance.y
    float bw;  // bitmap.width
    float bh;  // bitmap.rows
    float bl;  // bitmap_left
    float bt;  // bitmap_top
    float tx;  // x offset of glyph in texture coordinates
    float ty;  // y offset of glyph in texture coordinates
} Character;

// Hash table for character storage
#define FONT_HASH_SIZE 256
typedef struct CharNode {
    uint32_t codepoint;
    Character character;
    struct CharNode *next;
} CharNode;

// Direct-mapped L1 character cache (power of 2 for fast modulo)
#define CHAR_CACHE_SIZE 256
typedef struct {
    uint32_t codepoint;
    Character *ch;
} CharCacheEntry;

typedef struct FontFallback FontFallback;

/// Optional performance counters
/* The hot path updates these only with OBSIDIAN_FONT_PROFILE=1. Timings are
 * wall clock CPU time, NOT GPU frame time. All access is engine-thread only. */
typedef struct ObsFontMetrics {
    uint64_t glyph_l1_hits;
    uint64_t glyph_hash_hits;
    uint64_t glyph_misses;
    uint64_t glyph_rasterizations;
    uint64_t atlas_uploads;
    uint64_t atlas_grows;
    uint64_t text_calls;
    uint64_t text_cpu_ns;
    uint64_t upload_cpu_ns;
    uint64_t emitted_glyphs;
} ObsFontMetrics;

typedef struct {
    FontFallback *fallbacks;      // owned faces, same pixel size as primary
    Texture2D texture;           // Vulkan texture atlas
    unsigned int width;          // width of the texture atlas
    unsigned int height;         // height of the texture atlas
    int ascent;                  // font ascent
    int descent;                 // font descent
    float underline_position;
    float underline_thickness;

    // Dynamic character storage
    CharNode *char_table[FONT_HASH_SIZE];

    // Atlas packing state
    int atlas_x;
    int atlas_y;
    int row_height;

    // FreeType face for loading new glyphs
    FT_Face face;
    unsigned char *atlas_buffer;  // Keep buffer for dynamic updates
    bool needs_update;            // Flag to reupload texture

    int display_size;
    CharCacheEntry char_cache[CHAR_CACHE_SIZE];
    ObsFontMetrics metrics;
    bool profile_enabled;
} Font;

void init_free_type(void);
Font *load_font(const char *fontPath, int fontSize);
/* Optional project font -> configured font -> system monospace. No generated
 * header or particular licensed font is required for a clean source checkout. */
Font *load_editor_font(int fontSize);
Font *load_font_from_memory(const unsigned char *font_data, size_t data_size, int fontSize);
void destroy_font(Font* font);

void text(Font* font, const char* text, float x, float y, Color color);
void text_with_size(Font* font, const char* text, float x, float y, Color color, float size);
void text3D(Font* font, const char* text_str, vec3 position, float size, Color color);
void fps(Font* font, float x, float y, Color color);

float font_height(Font* font);
float font_width(Font* font);
float character_width(Font* font, uint32_t codepoint);

Character* font_get_character(Font* font, uint32_t codepoint);
void font_flush_updates(Font* font);
bool save_font_atlas_png(Font* font, const char* filename);
bool grow_atlas(Font* font);
float character(Font* font, uint32_t codepoint, float x, float y, Color color);
float character_ex(Font* font, uint32_t codepoint, float x, float y,
                   Color color, const ObsFaceShaderData *face_data);
float character_ex_clipped(Font* font, uint32_t codepoint, float x, float y,
                           Color color, const ObsFaceShaderData *face_data,
                           vec4 clip_rect);
float measure_text_width(Font* font, const char* text_str, float scale);

void font_clear_fallbacks(Font *font);
/* Snapshot the current opt-in counters without mutating the font. */
void font_metrics_snapshot(const Font *font, ObsFontMetrics *out);
