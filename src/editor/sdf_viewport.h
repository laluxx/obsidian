/// sdf_viewport — owner-thread, opt-in editor tool for real SDF scene gestures
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdf_inspector.h"

/* Screen-to-world unprojection belongs to the real Vulkan gizmo/camera. This
 * core consumes checked world rays so drag state and scene writes can be tested
 * against the production SDF source without mocking an editor or a GPU. */
bool obs_sdf_viewport_enabled(void);
bool obs_sdf_viewport_dragging(void);
bool obs_sdf_viewport_set_enabled(bool enabled);
uint64_t obs_sdf_viewport_drag_ticket(void);
ObsSdfEditStatus obs_sdf_viewport_error(void);
/* Starts a transaction only on an actual, ticketed sphere or axis-aligned box
 * hit. Plane normal points along the camera view direction for the gesture. */
bool obs_sdf_viewport_press(const double origin[3], const double direction[3],
                            const double plane_normal[3], double far_distance);
/* Failure during motion is never silently committed; preview remains live and
 * the previous value unchanged. A no-op or parallel ray is harmless. */
bool obs_sdf_viewport_motion(const double origin[3], const double direction[3]);
/* Both return false if no gesture was active, otherwise consume the event.
 * On a conflict the gesture remains open so Escape can retry after recovery.
 * Scene reset invalidates the gesture without attempting a stale rollback. */
bool obs_sdf_viewport_release(void);
bool obs_sdf_viewport_cancel(void);

/* Called from the real GLFW input poll as well as focus events. A missing
 * release event must not strand a live preview. Focus loss requests cancel,
 * never commit, even when that rollback initially conflicts. Returns true
 * if it consumed a pending event; false if no action was necessary. */
bool obs_sdf_viewport_reconcile(bool left_down, bool focused);
