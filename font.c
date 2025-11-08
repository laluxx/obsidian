#include "font.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// TODO SDF
// TODO Option to save font atlas

FT_Library ft;

void init_free_type() {
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "Could not init FreeType Library\n");
        exit(1);
    }
}

Font* load_font(const char* fontPath, int fontSize) {
    FT_Face face;
    if (FT_New_Face(ft, fontPath, 0, &face)) {
        fprintf(stderr, "Failed to load font: %s\n", fontPath);
        return NULL;
    }

    printf("[LOADED FONT] %s %i\n", fontPath, fontSize);
    FT_Set_Pixel_Sizes(face, 0, fontSize);

    Font* font = (Font*)malloc(sizeof(Font));
    if (!font) {
        FT_Done_Face(face);
        fprintf(stderr, "Memory allocation failed for font structure\n");
        return NULL;
    }

    // Save ascent and descent
    font->ascent = face->size->metrics.ascender >> 6;
    font->descent = -(face->size->metrics.descender >> 6);

    // Create atlas buffer (RGBA for Vulkan)
    font->width = 1024;
    font->height = 1024;
    unsigned char* atlas = (unsigned char*)calloc(font->width * font->height * 4, sizeof(unsigned char));
    if (!atlas) {
        free(font);
        FT_Done_Face(face);
        fprintf(stderr, "Memory allocation failed for texture atlas\n");
        return NULL;
    }

    // Pack glyphs into atlas
    int ox = 0, oy = 0, rowh = 0;
    for (int i = 32; i < ASCII; i++) {
        if (FT_Load_Char(face, i, FT_LOAD_RENDER)) {
            fprintf(stderr, "Loading character %c failed!\n", i);
            continue;
        }

        if (ox + face->glyph->bitmap.width + 1 >= font->width) {
            oy += rowh;
            rowh = 0;
            ox = 0;
        }

        // Copy glyph to atlas (convert grayscale to RGBA)
        for (int y = 0; y < face->glyph->bitmap.rows; y++) {
            for (int x = 0; x < face->glyph->bitmap.width; x++) {
                int atlas_y = oy + y;
                int glyph_y = face->glyph->bitmap.rows - 1 - y; // Flip vertically
                unsigned char value = face->glyph->bitmap.buffer[x + glyph_y * face->glyph->bitmap.pitch];
                
                int atlas_idx = ((ox + x) + (atlas_y * font->width)) * 4;
                atlas[atlas_idx + 0] = 255;      // R
                atlas[atlas_idx + 1] = 255;      // G
                atlas[atlas_idx + 2] = 255;      // B
                atlas[atlas_idx + 3] = value;    // A
            }
        }

        // Store character info
        font->characters[i].ax = face->glyph->advance.x >> 6;
        font->characters[i].ay = face->glyph->advance.y >> 6;
        font->characters[i].bw = face->glyph->bitmap.width;
        font->characters[i].bh = face->glyph->bitmap.rows;
        font->characters[i].bl = face->glyph->bitmap_left;
        font->characters[i].bt = face->glyph->bitmap_top;
        font->characters[i].tx = ox / (float)font->width;
        font->characters[i].ty = oy / (float)font->height;

        rowh = fmax(rowh, face->glyph->bitmap.rows);
        ox += face->glyph->bitmap.width + 1;
    }

    // Create Vulkan texture
    if (!load_texture_from_rgba(&context, atlas, font->width, font->height, &font->texture)) {
        fprintf(stderr, "Failed to create Vulkan texture from font atlas\n");
        free(atlas);
        free(font);
        FT_Done_Face(face);
        return NULL;
    }

    free(atlas);
    FT_Done_Face(face);
    return font;
}

float character(Font* font, unsigned char c, float x, float y, Color color) {
    if (!font) return 0.0f;
    
    // Handle newline (don't draw, just return advance)
    if (c == '\n') {
        return 0.0f;
    }
    
    // Skip non-printable characters
    if (c < 32 || c >= ASCII) {
        return 0.0f;
    }
    
    Character ch = font->characters[c];
    
    // Calculate position (align baseline)
    float xpos = x + ch.bl;
    float ypos = y - (ch.bh - ch.bt + font->descent);
    
    float w = ch.bw;
    float h = ch.bh;
    
    // Skip if no visible pixels, but still return advance
    if (w == 0 || h == 0) {
        return ch.ax;
    }
    
    // Calculate UV coordinates
    float u1 = ch.tx;
    float v1 = ch.ty + ch.bh / (float)font->height;
    float u2 = ch.tx + ch.bw / (float)font->width;
    float v2 = ch.ty;
    
    // Check vertex buffer space
    if (vertexCount2D + 6 > MAX_VERTICES) {
        fprintf(stderr, "Vertex buffer full, cannot render character\n");
        return ch.ax;
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
        // Continue existing batch
        batchIndex = textureBatchCount - 1;
    } else {
        // Create new batch
        if (textureBatchCount >= MAX_TEXTURES) {
            fprintf(stderr, "Too many texture batches!\n");
            return ch.ax;
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
    
    // Return the advance width for cursor positioning
    return ch.ax;
}

void text(Font* font, const char* text_str, float x, float y, Color color) {
    if (!font || !text_str) return;

    const unsigned char* p;
    float initialX = x;
    float lineHeight = (font->ascent + font->descent);

    for (p = (const unsigned char*)text_str; *p; p++) {
        if (*p == '\n') {
            x = initialX;
            y -= lineHeight;
            continue;
        }

        if (*p < 32 || *p >= (unsigned char)ASCII) continue;

        Character ch = font->characters[*p];

        // Calculate position (align baseline)
        float xpos = x + ch.bl;
        float ypos = y - (ch.bh - ch.bt + font->descent);

        float w = ch.bw;
        float h = ch.bh;

        // Skip if no visible pixels
        if (w == 0 || h == 0) {
            x += ch.ax;
            continue;
        }

        // Calculate UV coordinates
        float u1 = ch.tx;
        float v1 = ch.ty + ch.bh / (float)font->height;
        float u2 = ch.tx + ch.bw / (float)font->width;
        float v2 = ch.ty;

        // Check vertex buffer space
        if (vertexCount2D + 6 > MAX_VERTICES) {
            fprintf(stderr, "Vertex buffer full, cannot render text\n");
            return;
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
            // Continue existing batch
            batchIndex = textureBatchCount - 1;
        } else {
            // Create new batch
            if (textureBatchCount >= MAX_TEXTURES) {
                fprintf(stderr, "Too many texture batches!\n");
                return;
            }
            batchIndex = textureBatchCount++;
            textureBatches[batchIndex].texture = &font->texture;
            // Textured vertices always start after colored vertices
            textureBatches[batchIndex].startVertex = coloredVertexCount + (vertexCount2D - coloredVertexCount);
            textureBatches[batchIndex].vertexCount = 0;
        }

        // Add vertices at the end of the buffer (after colored section)
        memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
        vertexCount2D += 6;
        textureBatches[batchIndex].vertexCount += 6;

        // Advance cursor
        x += ch.ax;
    }
}

void text3D(Font* font, const char* text_str, vec3 position, float size, Color color) {
    if (!font || !text_str) return;

    // Calculate total text width for centering
    float totalWidth = 0.0f;
    int charCount = 0;
    const unsigned char* p;
    for (p = (const unsigned char*)text_str; *p && *p != '\n'; p++) {
        if (*p < 32 || *p >= (unsigned char)ASCII) continue;
        totalWidth += font->characters[*p].ax * size / (float)font->height;
        charCount++;
    }

    // Center the text
    float startX = -totalWidth / 2.0f;
    float x = startX;

    // Render characters in reverse order to compensate for texture flip
    for (int i = charCount - 1; i >= 0; i--) {
        const unsigned char ch_char = ((const unsigned char*)text_str)[i];
        if (ch_char < 32 || ch_char >= (unsigned char)ASCII) continue;

        Character ch = font->characters[ch_char];

        // Calculate character dimensions in 3D space
        float charWidth = ch.bw * size / (float)font->height;
        float charHeight = ch.bh * size / (float)font->height;
        
        // Position relative to baseline
        float xpos = x + ch.bl * size / (float)font->height;
        float ypos = -(ch.bh - ch.bt) * size / (float)font->height - font->descent * size / (float)font->height;

        // Skip if no visible pixels
        if (ch.bw == 0 || ch.bh == 0) {
            x += ch.ax * size / (float)font->height;
            continue;
        }

        // Check vertex buffer space
        if (vertex_count_3D_textured + 6 > MAX_VERTICES) {
            fprintf(stderr, "3D textured vertex buffer full, cannot render text\n");
            return;
        }

        // Calculate UV coordinates
        float u1 = ch.tx;
        float v1 = ch.ty + ch.bh / (float)font->height;
        float u2 = ch.tx + ch.bw / (float)font->width;
        float v2 = ch.ty;

        Vertex quad[6] = {
            // Triangle 1
            {
                .pos = {position[0] + xpos, position[1] + ypos + charHeight, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u2, v1}  // Flipped: u1 -> u2
            },
            {
                .pos = {position[0] + xpos, position[1] + ypos, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u2, v2}  // Flipped: u1 -> u2
            },
            {
                .pos = {position[0] + xpos + charWidth, position[1] + ypos, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u1, v2}  // Flipped: u2 -> u1
            },
            
            // Triangle 2
            {
                .pos = {position[0] + xpos, position[1] + ypos + charHeight, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u2, v1}  // Flipped: u1 -> u2
            },
            {
                .pos = {position[0] + xpos + charWidth, position[1] + ypos, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u1, v2}  // Flipped: u2 -> u1
            },
            {
                .pos = {position[0] + xpos + charWidth, position[1] + ypos + charHeight, position[2]},
                .color = {color.r, color.g, color.b, color.a},
                .normal = {0.0f, 0.0f, 1.0f},
                .texCoord = {u1, v1}  // Flipped: u2 -> u1
            }
        };
        
        // Find or create batch for this texture
        int batchIndex = -1;
        if (texture3DBatchCount > 0 && texture3DBatches[texture3DBatchCount - 1].texture == &font->texture) {
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
        
        // Add vertices to 3D textured buffer
        memcpy(&vertices3D_textured[vertex_count_3D_textured], quad, sizeof(quad));
        vertex_count_3D_textured += 6;
        texture3DBatches[batchIndex].vertexCount += 6;
        
        // Advance cursor (moving forward since we're rendering backwards)
        x += ch.ax * size / (float)font->height;
    }
}

void destroy_font(Font* font) {
    if (font) {
        destroy_texture(&context, &font->texture);
        free(font);
    }
}

float font_height(Font* font) {
    if (!font) return 0;
    float max_height = 0;
    for (int i = 0; i < ASCII; i++) {
        if (font->characters[i].bh > max_height) {
            max_height = font->characters[i].bh;
        }
    }
    return max_height;
}

float font_width(Font* font) {
    if (!font) return 0;
    return font->characters[' '].ax;
}

float character_width(Font* font, char character) {
    if (!font || character < 0 || (unsigned char)character >= ASCII) {
        return 0;
    }
    return font->characters[(unsigned char)character].ax;
}
