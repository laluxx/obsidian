#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "renderer.h"
#include "common.h"

typedef struct {
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

typedef enum {
    FONT_RENDER_NORMAL,  // Uses regular texture pipeline
    FONT_RENDER_SDF      // Uses SDF pipeline
} FontRenderMode;

typedef struct {
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

    FontRenderMode render_mode;
    int sdf_spread;
    int display_size;
} Font;

void init_free_type(void);
Font *load_font(const char *fontPath, int fontSize);
Font* load_font_sdf(const char* fontPath, int fontSize, int spread);
void destroy_font(Font* font);

// UPDATED SIGNATURE - now takes scale parameter
void text(Font* font, const char* text, float x, float y, Color color);
void text_with_size(Font* font, const char* text, float x, float y, Color color, float size);
void text3D(Font* font, const char* text_str, vec3 position, float size, Color color);
void fps(Font* font, float x, float y, Color color);

float font_height(Font* font);
float font_width(Font* font);
float character_width(Font* font, uint32_t codepoint);

// Internal: Get or load a character
Character* font_get_character(Font* font, uint32_t codepoint);
void font_flush_updates(Font* font);
bool save_font_atlas_png(Font* font, const char* filename);
bool grow_atlas(Font* font);
void diagnose_sdf_font(Font* font);
float character(Font* font, uint32_t codepoint, float x, float y, Color color);
float measure_text_width(Font* font, const char* text_str, float scale);
