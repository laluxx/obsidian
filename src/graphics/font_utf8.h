/// FontUtf8 — bounded Unicode scalar scanning for NUL-terminated UI strings
#ifndef OBSIDIAN_FONT_UTF8_H
#define OBSIDIAN_FONT_UTF8_H

#include <stdint.h>
#include <stddef.h>

/// Public interface
/* Returns the next scalar, U+FFFD for malformed sequences, or 0 at the
 * terminator. On invalid input consumes exactly one byte, so callers never
 * read beyond an incomplete sequence or get stuck on malformed input. */
static inline uint32_t obs_font_utf8_next(const unsigned char **cursor) {
    if (!cursor || !*cursor || !**cursor) return 0;
    const unsigned char *p = *cursor;
    uint32_t cp = p[0];
    if (cp < 0x80) {
        *cursor = p + 1;
        return cp;
    }
    /* Read each continuation only after proving the previous byte exists.
     * Reject overlong forms, UTF-16 surrogates and >U+10FFFF. */
    unsigned needed;
    uint32_t min_value;
    if (cp >= 0xc2 && cp <= 0xdf) {
        needed = 2; cp &= 0x1f; min_value = 0x80;
    } else if (cp >= 0xe0 && cp <= 0xef) {
        needed = 3; cp &= 0x0f; min_value = 0x800;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
        needed = 4; cp &= 0x07; min_value = 0x10000;
    } else goto invalid;
    for (unsigned i = 1; i < needed; ++i) {
        if (!p[i] || (p[i] & 0xc0) != 0x80) goto invalid;
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    if (cp < min_value || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) goto invalid;
    *cursor = p + needed;
    return cp;
invalid:
    *cursor = p + 1;
    return 0xfffd;
}

/* A read-only preflight for profiling and hot text paths. Includes malformed
 * sequences as one replacement scalar each. Newlines count as scalars. */
size_t obs_font_utf8_count(const char *text);

#endif
