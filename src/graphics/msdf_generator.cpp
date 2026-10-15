/// msdf_generator.cpp — Upstream msdfgen-backed vector glyph rasterization
/// Dependencies
#include "msdf_generator.h"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>

/// Resource guards
namespace {
struct FreetypeShutdown {
    void operator()(msdfgen::FreetypeHandle *p) const {
        if (p) msdfgen::deinitializeFreetype(p);
    }
};
struct FontShutdown {
    void operator()(msdfgen::FontHandle *p) const {
        if (p) msdfgen::destroyFont(p);
    }
};
static void diagnostic(char *buffer, size_t capacity, const char *message) {
    if (buffer && capacity)
        std::snprintf(buffer, capacity, "%s", message ? message : "unknown MSDF error");
}
static uint8_t to_byte(float value) {
    if (!std::isfinite(value)) return 0;
    const double clamped = std::max(0.0, std::min(1.0, (double)value));
    return (uint8_t)std::lround(clamped * 255.0);
}
}

/// Public C interface
extern "C" ObsMsdfOptions obs_msdf_defaults(void) {
    return ObsMsdfOptions{64, 64, 4.0, 3.0, 5};
}

extern "C" void obs_msdf_release(ObsMsdfBitmap *image) {
    if (!image) return;
    std::free(image->pixels);
    *image = ObsMsdfBitmap{};
}

extern "C" bool obs_msdf_generate_font_glyph(
    const char *font_path, uint32_t unicode, const ObsMsdfOptions *options,
    ObsMsdfBitmap *output, char *error, size_t capacity) {
    if (output) *output = ObsMsdfBitmap{};
    diagnostic(error, capacity, "");
    if (!font_path || !*font_path || !options || !output ||
        unicode > 0x10ffffu || (unicode >= 0xd800u && unicode <= 0xdfffu) ||
        options->width < 8 || options->height < 8 ||
        options->width > 4096 || options->height > 4096 ||
        options->padding < 2 || options->padding >= options->width / 2 ||
        options->padding >= options->height / 2 ||
        !std::isfinite(options->pixel_range) || options->pixel_range < 1.0 ||
        options->pixel_range > (double)options->padding ||
        !std::isfinite(options->corner_angle_radians) ||
        options->corner_angle_radians <= 0 ||
        options->corner_angle_radians > 3.14159265358979323846) {
        diagnostic(error, capacity, "invalid MSDF generation arguments");
        return false;
    }
    try {
        std::unique_ptr<msdfgen::FreetypeHandle, FreetypeShutdown> ft(
            msdfgen::initializeFreetype());
        if (!ft) {
            diagnostic(error, capacity, "FreeType initialization failed");
            return false;
        }
        std::unique_ptr<msdfgen::FontHandle, FontShutdown> font(
            msdfgen::loadFont(ft.get(), font_path));
        if (!font) {
            diagnostic(error, capacity, "cannot open font");
            return false;
        }
        msdfgen::Shape shape;
        if (!msdfgen::loadGlyph(shape, font.get(), unicode,
                                msdfgen::FONT_SCALING_EM_NORMALIZED)) {
            diagnostic(error, capacity, "font does not contain requested outline glyph");
            return false;
        }
        shape.normalize();
        if (shape.contours.empty()) {
            diagnostic(error, capacity, "glyph outline is empty (e.g. whitespace)");
            return false;
        }
        if (!shape.validate()) {
            diagnostic(error, capacity, "glyph shape failed validation");
            return false;
        }
        const msdfgen::Shape::Bounds bounds = shape.getBounds();
        const double l = bounds.l, b = bounds.b;
        const double r = bounds.r, t = bounds.t;
        const double glyph_width = r-l, glyph_height = t-b;
        if (!(glyph_width > 0) || !(glyph_height > 0) ||
            !std::isfinite(glyph_width) || !std::isfinite(glyph_height)) {
            diagnostic(error, capacity, "invalid glyph bounds");
            return false;
        }
        const double usable_width = options->width - 2.0*options->padding;
        const double usable_height = options->height - 2.0*options->padding;
        const double scale = std::min(usable_width/glyph_width,
                                      usable_height/glyph_height);
        if (!(scale > 0) || !std::isfinite(scale)) {
            diagnostic(error, capacity, "invalid glyph scale");
            return false;
        }
        /* Centering is essential when glyphs have different aspect ratios.
         * Shape coordinates and output range use the SAME em/px projection. */
        const double offset_x = (options->width-scale*glyph_width)*0.5;
        const double offset_y = (options->height-scale*glyph_height)*0.5;
        msdfgen::Vector2 translation(offset_x/scale-l, offset_y/scale-b);
        msdfgen::edgeColoringSimple(shape, options->corner_angle_radians);
        msdfgen::Bitmap<float, 3> field((int)options->width,
                                        (int)options->height);
        msdfgen::SDFTransformation transform(
            msdfgen::Projection(msdfgen::Vector2(scale, scale), translation),
            msdfgen::Range(options->pixel_range/scale));
        msdfgen::generateMSDF(field, shape, transform);
        const size_t stride = (size_t)options->width*4;
        const size_t bytes = stride*options->height;
        auto *pixels = static_cast<uint8_t *>(std::malloc(bytes));
        if (!pixels) {
            diagnostic(error, capacity, "MSDF output allocation failed");
            return false;
        }
        /* msdfgen's bitmap origin is bottom-left. Present rows top-down to
         * conventional image consumers; UV orientation is an atlas concern. */
        for (uint32_t y=0; y<options->height; ++y) {
            for (uint32_t x=0; x<options->width; ++x) {
                const auto &rgb = field((int)x, (int)(options->height-1-y));
                uint8_t *dst = pixels + (size_t)y*stride + (size_t)x*4;
                dst[0] = to_byte(rgb[0]);
                dst[1] = to_byte(rgb[1]);
                dst[2] = to_byte(rgb[2]);
                dst[3] = 255;
            }
        }
        output->pixels = pixels;
        output->width = options->width;
        output->height = options->height;
        output->stride = (uint32_t)stride;
        output->em_bounds[0] = l; output->em_bounds[1] = b;
        output->em_bounds[2] = r; output->em_bounds[3] = t;
        output->pixels_per_em = scale;
        output->pixel_range = options->pixel_range;
        return true;
    } catch (const std::exception &ex) {
        diagnostic(error, capacity, ex.what());
    } catch (...) {
        diagnostic(error, capacity, "unknown MSDF generator exception");
    }
    return false;
}
