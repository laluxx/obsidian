/// main.cpp — Standalone MSDF generation and artifact inspection
/// Dependencies
#include "msdf_generator.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <limits>

/// CLI and portable output
static bool parse_codepoint(const char *raw, uint32_t *out) {
    if (!raw || !*raw || !out || *raw == '-') return false;
    errno = 0;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(raw, &end, 0);
    if (errno || *end || parsed > 0x10fffful ||
        (parsed >= 0xd800ul && parsed <= 0xdffful)) return false;
    *out = (uint32_t)parsed;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: obsidian-msdf font.ttf codepoint output.ppm\n"
                             "codepoint: decimal Unicode scalar or 0xHEX\n"
                             "output: P6 RGB, linear UNORM; default 64x64, range 4px\n");
        return 2;
    }
    uint32_t codepoint;
    if (!parse_codepoint(argv[2], &codepoint)) {
        std::fprintf(stderr, "invalid Unicode scalar: %s\n", argv[2]);
        return 2;
    }
    ObsMsdfOptions opt = obs_msdf_defaults();
    ObsMsdfBitmap image{};
    char error[256];
    if (!obs_msdf_generate_font_glyph(argv[1], codepoint, &opt,
                                      &image, error, sizeof(error))) {
        std::fprintf(stderr, "MSDF generation failed: %s\n", error);
        return 1;
    }
    FILE *fp = std::fopen(argv[3], "wb");
    if (!fp) {
        std::fprintf(stderr, "cannot create output: %s\n", argv[3]);
        obs_msdf_release(&image);
        return 1;
    }
    bool ok = std::fprintf(fp, "P6\n%u %u\n255\n", image.width, image.height)>0;
    for (uint32_t y=0; ok && y<image.height; ++y) {
        const uint8_t *row = image.pixels+(size_t)y*image.stride;
        for (uint32_t x=0; ok && x<image.width; ++x)
            ok = std::fwrite(row+(size_t)x*4,1,3,fp)==3;
    }
    if (std::fclose(fp)!=0) ok=false;
    obs_msdf_release(&image);
    if (!ok) std::fprintf(stderr, "failed writing output image\n");
    return ok ? 0 : 1;
}
