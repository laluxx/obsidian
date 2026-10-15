/* Cycle 53: production scene inspector, Emacs undo command semantics.
 * Mirrors the command traces described in docs/EMACS-SCENE-UNDO.org. */
#define _POSIX_C_SOURCE 200809L
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
static SdfPrimitive gpu[2][MAX_SDF_PRIMITIVES];
void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name, int32_t parent,
                            int32_t mesh_index) {
  assert(name && parent >= 0 && mesh_index == -1);
  if (tree->count >= SCENE_TREE_MAX_NODES)
    return -1;
  int32_t idx = tree->count++;
  tree->nodes[idx].sdf_index = -1;
  return idx;
}
static uint64_t id;
static void new_scene(void) {
  free(scene->sdfs);
  scene->sdfs = NULL;
  scene->sdf_count = scene->sdf_capacity = 0;
  scene->tree.count = 1;
  ++scene->sdf_lifetime_epoch;
  assert(obs_sculpt_sphere((double[]){0, 0, 0}, 0.5, (double[]){1, 0.3, 0.4},
                           &id));
  assert(obs_sdf_inspector_select(id) == OBS_SDF_EDIT_OK);
}
static void edit(double x) {
  ObsSdfEditValues v = {.xyz = {x, 0, 0}};
  assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
}
static void check(double expected) {
  double actual = -999;
  assert(obs_sculpt_x(id, &actual) && actual == expected);
}
int main(void) {
  sdfSSBOMapped[0] = gpu[0];
  sdfSSBOMapped[1] = gpu[1];
  /* Emacs: successive undo walks older boundaries; the undo's inverse
   * is recorded, but does not interrupt that same sequence. */
  new_scene();
  edit(1);
  edit(2);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(1);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(0);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_EMPTY);
  check(0);
  /* Emacs: an intervening command makes the next ordinary undo redo. */
  obs_sdf_inspector_command_boundary();
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(1);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(0);
  /* Emacs undo-only skips undo-generated records, even across a boundary. */
  new_scene();
  edit(1);
  edit(2);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(1);
  obs_sdf_inspector_command_boundary();
  assert(obs_sdf_inspector_undo_only() == OBS_SDF_EDIT_OK);
  check(0);
  assert(obs_sdf_inspector_undo_only() == OBS_SDF_EDIT_EMPTY);
  /* undo-redo consumes the last undo record, and refuses after edits. */
  new_scene();
  edit(1);
  edit(2);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(1);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
  check(2);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_EMPTY);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(1);
  edit(3);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_EMPTY);
  /* Divergence never overwrites a scene object changed outside journal. */
  assert(obs_sculpt_move(id, (double[]){7, 0, 0}));
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_CONFLICT);
  check(7);
  /* Command grouping is atomic, even across different selected spheres. */
  new_scene();
  uint64_t another;
  assert(obs_sculpt_sphere((double[]){5, 0, 0}, 0.5, (double[]){0.3, 0.4, 0.5},
                           &another));
  assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_INVALID);
  edit(1);
  edit(2);
  assert(obs_sdf_inspector_select(another) == OBS_SDF_EDIT_OK);
  edit(6);
  assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_INVALID);
  assert(obs_sdf_inspector_state().undo_count == 1);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(0);
  double value;
  assert(obs_sculpt_x(another, &value) && value == 5);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
  check(2);
  assert(obs_sculpt_x(another, &value) && value == 6);
  /* A group conflict must reject before replaying *any* member. */
  obs_sdf_inspector_command_boundary();
  assert(obs_sculpt_move(id, (double[]){13, 0, 0}));
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_CONFLICT);
  check(13);
  assert(obs_sculpt_x(another, &value) && value == 6);
  /* A separate, valid scene epoch has no borrowed history. */
  new_scene();
  assert(obs_sdf_inspector_state().undo_count == 0);
  assert(obs_sdf_inspector_undo_only() == OBS_SDF_EDIT_EMPTY);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_EMPTY);
  /* Bounded records, still deterministic after eviction. */
  for (unsigned i = 1; i <= 100; ++i)
    edit((double)i);
  assert(obs_sdf_inspector_state().undo_count == OBS_SDF_EDIT_HISTORY_CAP);
  for (unsigned i = 0; i < 10; ++i)
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  check(90);
  assert(obs_sdf_sync_frame(0));
  assert(gpu[0][0].inverseTransform[3][0] == -90);
  /* A deterministic repeated edit/undo/explicit-redo soak: all transitions
   * must reproduce the exact quantized scene state under bounded eviction. */
  new_scene();
  for (unsigned i = 1; i <= 2000; ++i) {
    double target = (double)(i % 23 + 1);
    edit(target);
    check(target);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    check(target);
  }
  /* One oldest record may be evicted to make room for the temporary
   * inverse, just as Emacs truncates history under its memory limits. */
  assert(obs_sdf_inspector_state().undo_count >= OBS_SDF_EDIT_HISTORY_CAP - 1);
  free(scene->sdfs);
  puts("Cycle 53 Emacs-style scene undo / undo-only / undo-redo / command "
       "groups PASS");
  return 0;
}
