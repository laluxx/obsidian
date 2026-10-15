/// FontUtf8Tests — host-independent decoder boundary cases
#include "font_utf8.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/// Scalars
static void cases(void) {
    static const struct { const char *text; uint32_t cps[6]; } vectors[] = {
        {"ASCII", {'A','S','C','I','I',0}},
        {"\xc2\xa2", {0xa2,0}},
        {"\xe2\x82\xac", {0x20ac,0}},
        {"\xf0\x9f\x98\x80", {0x1f600,0}},
        {"\xc0\xaf", {0xfffd,0xfffd,0}},
        {"\xed\xa0\x80", {0xfffd,0xfffd,0xfffd,0}},
        {"\xf4\x90\x80\x80", {0xfffd,0xfffd,0xfffd,0xfffd,0}},
        {"\xe2\x82", {0xfffd,0xfffd,0}},
        {"\x80\x80", {0xfffd,0xfffd,0}},
        {"\xf0", {0xfffd,0}},
    };
    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        const unsigned char *p = (const unsigned char *)vectors[i].text;
        size_t count = 0;
        for (;;) {
            const unsigned char *before = p;
            uint32_t cp = obs_font_utf8_next(&p);
            assert(cp == vectors[i].cps[count]);
            if (!cp) break;
            assert(p > before);
            ++count;
            assert(count < 6);
        }
        assert(obs_font_utf8_count(vectors[i].text) == count);
    }
    assert(obs_font_utf8_count(NULL) == 0);
    const unsigned char *none = NULL;
    assert(obs_font_utf8_next(&none) == 0);
}

/// Deterministic malformed input stress
static void fuzz(void) {
    uint32_t state = 0x47a3e219u;
    unsigned char buffer[64];
    for (size_t n = 0; n < 20000; ++n) {
        size_t length = 1 + n % (sizeof(buffer) - 1);
        for (size_t i = 0; i < length; ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            buffer[i] = (unsigned char)(state & 0xff);
            if (!buffer[i]) buffer[i] = 0x80;
        }
        buffer[length] = 0;
        const unsigned char *p = buffer;
        size_t steps = 0;
        while (*p) {
            const unsigned char *previous = p;
            uint32_t cp = obs_font_utf8_next(&p);
            assert(cp > 0 && cp <= 0x10ffff);
            assert(cp < 0xd800 || cp > 0xdfff);
            assert(p > previous && p <= buffer + length);
            ++steps;
        }
        assert(steps == obs_font_utf8_count((const char *)buffer));
    }
}
int main(void) {
    cases(); fuzz();
    puts("ok: utf8 scalar boundaries, malformed input, 20k deterministic fuzz cases");
    return 0;
}
