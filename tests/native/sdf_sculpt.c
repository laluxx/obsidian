#define _POSIX_C_SOURCE 200809L
#include "sdf_sculpt.h"
#include "agent_engine_contract.h"
#include "sdf_inspector.h"
#include "sdf_tokens.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Scene scene_storage = {.sdf_lifetime_epoch = 11, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[2][MAX_SDF_PRIMITIVES];
void scene_bump_sdf_revision(Scene *s) { s->sdf_revision++; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name, int32_t parent,
                            int32_t mesh_index) {
  assert(name && parent >= 0 && mesh_index == -1);
  if (tree->count >= SCENE_TREE_MAX_NODES)
    return -1;
  int32_t idx = tree->count++;
  tree->nodes[idx].sdf_index = -1;
  return idx;
}
static void reset(void) {
  free(scene->sdfs);
  scene->sdfs = NULL;
  scene->sdf_capacity = scene->sdf_count = 0;
  scene->tree.count = 1;
  scene->sdf_lifetime_epoch++;
  flushSdfSSBO();
}
int main(void) {
  sdfSSBOMapped[0] = gpu[0];
  sdfSSBOMapped[1] = gpu[1];
  assert(!obs_sdf_sync_frame(2));
  double xyz[3] = {0, 0, -2}, rgb[3] = {1, 0.2, 0.1};
  uint64_t first = 123;
  double bad[3] = {0, NAN, -2};
  assert(!obs_sculpt_sphere(bad, 0.5, rgb, &first));
  assert(first == 123 && scene->sdf_count == 0);
  assert(!obs_sculpt_sphere(xyz, 0.0, rgb, &first));
  assert(obs_sculpt_sphere(xyz, 0.5, rgb, &first) && first > 0);
  assert(scene->sdf_count == 1 && active_sdf_count == 0);
  assert(obs_sculpt_ticket_at(0) == first && obs_sculpt_ticket_at(1) == 0 &&
         obs_sculpt_ticket_at(UINT32_MAX) == 0);
  uint32_t resolved = UINT32_MAX;
  assert(obs_sculpt_index(first, &resolved) && resolved == 0);
  assert(!obs_sculpt_index(0, &resolved));
  ObsSculptRayHit picked = {0};
  assert(obs_sculpt_ray_cast((double[3]){0, 0, -5}, (double[3]){0, 0, 2}, 10,
                             &picked) &&
         picked.ticket == first && fabs(picked.distance - 2.5) < 1e-6 &&
         fabs(picked.normal[2] + 1) < 1e-6);
  assert(obs_sculpt_ray_cast((double[3]){0, 0, -2}, (double[3]){0, 0, 1}, 10,
                             &picked) &&
         fabs(picked.distance - 0.5) < 1e-6);
  assert(obs_sculpt_ray_cast((double[3]){0.5, 0, -5}, (double[3]){0, 0, 1}, 10,
                             &picked) &&
         fabs(picked.distance - 3) < 1e-6);
  uint64_t unchanged = picked.ticket;
  assert(!obs_sculpt_ray_cast((double[3]){1, 0, -5}, (double[3]){0, 0, 1}, 10,
                              &picked) &&
         picked.ticket == unchanged);
  assert(!obs_sculpt_ray_cast((double[3]){0, 0, -5}, (double[3]){0, 0, 0}, 10,
                              &picked));
  assert(!obs_sculpt_ray_cast((double[3]){0, 0, -5}, (double[3]){0, 0, 1}, NAN,
                              &picked));
  scene->sdfs[0].inverseTransform[0][0] = 2;
  assert(!obs_sculpt_ray_cast((double[3]){0, 0, -5}, (double[3]){0, 0, 1}, 10,
                              &picked));
  scene->sdfs[0].inverseTransform[0][0] = 1;
  assert(gpu[0][0].size[0] == 0 && gpu[1][0].size[0] == 0);
  assert(obs_sdf_sync_frame(0) && active_sdf_count == 1);
  assert(gpu[0][0].size[0] == 0.5f && gpu[1][0].size[0] == 0);
  xyz[0] = 2.5;
  assert(obs_sculpt_move(first, xyz));
  assert(!obs_sculpt_move(first, bad));
  assert(obs_sdf_sync_frame(1));
  assert(gpu[1][0].inverseTransform[3][0] == -2.5f);
  assert(gpu[0][0].inverseTransform[3][0] == 0.0f);
  assert(obs_sdf_sync_frame(0));
  assert(gpu[0][0].inverseTransform[3][0] == -2.5f);
  rgb[0] = 0.1;
  assert(obs_sculpt_tint(first, rgb));
  double x = -999;
  assert(obs_sculpt_x(first, &x) && x == 2.5);
  assert(obs_sdf_sync_frame(1) && gpu[1][0].color[0] > 0.09f);
  assert(gpu[0][0].color[0] == 1);
  uint64_t second;
  xyz[0] = -1;
  assert(obs_sculpt_sphere(xyz, 0.25, rgb, &second) && second != first);
  assert(obs_sculpt_ticket_at(0) == first && obs_sculpt_ticket_at(1) == second);
  assert(!obs_sculpt_move(UINT64_MAX, xyz));
  /* Edit journal: no-op does not push, branch drops redo, conflicting direct
   * writes cannot be silently undone, wrap evicts oldest entries. */
  assert(obs_sdf_inspector_select(first) == OBS_SDF_EDIT_OK);
  ObsSdfEditValues edit = {0};
  edit.xyz[0] = 4;
  edit.xyz[1] = 0;
  edit.xyz[2] = -2;
  assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &edit) ==
         OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_state().undo_count == 1);
  assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &edit) ==
         OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_state().undo_count == 1);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_state().redo_count == 1);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
  assert(obs_sculpt_x(first, &x) && x == 4);
  assert(obs_sdf_sync_frame(0) && gpu[0][0].inverseTransform[3][0] == -4);
  assert(obs_sculpt_move(first, (double[3]){7, 0, -2}));
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_CONFLICT);
  assert(obs_sculpt_x(first, &x) && x == 7);
  obs_sdf_inspector_clear();
  for (unsigned k = 0; k < 70; ++k) {
    edit.xyz[0] = (double)k;
    assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &edit) ==
           OBS_SDF_EDIT_OK);
  }
  assert(obs_sdf_inspector_state().undo_count == OBS_SDF_EDIT_HISTORY_CAP);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_state().redo_count == 1);
  edit.xyz[0] = 123;
  assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &edit) ==
         OBS_SDF_EDIT_OK);
  assert(obs_sdf_inspector_state().redo_count == 0);
  assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_EMPTY);
  edit.xyz[0] = NAN;
  assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &edit) ==
         OBS_SDF_EDIT_INVALID);
  assert(obs_sdf_inspector_state().undo_count ==
         OBS_SDF_EDIT_HISTORY_CAP - 1);
  reset();
  assert(obs_sdf_inspector_state().undo_count == 0);
  assert(obs_sdf_inspector_state().selection == 0);
  assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_EMPTY);
  assert(!obs_sculpt_move(first, xyz) && !obs_sculpt_x(first, &x));
  assert(obs_sculpt_ticket_at(0) == 0);
  assert(obs_sculpt_sphere(xyz, 0.25, rgb, &second) && second != first);
  assert(!obs_sculpt_move(first, xyz));
  assert(!obs_sculpt_index(first, &resolved));
  reset();
  printf("Cycle 50 procedural scene + isolated GPU slot publication PASS\n");
  return 0;
}
