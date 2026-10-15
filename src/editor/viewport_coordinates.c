/// viewport_coordinates — no GLFW/Vulkan dependency: portable production mapper
#include "viewport_coordinates.h"
#include <float.h>
#include <math.h>

bool obs_viewport_cursor_pixel(double x, double y,
                               int window_width, int window_height,
                               uint32_t render_width, uint32_t render_height,
                               bool allow_outside, ObsViewportPixel *out) {
    if (!out || !isfinite(x) || !isfinite(y) || window_width <= 0 ||
        window_height <= 0 || render_width == 0 || render_height == 0)
        return false;
    const bool inside = x >= 0 && y >= 0 &&
                        x < (double)window_width && y < (double)window_height;
    if (!inside && !allow_outside) return false;
    /* Multiply AFTER dividing; this avoids overflow on large framebuffers.
     * Reject values that become infinite or lose meaningful float precision
     * in the downstream gizmo's float matrix operations. */
    const double px = (x / (double)window_width) * (double)render_width;
    const double py = (y / (double)window_height) * (double)render_height;
    if (!isfinite(px) || !isfinite(py) || fabs(px) > FLT_MAX ||
        fabs(py) > FLT_MAX) return false;
    *out = (ObsViewportPixel){.x = px, .y = py, .inside = inside};
    return true;
}
