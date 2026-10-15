/// sdf_viewport — actual selection, camera-plane drag and atomic cancellation
#include "sdf_viewport.h"
#include "sdf_sculpt.h"
#include <math.h>
#include <string.h>

typedef struct {
  bool enabled, active, cancel_pending;
  uint64_t ticket;
  double normal[3], initial[3], anchor[3], grab[3];
  ObsSdfEditStatus error;
} ObsViewportDrag;
static ObsViewportDrag tool;

static bool finite3(const double v[3]) {
  return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}
static bool unit(const double in[3], double out[3]) {
  if (!finite3(in))
    return false;
  double n = hypot(hypot(in[0], in[1]), in[2]);
  if (!(n > 1e-12) || !isfinite(n))
    return false;
  for (unsigned i = 0; i < 3; ++i)
    out[i] = in[i] / n;
  return true;
}
static bool plane_hit(const double origin[3], const double dir[3],
                      const double point[3], const double normal[3],
                      double result[3]) {
  double ray[3];
  if (!finite3(origin) || !unit(dir, ray))
    return false;
  double denom = ray[0] * normal[0] + ray[1] * normal[1] + ray[2] * normal[2];
  if (!isfinite(denom) || fabs(denom) < 1e-8)
    return false;
  double t =
      ((point[0] - origin[0]) * normal[0] + (point[1] - origin[1]) * normal[1] +
       (point[2] - origin[2]) * normal[2]) /
      denom;
  if (!isfinite(t) || t < 0)
    return false;
  for (unsigned i = 0; i < 3; ++i)
    result[i] = origin[i] + ray[i] * t;
  return finite3(result);
}
bool obs_sdf_viewport_enabled(void) { return tool.enabled; }
bool obs_sdf_viewport_dragging(void) { return tool.active; }
uint64_t obs_sdf_viewport_drag_ticket(void) {
  return tool.active ? tool.ticket : 0;
}
ObsSdfEditStatus obs_sdf_viewport_error(void) { return tool.error; }
bool obs_sdf_viewport_set_enabled(bool enabled) {
  if (!enabled && tool.active) {
    obs_sdf_viewport_cancel();
    if (tool.active)
      return false;
  }
  tool.enabled = enabled;
  return true;
}
bool obs_sdf_viewport_press(const double origin[3], const double direction[3],
                            const double plane_normal[3], double far_distance) {
  if (!tool.enabled || tool.active || !finite3(origin) || !finite3(direction) ||
      !isfinite(far_distance) || far_distance <= 0)
    return false;
  double n[3], initial[3], grab[3];
  if (!unit(plane_normal, n))
    return false;
  ObsSculptRayHit hit;
  if (!obs_sculpt_ray_cast_primitive(origin, direction, far_distance, &hit) ||
      !obs_sculpt_position_any(hit.ticket, initial) ||
      !plane_hit(origin, direction, initial, n, grab))
    return false;
  tool.error = obs_sdf_inspector_begin_group();
  if (tool.error != OBS_SDF_EDIT_OK)
    return false;
  tool.error = obs_sdf_inspector_select(hit.ticket);
  if (tool.error != OBS_SDF_EDIT_OK) {
    (void)obs_sdf_inspector_abort_group();
    return false;
  }
  tool.active = true;
  tool.cancel_pending = false;
  tool.ticket = hit.ticket;
  memcpy(tool.normal, n, sizeof(n));
  memcpy(tool.initial, initial, sizeof(initial));
  memcpy(tool.anchor, initial, sizeof(initial));
  memcpy(tool.grab, grab, sizeof(grab));
  return true;
}
bool obs_sdf_viewport_motion(const double origin[3],
                             const double direction[3]) {
  if (!tool.active)
    return false;
  double point[3], target[3];
  double unit_direction[3];
  if (!finite3(origin) || !unit(direction, unit_direction)) {
    tool.error = OBS_SDF_EDIT_INVALID;
    return true;
  }
  if (!plane_hit(origin, unit_direction, tool.anchor, tool.normal, point))
    return true;
  for (unsigned i = 0; i < 3; ++i)
    target[i] = tool.initial[i] + point[i] - tool.grab[i];
  if (!finite3(target)) {
    tool.error = OBS_SDF_EDIT_INVALID;
    return true;
  }
  tool.error = obs_sdf_inspector_drag_move(target);
  return true;
}
static bool finish(bool cancel) {
  if (!tool.active)
    return false;
  /* Never accept a partially failed pointer trajectory. A later valid
   * motion can recover the gesture; Escape can always attempt rollback. */
  if (!cancel && tool.error != OBS_SDF_EDIT_OK)
    return true;
  tool.error =
      cancel ? obs_sdf_inspector_abort_group() : obs_sdf_inspector_end_group();
  if (tool.error == OBS_SDF_EDIT_OK || tool.error == OBS_SDF_EDIT_INVALID ||
      tool.error == OBS_SDF_EDIT_STALE) {
    tool.active = false;
    tool.cancel_pending = false;
    tool.ticket = 0;
  }
  return true;
}
/* A cancelled capture cannot subsequently commit from a late GLFW release.
 * Conflict refusal leaves the cancellation intent latched until the user
 * repairs the conflicting live state or resets the scene. */
bool obs_sdf_viewport_release(void) { return finish(tool.cancel_pending); }
bool obs_sdf_viewport_cancel(void) {
  if (tool.active)
    tool.cancel_pending = true;
  return finish(true);
}
bool obs_sdf_viewport_reconcile(bool left_down, bool focused) {
  if (!tool.active)
    return false;
  if (!focused)
    tool.cancel_pending = true;
  if (tool.cancel_pending)
    return obs_sdf_viewport_cancel();
  if (!left_down)
    return obs_sdf_viewport_release();
  return false;
}
