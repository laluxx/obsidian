/// viewport_coordinates — checked GLFW window-space to Vulkan render-pixel mapping
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double x, y;                 /* framebuffer pixels, origin at top left */
    bool inside;                 /* whether logical cursor is in the window */
} ObsViewportPixel;

/* GLFW cursor positions use logical window coordinates. Render and depth
 * operations use drawable pixels. This mapping is deliberately based on the
 * render target dimensions, not an assumed integer OS content scale.
 * Passing allow_outside permits a captured drag to continue outside the
 * window; a new pick must set it false. No clamping (clamping causes jumps). */
bool obs_viewport_cursor_pixel(double x, double y,
                               int window_width, int window_height,
                               uint32_t render_width, uint32_t render_height,
                               bool allow_outside, ObsViewportPixel *out);
