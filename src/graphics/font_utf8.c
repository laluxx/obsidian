/// FontUtf8 — allocation-free Unicode scalar decoder
#include "font_utf8.h"

/// Text preflight
size_t obs_font_utf8_count(const char *text) {
    if (!text) return 0;
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (*p) { (void)obs_font_utf8_next(&p); ++count; }
    return count;
}
