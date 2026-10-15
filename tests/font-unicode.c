/// font-unicode — tests subsystem
/* CPU atlas proof: no Vulkan context or display is required. */
#include "font.h"
#include <fontconfig/fontconfig.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/// Implementation

int main(void) {
    FT_Library library;
    assert(!FT_Init_FreeType(&library));
    FcPattern *pattern = FcNameParse((const FcChar8 *)"monospace");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcChar8 *file;
    assert(match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch);
    Font font = {.width = 2048, .height = 2048, .display_size = 18};
    assert(!FT_New_Face(library, (const char *)file, 0, &font.face));
    assert(!FT_Set_Pixel_Sizes(font.face, 0, 18));
    FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    font.atlas_buffer = calloc((size_t)font.width * font.height, 4);
    assert(font.atlas_buffer);
    /* A missing codepoint must use a real supporting font, not .notdef. */
    const uint32_t candidates[] = {0x2603, 0x262f, 0x4e2d, 0x1f642};
    uint32_t cp = 0;
    for (size_t i = 0; i < sizeof(candidates)/sizeof(*candidates); i++) {
        if (FT_Get_Char_Index(font.face, candidates[i])) continue;
        FcCharSet *set = FcCharSetCreate();
        FcCharSetAddChar(set, candidates[i]);
        pattern = FcPatternCreate();
        FcPatternAddCharSet(pattern, FC_CHARSET, set);
        FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(NULL, pattern, &result);
        FcCharSet *coverage = NULL;
        bool supported = match && FcPatternGetCharSet(match, FC_CHARSET, 0,
            &coverage) == FcResultMatch && FcCharSetHasChar(coverage, candidates[i]);
        FcPatternDestroy(match); FcPatternDestroy(pattern); FcCharSetDestroy(set);
        if (supported) { cp = candidates[i]; break; }
    }
    assert(cp && "install fonts with Unicode fallback coverage");
    Character *glyph = font_get_character(&font, cp);
    assert(glyph && glyph->bw > 0 && glyph->bh > 0);
    assert(glyph->glyph_index != 0 &&
           "missing Unicode silently rendered .notdef instead of fallback");
    assert(font_get_character(&font, cp) == glyph);
    printf("ok: Unicode U+%04X fallback and atlas cache\n", cp);
    const uint32_t debug_symbols[] = {0x2502, 0x256d, 0x2192, 0x03bb, 0x03a9, 0x221e};
    for (size_t i = 0; i < sizeof(debug_symbols)/sizeof(*debug_symbols); i++) {
        Character *symbol = font_get_character(&font, debug_symbols[i]);
        assert(symbol && symbol->glyph_index && symbol->bw > 0 && symbol->bh > 0);
    }
    Character *unsupported = font_get_character(&font, 0x10ffff);
    assert(unsupported && unsupported->glyph_index);
    assert(font_get_character(&font, 0x10ffff) == unsupported);
    font_clear_fallbacks(&font);
    for (size_t i = 0; i < FONT_HASH_SIZE; i++) {
        CharNode *node = font.char_table[i];
        while (node) { CharNode *next = node->next; free(node); node = next; }
    }
    free(font.atlas_buffer);
    FT_Done_Face(font.face);
    FT_Done_FreeType(library);
    FcFini();
    puts("ok: theme/debugger Unicode symbols and missing-scalar replacement cache");
    return 0;
}
