#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "atomic_file.h"
#include "obsidian_agent.h"
#include "sdf_sculpt.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

VulkanContext context;
static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu_sdfs[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];
static bool effect_ok = true;
bool obs_monad_effect_allowed(unsigned effects, unsigned lifetime, bool force) {
  assert(effects == (OBS_EFFECT_WRITE | OBS_EFFECT_GPU));
  assert(lifetime == OBS_LIFETIME_ENGINE && !force);
  return effect_ok;
}
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

static ObsMonadRuntime runtime;
static monad_source_result_t source_result = {.published = 1};
static bool monad_started, start_ok = true, load_ok = true, frame_ok = true;
static bool policy_ok = true, gpu_idle_ok = true;
static unsigned init_calls, cleanup_calls, starts, stops, loads, policies;
static unsigned frame_calls, raw_calls, draws, source_result_destroys;
static ObsAgentEngine *reentrant_engine;
static ObsAgentSdfRef reentrant_sphere;

bool obs_headless_init(uint32_t w, uint32_t h, bool readback) {
  assert(w == 16 && h == 8 && readback);
  ++init_calls;
  context.device = (void *)0x1;
  sdfSSBOMapped[0] = gpu_sdfs[0];
  sdfSSBOMapped[1] = gpu_sdfs[1];
  return true;
}
void cleanup(VulkanContext *ctx) {
  ++cleanup_calls;
  ctx->device = NULL;
}
int vkDeviceWaitIdle(void *device) {
  assert(device == (void *)0x1);
  return gpu_idle_ok ? VK_SUCCESS : -1;
}
bool obs_monad_engine_start(void) {
  ++starts;
  return monad_started = start_ok;
}
void obs_monad_engine_stop(void) {
  ++stops;
  monad_started = false;
}
bool obs_monad_engine_ready(void) { return monad_started; }
bool obs_monad_camera_step_checked(void) {
  ++policies;
  return policy_ok;
}
ObsMonadRuntime *obs_monad_engine(void) { return &runtime; }
bool obs_monad_load_file(ObsMonadRuntime *host, const char *path,
                         monad_source_result_t **result, monad_error_t *error) {
  assert(host == &runtime);
  assert(strcmp(path, "game.mon") == 0 || strcmp(path, "reject.mon") == 0);
  ++loads;
  *result = &source_result;
  if (!load_ok)
    error->message = "mock compiler rejected the source";
  return load_ok;
}
bool obs_monad_load_source(ObsMonadRuntime *host, const char *name,
                           const char *source, monad_source_result_t **result,
                           monad_error_t *error) {
  assert(host == &runtime);
  assert(strcmp(name, "agent://memory") == 0);
  assert(strcmp(source, "(module Main)") == 0);
  ++loads;
  *result = &source_result;
  if (!load_ok)
    error->message = "inline compiler rejected source";
  return load_ok;
}
size_t
monad_source_result_diagnostic_count(const monad_source_result_t *result) {
  assert(result == &source_result);
  return 0;
}
const char *
monad_source_result_diagnostic_message(const monad_source_result_t *result,
                                       size_t index) {
  (void)result;
  (void)index;
  return "unreachable";
}
void monad_source_result_destroy(monad_source_result_t *result) {
  assert(result == &source_result);
  ++source_result_destroys;
}

static void draw(void *userdata) {
  ++draws;
  assert(userdata == (void *)0x1234);
  if (reentrant_engine) {
    ObsAgentError error = {0};
    assert(obs_agent_step(reentrant_engine, NULL, NULL, &error) ==
           OBS_AGENT_BUSY);
    assert(error.code == OBS_AGENT_BUSY);
    ObsAgentRayHit ray_hit;
    assert(obs_agent_scene_pick_ray(reentrant_engine, (double[3]){0, 0, -3},
                                    (double[3]){0, 0, 1}, 10, &ray_hit,
                                    &error) == OBS_AGENT_BUSY);
    ObsAgentSphere state = {0};
    assert(obs_agent_sphere_query(reentrant_engine, reentrant_sphere, &state,
                                  &error) == OBS_AGENT_BUSY);
  }
}
bool obs_headless_render_frame(void (*callback)(void *), void *userdata) {
  ++frame_calls;
  if (callback)
    callback(userdata);
  return frame_ok;
}
bool obs_headless_render_raw(const char *path, void (*callback)(void *),
                             void *userdata) {
  ++raw_calls;
  if (callback)
    callback(userdata);
  if (!frame_ok)
    return false;
  unsigned char data[16u * 8u * 4u];
  memset(data, 0x7b, sizeof(data));
  return obs_atomic_write_bytes(path, data, sizeof(data));
}

typedef struct {
  ObsAgentEngine *engine;
  ObsAgentStatus status;
} WrongThread;
static void *thread_attempt(void *arg) {
  WrongThread *thread = arg;
  thread->status = obs_agent_step(thread->engine, NULL, NULL, NULL);
  return NULL;
}

int main(void) {
  ObsAgentError error = {0};
  ObsAgentConfig cfg = {.abi_version = OBS_AGENT_ABI_VERSION,
                        .width = 16,
                        .height = 8,
                        .call_camera_policy = true};
  assert(!obs_agent_open(NULL, &error) &&
         error.code == OBS_AGENT_INVALID_ARGUMENT);
  cfg.abi_version = 99;
  assert(!obs_agent_open(&cfg, &error) &&
         error.code == OBS_AGENT_INVALID_ARGUMENT);
  cfg.abi_version = OBS_AGENT_ABI_VERSION;
  cfg.width = 8193;
  assert(!obs_agent_open(&cfg, &error) &&
         error.code == OBS_AGENT_INVALID_ARGUMENT);
  cfg.width = 16;
  start_ok = false;
  assert(!obs_agent_open(&cfg, &error) &&
         error.code == OBS_AGENT_INITIALIZATION_FAILED);
  assert(cleanup_calls == 1 && !context.device);
  start_ok = true;
  ObsAgentEngine *engine = obs_agent_open(&cfg, &error);
  assert(engine && error.code == OBS_AGENT_OK && !error.message[0]);
  assert(!obs_agent_open(&cfg, &error) && error.code == OBS_AGENT_BUSY);
  WrongThread attempt = {.engine = engine};
  pthread_t t;
  assert(pthread_create(&t, NULL, thread_attempt, &attempt) == 0);
  assert(pthread_join(t, NULL) == 0);
  assert(attempt.status == OBS_AGENT_WRONG_THREAD);
  assert(obs_agent_step(engine, NULL, NULL, &error) == OBS_AGENT_FRAME_FAILED);
  assert(frame_calls == 0 && policies == 0);
  assert(obs_agent_load_file(engine, NULL, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  load_ok = false;
  assert(obs_agent_load_file(engine, "reject.mon", &error) ==
         OBS_AGENT_MODULE_FAILED);
  assert(strstr(error.message, "rejected"));
  load_ok = true;
  assert(obs_agent_load_file(engine, "game.mon", &error) == OBS_AGENT_OK);
  assert(obs_agent_load_file(engine, "game.mon", &error) == OBS_AGENT_OK);
  assert(loads == 3 && source_result_destroys == 3);
  assert(obs_agent_load_source(engine, "agent://memory", "", &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  assert(obs_agent_load_source(engine, "agent://memory", "(module Main)",
                               &error) == OBS_AGENT_OK);
  assert(loads == 4 && source_result_destroys == 4);
  policy_ok = false;
  assert(obs_agent_step(engine, NULL, NULL, &error) == OBS_AGENT_FRAME_FAILED);
  assert(frame_calls == 0);
  policy_ok = true;
  reentrant_engine = engine;
  assert(obs_agent_step(engine, draw, (void *)0x1234, &error) == OBS_AGENT_OK);
  reentrant_engine = NULL;
  assert(draws == 1 && frame_calls == 1);
  frame_ok = false;
  assert(obs_agent_step(engine, NULL, NULL, &error) == OBS_AGENT_FRAME_FAILED);
  frame_ok = true;
  assert(obs_agent_step(engine, NULL, NULL, &error) == OBS_AGENT_OK);

  /* The actual production SDF append, tree, GPU flush and typed outer ABI. */
  ObsAgentSphere sphere = {.position = {1.25, -2.5, 3},
                           .radius = 0.75,
                           .rgba = {0.2, 0.4, 0.6, 1},
                           .roughness = 0.3,
                           .metallic = 0.1};
  ObsAgentSdfRef sphere_id = {0};
  assert(obs_agent_sphere_create(engine, NULL, &sphere_id, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  ObsAgentSphere bad = sphere;
  bad.position[0] = NAN;
  assert(obs_agent_sphere_create(engine, &bad, &sphere_id, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  assert(scene->sdf_count == 0 && active_sdf_count == 0);
  effect_ok = false;
  assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
         OBS_AGENT_BUSY);
  effect_ok = true;
  assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
             OBS_AGENT_OK &&
         sphere_id.ticket != 0);
  assert(scene->sdf_count == 1 && active_sdf_count == 0 &&
         scene->tree.count == 3);
  reentrant_engine = engine;
  reentrant_sphere = sphere_id;
  assert(obs_agent_step(engine, draw, (void *)0x1234, &error) == OBS_AGENT_OK);
  reentrant_engine = NULL;
  /* Slot upload is intentionally not immediate. Only the fenced slot is
   * modified; another frame's mapped SDF data stays untouched. */
  assert(gpu_sdfs[0][0].size[0] == 0.0f);
  assert(obs_sdf_sync_frame(0));
  assert(active_sdf_count == 1);
  assert(gpu_sdfs[0][0].size[0] == 0.75f && gpu_sdfs[1][0].size[0] == 0.0f);
  assert(obs_sdf_sync_frame(1));
  assert(gpu_sdfs[1][0].roughness == scene->sdfs[0].roughness);
  ObsAgentSphere observed = {0};
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
             OBS_AGENT_OK &&
         fabs(observed.position[0] - 1.25) < 1e-6 &&
         fabs(observed.radius - 0.75) < 1e-6);
  double xyz[3] = {-4, 5.5, 9};
  double rgba[4] = {0.75, 0.5, 0.25, 1};
  gpu_idle_ok = false; /* Mutations no longer depend on device-wide idle. */
  assert(obs_agent_sphere_set_position(engine, sphere_id, xyz, &error) ==
             OBS_AGENT_OK &&
         gpu_sdfs[0][0].inverseTransform[3][1] != -5.5f);
  gpu_idle_ok = true;
  assert(obs_agent_sphere_set_material(engine, sphere_id, rgba, 0.8, 0.2,
                                       &error) == OBS_AGENT_OK &&
         gpu_sdfs[1][0].color[0] != 0.75f);
  assert(obs_sdf_sync_frame(1));
  assert(gpu_sdfs[1][0].color[0] == 0.75f &&
         gpu_sdfs[0][0].inverseTransform[3][1] != -5.5f);
  assert(obs_sdf_sync_frame(0));
  assert(gpu_sdfs[0][0].inverseTransform[3][1] == -5.5f);
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == -4 && observed.roughness > 0.79 &&
         observed.roughness < 0.81);
  uint32_t visible_count = 0;
  assert(obs_agent_scene_sphere_count(engine, &visible_count, &error) ==
             OBS_AGENT_OK &&
         visible_count == 1);
  assert(obs_agent_scene_sphere_at(engine, 0, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == -4);
  assert(obs_agent_scene_sphere_at(engine, 99, &observed, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  /* Unified capability namespace: a C-built sphere is accessible from
   * compiled Monad, and a Monad-built sphere from the external C facade. */
  assert(obs_sculpt_ticket_at(0) == sphere_id.ticket);
  double cast_origin[3] = {-4, 5.5, 0};
  double cast_direction[3] = {0, 0, 5};
  ObsAgentRayHit hit = {0};
  assert(obs_agent_scene_pick_ray(engine, cast_origin, cast_direction, 30, &hit,
                                  &error) == OBS_AGENT_OK);
  assert(hit.ref.ticket == sphere_id.ticket && hit.index == 0 &&
         fabs(hit.distance - 8.25) < 0.00001 &&
         fabs(hit.normal[2] + 1) < 0.00001);
  /* Cross-boundary selection, bounded journal, conflict-safe undo and redo. */
  effect_ok = false;
  assert(obs_agent_scene_select(engine, hit.ref, &error) == OBS_AGENT_BUSY);
  effect_ok = true;
  assert(obs_agent_scene_select(engine, hit.ref, &error) == OBS_AGENT_OK);
  ObsAgentSdfRef selected = {0};
  ObsAgentEditHistory history = {0};
  assert(obs_agent_scene_selected(engine, &selected, &error) == OBS_AGENT_OK &&
         selected.ticket == hit.ref.ticket);
  ObsAgentSphere desired = observed;
  desired.position[0] = -6;
  desired.position[1] = 5.5;
  desired.position[2] = 9;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION,
                                       &desired, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK &&
         history.undo_count == 1 && history.redo_count == 0);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == -6);
  effect_ok = false;
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_BUSY);
  effect_ok = true;
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK &&
         history.undo_count == 0 && history.redo_count == 1);
  assert(obs_agent_scene_redo(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == -6);
  assert(obs_agent_sphere_set_position(engine, hit.ref, (double[3]){-7, 5.5, 9},
                                       &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_EDIT_CONFLICT);
  assert(obs_agent_scene_discard_history(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_HISTORY_EMPTY);
  /* The actual public C API groups multiple edits and implements the
   * ordinary Emacs undo-of-undo command boundary. */
  assert(obs_agent_scene_begin_group(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_begin_group(engine, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
         OBS_AGENT_OK);
  double original_x = observed.position[0];
  desired = observed;
  desired.position[0] = -8;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION,
                                       &desired, &error) == OBS_AGENT_OK);
  desired.position[0] = -9;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION,
                                       &desired, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_end_group(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == original_x);
  assert(obs_agent_scene_command_boundary(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_undo(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == -9);
  assert(obs_agent_scene_undo_only(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == original_x);
  assert(obs_agent_scene_redo(engine, &error) == OBS_AGENT_OK);
  /* External preview can be cancelled without disturbing committed state,
   * and discard_history is forbidden while a speculative group is active. */
  assert(obs_agent_scene_begin_group(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK);
  uint32_t previous_undo_count = history.undo_count;
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
         OBS_AGENT_OK);
  double previous_x = observed.position[0];
  desired = observed;
  desired.position[0] = -11;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION,
                                       &desired, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_discard_history(engine, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK &&
         history.undo_count == previous_undo_count);
  effect_ok = false;
  assert(obs_agent_scene_abort_group(engine, &error) == OBS_AGENT_BUSY);
  effect_ok = true;
  assert(obs_agent_scene_abort_group(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, hit.ref, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == previous_x);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK &&
         history.undo_count == previous_undo_count);
  assert(obs_agent_scene_abort_group(engine, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  assert(obs_agent_scene_discard_history(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_select(engine, (ObsAgentSdfRef){0}, &error) ==
         OBS_AGENT_OK);
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION,
                                       &desired,
                                       &error) == OBS_AGENT_STALE_REFERENCE);
  assert(obs_agent_scene_select(engine, hit.ref, &error) == OBS_AGENT_OK);
  double miss[3] = {100, 100, 0};
  ObsAgentRayHit unchanged = hit;
  assert(obs_agent_scene_pick_ray(engine, miss, cast_direction, 30, &hit,
                                  &error) == OBS_AGENT_NO_HIT);
  assert(hit.ref.ticket == unchanged.ref.ticket);
  assert(obs_agent_scene_pick_ray(engine, miss, (double[3]){0, 0, 0}, 30, &hit,
                                  &error) == OBS_AGENT_INVALID_ARGUMENT);
  assert(obs_agent_scene_pick_ray(engine, miss, cast_direction, NAN, &hit,
                                  &error) == OBS_AGENT_INVALID_ARGUMENT);
  double monad_xyz[3] = {2, -1, 0}, monad_rgb[3] = {0.9, 0.1, 0.2};
  uint64_t monad_id = 0;
  assert(obs_sculpt_sphere(monad_xyz, 0.5, monad_rgb, &monad_id));
  assert(obs_agent_sphere_query(engine, (ObsAgentSdfRef){monad_id}, &observed,
                                &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_pick_ray(engine, (double[3]){2, -1, -3},
                                  (double[3]){0, 0, 1}, 10, &hit,
                                  &error) == OBS_AGENT_OK);
  assert(hit.ref.ticket == monad_id && hit.index == 1);
  assert(obs_agent_sphere_set_material(engine, hit.ref,
                                       (double[4]){0.2, 0.5, 0.8, 1}, 0.7, 0.1,
                                       &error) == OBS_AGENT_OK);
  assert(fabs(scene->sdfs[1].color[2] - 0.8f) < 1e-5);
  assert(obs_sculpt_tint(sphere_id.ticket, (double[3]){0.5, 0.4, 0.3}));
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.rgba[0] == 0.5);
  /* The public agent API shares selected-object history and effect admission. */
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
         OBS_AGENT_OK);
  const double selected_original_x = observed.position[0];
  assert(obs_agent_scene_select(engine, sphere_id, &error) == OBS_AGENT_OK);
  ObsAgentSphere edit = observed;
  edit.position[0] = selected_original_x + 1;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION, &edit,
                                       &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, (ObsAgentSdfRef){monad_id}, &observed,
                                &error) == OBS_AGENT_OK);
  const double monad_original_x = observed.position[0];
  assert(obs_agent_scene_select(engine, (ObsAgentSdfRef){monad_id}, &error) ==
         OBS_AGENT_OK);
  edit = observed;
  edit.position[0] = monad_original_x + 1;
  assert(obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_POSITION, &edit,
                                       &error) == OBS_AGENT_OK);
  assert(obs_agent_scene_select(engine, sphere_id, &error) == OBS_AGENT_OK);
  effect_ok = false;
  assert(obs_agent_scene_undo_selected(engine, &error) == OBS_AGENT_BUSY);
  effect_ok = true;
  assert(obs_agent_scene_undo_selected(engine, &error) == OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
             OBS_AGENT_OK &&
         observed.position[0] == selected_original_x);
  assert(obs_agent_sphere_query(engine, (ObsAgentSdfRef){monad_id}, &observed,
                                &error) == OBS_AGENT_OK &&
         observed.position[0] == monad_original_x + 1);
  /* Restore expected counts/material for subsequent capacity tests. */
  uint32_t old_count = scene->sdf_count;
  float old_roughness = scene->sdfs[0].roughness;
  double invalid_color[4] = {1, -1, 0, 1};
  assert(obs_agent_sphere_set_material(engine, sphere_id, invalid_color, 0, 0,
                                       &error) == OBS_AGENT_INVALID_ARGUMENT);
  assert(scene->sdf_count == old_count &&
         scene->sdfs[0].roughness == old_roughness);
  assert(obs_agent_sphere_query(engine, (ObsAgentSdfRef){UINT64_MAX}, &observed,
                                &error) == OBS_AGENT_STALE_REFERENCE);
  scene->sdf_lifetime_epoch++; /* simulate real scene_cleanup/scene_init */
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
         OBS_AGENT_STALE_REFERENCE);
  assert(obs_agent_scene_history(engine, &history, &error) == OBS_AGENT_OK &&
         history.undo_count == 0 && history.selected.ticket == 0);
  assert(obs_agent_scene_selected(engine, &selected, &error) == OBS_AGENT_OK &&
         selected.ticket == 0);
  assert(obs_agent_sphere_query(engine, (ObsAgentSdfRef){monad_id}, &observed,
                                &error) == OBS_AGENT_STALE_REFERENCE);
  assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
         OBS_AGENT_OK);
  assert(obs_agent_sphere_query(engine, sphere_id, &observed, &error) ==
         OBS_AGENT_OK);
  scene->tree.count = SCENE_TREE_MAX_NODES - 1;
  assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
             OBS_AGENT_RESOURCE_LIMIT &&
         scene->sdf_count == old_count + 1);
  scene->tree.count = 5;
  for (uint32_t i = scene->sdf_count; i < MAX_SDF_PRIMITIVES; ++i)
    assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
           OBS_AGENT_OK);
  assert(obs_sdf_sync_frame(0));
  assert(active_sdf_count == MAX_SDF_PRIMITIVES);
  assert(obs_agent_sphere_create(engine, &sphere, &sphere_id, &error) ==
         OBS_AGENT_RESOURCE_LIMIT);
  assert(scene->sdf_count == MAX_SDF_PRIMITIVES);

  char dir[] = "/tmp/obs-agent-api-XXXXXX";
  assert(mkdtemp(dir));
  char raw[256], receipt[256];
  snprintf(raw, sizeof(raw), "%s/image.rgba", dir);
  snprintf(receipt, sizeof(receipt), "%s/receipt.json", dir);
  assert(obs_atomic_write_bytes(receipt, "old status", 10));
  assert(obs_agent_capture(engine, raw, raw, NULL, NULL, &error) ==
         OBS_AGENT_INVALID_ARGUMENT);
  frame_ok = false;
  assert(obs_agent_capture(engine, raw, receipt, NULL, NULL, &error) ==
         OBS_AGENT_FRAME_FAILED);
  assert(access(receipt, F_OK) != 0); /* stale success removed */
  frame_ok = true;
  assert(obs_agent_capture(engine, raw, receipt, NULL, NULL, &error) ==
         OBS_AGENT_OK);
  struct stat image_stat;
  assert(stat(raw, &image_stat) == 0 && image_stat.st_size == 512);
  FILE *json = fopen(receipt, "r");
  assert(json);
  char record[1024];
  size_t len = fread(record, 1, sizeof(record) - 1u, json);
  record[len] = 0;
  fclose(json);
  assert(strstr(record, "\"frames\":4"));
  assert(strstr(record, "\"bytes\":512"));
  assert(strstr(record, "\"source\":\"inline\""));

  gpu_idle_ok = false;
  assert(obs_agent_close(engine, &error) == OBS_AGENT_SHUTDOWN_FAILED);
  assert(context.device && monad_started);
  gpu_idle_ok = true;
  assert(obs_agent_close(engine, &error) == OBS_AGENT_OK);
  assert(cleanup_calls == 2 && !context.device && !monad_started);
  assert(unlink(raw) == 0 && unlink(receipt) == 0 && rmdir(dir) == 0);
  free(scene->sdfs);
  scene->sdfs = NULL;
  scene->sdf_count = scene->sdf_capacity = 0;
  printf("external C agent ABI: single owner, module, step, render, error, "
         "close PASS\n");
  return 0;
}
