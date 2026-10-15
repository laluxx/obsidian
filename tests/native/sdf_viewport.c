/* Cycle 54: source-bound transactional inspector gestures (no shadow scene). */
#define _POSIX_C_SOURCE 200809L
#include "sdf_viewport.h"
#include "agent_engine_contract.h"
#include "sdf_inspector.h"
#include "sdf_sculpt.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];
void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name, int32_t parent,
                            int32_t mesh_index) {
  assert(name && parent >= 0 && mesh_index == -1);
  if (tree->count >= SCENE_TREE_MAX_NODES)
    return -1;
  int32_t index = tree->count++;
  tree->nodes[index].sdf_index = -1;
  return index;
}

static uint64_t a, b;
static void fresh(void) {
  free(scene->sdfs);
  scene->sdfs = NULL;
  scene->sdf_count = scene->sdf_capacity = 0;
  scene->tree.count = 1;
  ++scene->sdf_lifetime_epoch;
  assert(
      obs_sculpt_sphere((double[]){0, 0, 0}, 0.5, (double[]){1, 0.3, 0.4}, &a));
  assert(obs_sculpt_sphere((double[]){5, 0, 0}, 0.5, (double[]){0.2, 0.4, 0.5},
                           &b));
}
static double x(void) {
  double value;
  assert(obs_sculpt_x(a, &value));
  return value;
}
static bool press(double x) {
  double origin[3] = {x, 0, 10}, direction[3] = {0, 0, -1};
  double facing[3] = {0, 0, -1};
  return obs_sdf_viewport_press(origin, direction, facing, 100.0);
}
static void slide(double px) {
  double origin[3] = {px, 0, 10}, direction[3] = {0, 0, -1};
  assert(obs_sdf_viewport_motion(origin, direction));
  assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_OK);
}
int main(void) {
  for (unsigned slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
    sdfSSBOMapped[slot] = gpu[slot];
  fresh();
  assert(!press(0)); /* tool explicitly opt-in */
  assert(obs_sdf_viewport_set_enabled(true));
  assert(!press(50)); /* missed ray does not open an edit */
  assert(!obs_sdf_viewport_dragging());
  assert(!obs_sdf_viewport_press((double[]){0, 0, 10}, (double[]){0, 0, -1},
                                 (double[]){0, 1, 0},
                                 100)); /* parallel drag plane */
  assert(!obs_sdf_viewport_press((double[]){0, 0, 10}, (double[]){0, 0, -1},
                                 (double[]){0, 0, -1}, 0));
  assert(press(0));
  assert(obs_sdf_viewport_drag_ticket() == a);
  assert(!press(0));                           /* no nesting */
  assert(obs_sdf_viewport_set_enabled(false)); /* disable cancels */
  assert(!obs_sdf_viewport_dragging());
  assert(!obs_sdf_viewport_enabled());
  assert(obs_sdf_inspector_state().undo_count == 0);
  assert(obs_sdf_viewport_set_enabled(true));
  assert(press(0));
  for (unsigned i = 0; i < 4000; ++i) {
    /* Same sphere, one coalesced staged command despite 4000 updates. */
    slide((double)((i % 20) + 1) / 10.0);
  }
  assert(x() == 2.0);
  assert(obs_sdf_inspector_state().undo_count == 0);
  assert(obs_sdf_sync_frame(0));
  assert(gpu[0][0].inverseTransform[3][0] == -2.0f);
  assert(obs_sdf_viewport_release());
  assert(!obs_sdf_viewport_dragging());
  assert(obs_sdf_inspector_state().undo_count == 1);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  assert(x() == 0.0);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
  assert(x() == 2.0);

  fresh();
  assert(press(0));
  slide(1);
  slide(2);
  assert(obs_sdf_sync_frame(1));
  assert(gpu[1][0].inverseTransform[3][0] == -2.0f);
  assert(obs_sdf_viewport_cancel());
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 0);
  assert(obs_sdf_sync_frame(0));
  assert(gpu[0][0].inverseTransform[3][0] == 0.0f);
  assert(obs_sdf_inspector_state().undo_count == 0);

  /* Returning to the exact origin is a no-op, not a spurious undo entry. */
  assert(press(0));
  slide(1);
  slide(0);
  assert(obs_sdf_viewport_release());
  assert(obs_sdf_inspector_state().undo_count == 0);

  /* Pointer capture: direct scene edits cannot be overwritten on release
   * or Escape. Once restored, Escape can perform a safe cancellation. */
  assert(press(0));
  slide(2);
  assert(obs_sculpt_move(a, (double[]){99, 0, 0}));
  assert(obs_sdf_viewport_release());
  assert(obs_sdf_viewport_dragging());
  assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_CONFLICT);
  assert(!obs_sdf_viewport_set_enabled(false));
  assert(x() == 99);
  assert(obs_sculpt_move(a, (double[]){2, 0, 0}));
  assert(obs_sdf_viewport_cancel());
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 0);

  /* A manual group from another API must not be captured or orphaned. */
  assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
  assert(!press(0));
  assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
  /* Scene reset discards stale preview without mutating a new sphere. */
  assert(press(0));
  slide(1);
  fresh();
  assert(obs_sdf_viewport_cancel());
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 0);

  /* A failed pointer sample may not silently commit the preceding valid
   * preview. Escape still rolls the whole transaction back. */
  assert(press(0));
  slide(2);
  assert(
      obs_sdf_viewport_motion((double[]){1e100, 0, 10}, (double[]){0, 0, -1}));
  assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_INVALID);
  assert(obs_sdf_viewport_release());
  assert(obs_sdf_viewport_dragging());
  assert(x() == 2);
  assert(obs_sdf_viewport_cancel());
  assert(x() == 0);

  /* A malformed ray after a valid preview must never release as a commit. */
  assert(press(0));
  slide(2);
  assert(obs_sdf_viewport_motion((double[]){NAN, 0, 10}, (double[]){0, 0, -1}));
  assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_INVALID);
  assert(obs_sdf_viewport_release());
  assert(obs_sdf_viewport_dragging());
  assert(x() == 2);
  assert(obs_sdf_viewport_cancel());
  assert(x() == 0);

  /* Sphere B gets its own stable ticket and its own reversible command. */
  assert(press(5));
  assert(obs_sdf_viewport_drag_ticket() == b);
  slide(6);
  assert(obs_sdf_viewport_release());
  double bx;
  assert(obs_sculpt_x(b, &bx) && bx == 6);
  assert(x() == 0);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  assert(obs_sculpt_x(b, &bx) && bx == 5);

  /* Invalid/parallel motion never changes geometry or closes the gesture. */
  assert(press(0));
  assert(obs_sdf_viewport_motion((double[]){0, 0, 10}, (double[]){1, 0, 0}));
  assert(x() == 0);
  assert(obs_sdf_viewport_motion((double[]){NAN, 0, 10}, (double[]){0, 0, -1}));
  assert(x() == 0);
  assert(obs_sdf_viewport_cancel());
  assert(obs_sdf_inspector_state().undo_count >= 1);

  /* Missing GLFW release: reconcile the real polled button, commit once. */
  fresh();
  assert(press(0));
  slide(3);
  assert(!obs_sdf_viewport_reconcile(true, true));
  assert(obs_sdf_viewport_dragging());
  assert(obs_sdf_viewport_reconcile(false, true));
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 3);
  assert(obs_sdf_inspector_state().undo_count == 1);
  assert(!obs_sdf_viewport_reconcile(false, true));
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK && x() == 0);

  /* Focus loss is a cancel, not a synthetic pointer-release commit. */
  fresh();
  assert(press(0));
  slide(3);
  assert(obs_sdf_viewport_reconcile(true, false));
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 0 && obs_sdf_inspector_state().undo_count == 0);

  /* If the cancel initially conflicts, it MUST remain a cancel after the
   * button is released and even if focus subsequently returns. */
  fresh();
  assert(press(0));
  slide(3);
  assert(obs_sculpt_move(a, (double[]){19, 0, 0}));
  assert(obs_sdf_viewport_reconcile(true, false));
  assert(obs_sdf_viewport_dragging());
  assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_CONFLICT);
  assert(obs_sdf_viewport_reconcile(false, true));
  assert(obs_sdf_viewport_dragging());
  assert(x() == 19);
  assert(obs_sculpt_move(a, (double[]){3, 0, 0}));
  /* The late RELEASE may not accidentally accept the old preview. */
  assert(obs_sdf_viewport_release());
  assert(!obs_sdf_viewport_dragging());
  assert(x() == 0);
  assert(obs_sdf_inspector_state().undo_count == 0);

  /* Repeated real drag sessions do not leak/accumulate staging slots. */
  for (unsigned i = 0; i < 600; ++i) {
    fresh();
    assert(press(0));
    slide((i % 8) + 1);
    if (i & 1) {
      assert(obs_sdf_viewport_cancel());
      assert(x() == 0);
    } else {
      assert(obs_sdf_viewport_release());
      assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
      assert(x() == 0);
    }
  }

  /* No pointer movement means no committed command. */
  assert(press(0));
  assert(obs_sdf_viewport_release());
  assert(obs_sdf_inspector_state().undo_count == 0);
  free(scene->sdfs);
  puts("Cycle 55 real SDF viewport pick/drag/cancel/undo PASS");
  return 0;
}
