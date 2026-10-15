#include "middle_orbit.h"
#include "../monad/runtime.h"
#include <math.h>

void obs_middle_orbit_begin(ObsMiddleOrbitDrag *drag, double x, double y) {
    if (!drag) return;
    drag->last_x = x;
    drag->last_y = y;
    drag->active = isfinite(x) && isfinite(y);
}

void obs_middle_orbit_end(ObsMiddleOrbitDrag *drag) {
    if (!drag) return;
    drag->active = false;
}

bool obs_middle_orbit_motion(ObsMiddleOrbitDrag *drag, double x, double y,
                             Camera *cam, vec3 pivot) {
    if (!drag || !drag->active || !cam || !isfinite(x) || !isfinite(y))
        return false;

    const double dx = x - drag->last_x;
    const double dy = y - drag->last_y;
    drag->last_x = x;
    drag->last_y = y;
    if (dx == 0.0 && dy == 0.0) return true;

    float yaw_radians = 0.0f;
    float pitch_radians = 0.0f;
    if (!obs_monad_middle_orbit_delta(dx, false, &yaw_radians) ||
        !obs_monad_middle_orbit_delta(dy, true, &pitch_radians))
        return false;

    camera_orbit_around_point(cam, pivot, yaw_radians, pitch_radians);
    return true;
}
