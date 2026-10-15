#ifndef OBSIDIAN_GRAPHICS_MSDF_GENERATOR_H
#define OBSIDIAN_GRAPHICS_MSDF_GENERATOR_H

/// Linear-space multi-channel distance textures
/* Optional tooling library: this C-compatible ABI hides msdfgen's C++ types.
 * It is not part of the legacy Vulkan renderer's compilation/linking path.
 * Unlike scene ray-marched SDF, these are 2D vector-glyph distance textures.
 * RGBA8 pixels are linear UNORM (VK_FORMAT_R8G8B8A8_UNORM, never SRGB).
 * RGB contains MSDF; A is opaque, not a true signed-distance channel.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Public interface

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ObsMsdfOptions {
    uint32_t width;
    uint32_t height;
    double pixel_range;
    double corner_angle_radians;
    uint32_t padding;
} ObsMsdfOptions;

typedef struct ObsMsdfBitmap {
    uint8_t *pixels;  /* owned malloc allocation; obs_msdf_release() */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    double em_bounds[4];  /* left, bottom, right, top, before framing */
    double pixels_per_em;
    double pixel_range;
} ObsMsdfBitmap;

/// Generation and lifetime
ObsMsdfOptions obs_msdf_defaults(void);
bool obs_msdf_generate_font_glyph(const char *font_path, uint32_t unicode,
                                  const ObsMsdfOptions *options,
                                  ObsMsdfBitmap *output,
                                  char *diagnostic, size_t diagnostic_capacity);
void obs_msdf_release(ObsMsdfBitmap *image);

#ifdef __cplusplus
}
#endif
#endif
