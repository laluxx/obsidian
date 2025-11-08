#pragma once

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "renderer.h"
#include "common.h"

#define ASCII 128  // Supporting basic ASCII

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

typedef struct {
    Texture2D texture;    // Vulkan texture atlas
    unsigned int width;   // width of the texture atlas
    unsigned int height;  // height of the texture atlas
    int ascent;           // font ascent
    int descent;          // font descent
    Character characters[ASCII];
} Font;

void init_free_type(void);
Font* load_font(const char* fontPath, int fontSize);
void destroy_font(Font* font);

float character(Font* font, unsigned char c, float x, float y, Color color);
void text(Font* font, const char* text, float x, float y, Color color);
void text3D(Font* font, const char* text_str, vec3 position, float size, Color color);

float font_height(Font* font);
float font_width(Font* font);
float character_width(Font* font, char character);
