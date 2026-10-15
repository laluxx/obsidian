#define _POSIX_C_SOURCE 200809L
/// agent_engine — strict single-instance facade over production engine owners
#include "headless_job.h"
#include "obsidian_agent.h"
#include "sdf_inspector.h"
#include "sdf_sculpt.h"
#ifdef OBS_AGENT_ENGINE_TEST
#include "agent_engine_contract.h"
#else
#include "context.h"
#include "effect_policy.h"
#include "scene.h"
#include "sdf.h"
#include "src/monad/runtime.h"
#include "vulkan_setup.h"
#include "window.h"
#endif
#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ObsAgentEngine {
  pthread_t owner;
  uint32_t width;
  uint32_t height;
  uint32_t frames;
  bool module_loaded;
  ObsAgentSourceKind source_kind;
  bool camera_policy;
  bool in_frame;
};

/* The actual VulkanContext and Monad host are process-global. Never imply
 * that this façade creates independent device or scene instances. */
static ObsAgentEngine *s_agent;

static ObsAgentStatus status(ObsAgentError *error, ObsAgentStatus code,
                             const char *message) {
  if (error) {
    error->code = code;
    snprintf(error->message, sizeof(error->message), "%s",
             message ? message : "");
  }
  return code;
}

static ObsAgentStatus admit(ObsAgentEngine *engine, ObsAgentError *error) {
  if (!engine || engine != s_agent)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "not the live headless engine instance");
  if (!pthread_equal(engine->owner, pthread_self()))
    return status(error, OBS_AGENT_WRONG_THREAD,
                  "headless engine is owned by a different thread");
  if (engine->in_frame)
    return status(error, OBS_AGENT_BUSY,
                  "recursive frame, module load or close is forbidden");
  return OBS_AGENT_OK;
}

ObsAgentEngine *obs_agent_open(const ObsAgentConfig *config,
                               ObsAgentError *error) {
  if (!config || config->abi_version != OBS_AGENT_ABI_VERSION ||
      !config->width || !config->height || config->width > 8192u ||
      config->height > 8192u) {
    status(error, OBS_AGENT_INVALID_ARGUMENT,
           "invalid headless config, dimensions or ABI version");
    return NULL;
  }
  if (s_agent || context.device) {
    status(error, OBS_AGENT_BUSY, "an engine already owns the global renderer");
    return NULL;
  }
  ObsAgentEngine *engine = calloc(1, sizeof(*engine));
  if (!engine) {
    status(error, OBS_AGENT_INITIALIZATION_FAILED,
           "cannot allocate headless engine handle");
    return NULL;
  }
  if (!obs_headless_init(config->width, config->height, true)) {
    free(engine);
    status(error, OBS_AGENT_INITIALIZATION_FAILED,
           "Vulkan headless initialization failed");
    return NULL;
  }
  if (!obs_monad_engine_start()) {
    /* Only the resources this call successfully initialized are torn down.
     * No running editor or other device may enter through this branch. */
    cleanup(&context);
    free(engine);
    status(error, OBS_AGENT_INITIALIZATION_FAILED,
           "engine-owned MonadC runtime failed to initialize");
    return NULL;
  }
  engine->owner = pthread_self();
  engine->width = config->width;
  engine->height = config->height;
  engine->camera_policy = config->call_camera_policy;
  s_agent = engine;
  status(error, OBS_AGENT_OK, "");
  return engine;
}

static ObsAgentStatus load_module(ObsAgentEngine *engine, const char *name,
                                  const char *source, ObsAgentSourceKind kind,
                                  ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!name || !*name ||
      (kind == OBS_AGENT_SOURCE_INLINE &&
       (!source || !*source ||
        strnlen(source, 16u * 1024u * 1024u + 1u) > 16u * 1024u * 1024u)))
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "module name/source is absent or exceeds the source cap");
  monad_error_t compile_error = {0};
  monad_source_result_t *result = NULL;
  const bool published =
      kind == OBS_AGENT_SOURCE_FILE
          ? obs_monad_load_file(obs_monad_engine(), name, &result,
                                &compile_error)
          : obs_monad_load_source(obs_monad_engine(), name, source, &result,
                                  &compile_error);
  char detail[256] = {0};
  if (!published) {
    const char *message =
        result && monad_source_result_diagnostic_count(result)
            ? monad_source_result_diagnostic_message(result, 0)
            : compile_error.message;
    snprintf(detail, sizeof(detail), "%s",
             message ? message : "Monad source publication failed");
  }
  if (result)
    monad_source_result_destroy(result);
  if (!published)
    return status(error, OBS_AGENT_MODULE_FAILED, detail);
  engine->module_loaded = true;
  engine->source_kind = kind;
  return status(error, OBS_AGENT_OK, "");
}

ObsAgentStatus obs_agent_load_file(ObsAgentEngine *engine, const char *path,
                                   ObsAgentError *error) {
  return load_module(engine, path, NULL, OBS_AGENT_SOURCE_FILE, error);
}

ObsAgentStatus obs_agent_load_source(ObsAgentEngine *engine, const char *name,
                                     const char *source, ObsAgentError *error) {
  return load_module(engine, name, source, OBS_AGENT_SOURCE_INLINE, error);
}

static ObsAgentStatus run_frame(ObsAgentEngine *engine, const char *path,
                                ObsAgentDraw draw, void *userdata,
                                ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (engine->frames >= 10000u)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "headless session frame budget exceeded (10000)");
  if (engine->camera_policy &&
      (!engine->module_loaded || !obs_monad_camera_step_checked()))
    return status(error, OBS_AGENT_FRAME_FAILED,
                  "missing or rejected compiled camera policy");
  engine->in_frame = true;
  const bool rendered = path ? obs_headless_render_raw(path, draw, userdata)
                             : obs_headless_render_frame(draw, userdata);
  engine->in_frame = false;
  if (!rendered)
    return status(error, OBS_AGENT_FRAME_FAILED,
                  "production Vulkan submission/readback failed");
  ++engine->frames;
  return status(error, OBS_AGENT_OK, "");
}

ObsAgentStatus obs_agent_step(ObsAgentEngine *engine, ObsAgentDraw draw,
                              void *userdata, ObsAgentError *error) {
  return run_frame(engine, NULL, draw, userdata, error);
}

ObsAgentStatus obs_agent_capture(ObsAgentEngine *engine, const char *raw_path,
                                 const char *receipt_path, ObsAgentDraw draw,
                                 void *userdata, ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!raw_path || !*raw_path ||
      (receipt_path && (!*receipt_path || strcmp(raw_path, receipt_path) == 0)))
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "capture requires a raw path and distinct optional receipt");
  if (receipt_path && unlink(receipt_path) != 0 && errno != ENOENT)
    return status(error, OBS_AGENT_OUTPUT_FAILED,
                  "cannot invalidate previous success receipt");
  gate = run_frame(engine, raw_path, draw, userdata, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (receipt_path) {
    const ObsAgentReceipt receipt = {.image_path = raw_path,
                                     .width = engine->width,
                                     .height = engine->height,
                                     .completed_frames = engine->frames,
                                     .source = engine->source_kind};
    if (!obs_headless_write_receipt(receipt_path, &receipt))
      return status(error, OBS_AGENT_OUTPUT_FAILED,
                    "image written, but agent receipt publication failed");
  }
  return status(error, OBS_AGENT_OK, "");
}

/// Typed SDF scene editing — real scene graph and renderer SSBO
static bool world_coord(double v) {
  /* Avoid intermediate INF when negating float translations. */
  return isfinite(v) && v > -(double)FLT_MAX && v < (double)FLT_MAX;
}
static bool unit_factor(double v) {
  return isfinite(v) && v >= 0.0 && v <= 1.0;
}
static bool sphere_valid(const ObsAgentSphere *v) {
  if (!v || !isfinite(v->radius) || v->radius <= 0.0 ||
      v->radius > (double)FLT_MAX)
    return false;
  for (unsigned k = 0; k < 3; ++k)
    if (!world_coord(v->position[k]))
      return false;
  for (unsigned k = 0; k < 4; ++k)
    if (!unit_factor(v->rgba[k]))
      return false;
  return unit_factor(v->roughness) && unit_factor(v->metallic);
}
static ObsAgentStatus find_sphere(ObsAgentSdfRef ref, SdfPrimitive **prim,
                                  ObsAgentError *error) {
  uint32_t index = 0;
  if (!obs_sculpt_index(ref.ticket, &index))
    return status(error, OBS_AGENT_STALE_REFERENCE,
                  "expired or foreign SDF capability");
  *prim = &scene->sdfs[index];
  return OBS_AGENT_OK;
}
ObsAgentStatus obs_agent_sphere_create(ObsAgentEngine *engine,
                                       const ObsAgentSphere *sphere,
                                       ObsAgentSdfRef *out,
                                       ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!sphere_valid(sphere) || !out)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "invalid sphere values or output");
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return status(error, OBS_AGENT_BUSY,
                  "engine GPU mutations are not admitted");
  if (scene->sdf_count >= MAX_SDF_PRIMITIVES || !obs_sculpt_can_ticket())
    return status(error, OBS_AGENT_RESOURCE_LIMIT,
                  "SDF capacity or ticket limit reached");
  vec3 pos = {(float)sphere->position[0], (float)sphere->position[1],
              (float)sphere->position[2]};
  vec4 size = {(float)sphere->radius, 0, 0, 0};
  vec4 color = {(float)sphere->rgba[0], (float)sphere->rgba[1],
                (float)sphere->rgba[2], (float)sphere->rgba[3]};
  int32_t index = obs_sdf_add_checked(scene, "Agent sphere", SDF_TYPE_SPHERE,
                                      pos, size, color);
  if (index < 0)
    return status(error, OBS_AGENT_RESOURCE_LIMIT,
                  "SDF storage or scene-tree capacity exhausted");
  SdfPrimitive *sdf = &scene->sdfs[index];
  sdf->roughness = (float)sphere->roughness;
  sdf->metallic = (float)sphere->metallic;
  flushSdfSSBO();
  /* Both Monad and the external agent use the same scene-epoch registry.
   * Preflight reserves token capacity before the fallible scene append. */
  out->ticket = obs_sculpt_ticket_at((uint32_t)index);
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_sphere_query(ObsAgentEngine *engine,
                                      ObsAgentSdfRef ref, ObsAgentSphere *out,
                                      ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!out)
    return status(error, OBS_AGENT_INVALID_ARGUMENT, "missing state output");
  SdfPrimitive *sdf = NULL;
  gate = find_sphere(ref, &sdf, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  ObsAgentSphere state = {0};
  for (unsigned i = 0; i < 3; ++i) {
    state.position[i] = -(double)sdf->inverseTransform[3][i];
    if (!isfinite(state.position[i]))
      return status(error, OBS_AGENT_STALE_REFERENCE,
                    "SDF model is not a finite translated sphere");
  }
  state.radius = sdf->size[0];
  for (unsigned i = 0; i < 4; ++i)
    state.rgba[i] = sdf->color[i];
  state.roughness = sdf->roughness;
  state.metallic = sdf->metallic;
  *out = state;
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_sphere_set_position(ObsAgentEngine *engine,
                                             ObsAgentSdfRef ref,
                                             const double xyz[3],
                                             ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!xyz || !world_coord(xyz[0]) || !world_coord(xyz[1]) ||
      !world_coord(xyz[2]))
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "nonfinite or out-of-range XYZ");
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return status(error, OBS_AGENT_BUSY,
                  "engine GPU mutations are not admitted");
  SdfPrimitive *sdf = NULL;
  gate = find_sphere(ref, &sdf, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  bool changed = false;
  for (unsigned i = 0; i < 3; ++i)
    changed |= sdf->inverseTransform[3][i] != -(float)xyz[i];
  if (changed) {
    /* The agent creates translation-only spheres; do not touch other rows. */
    for (unsigned i = 0; i < 3; ++i)
      sdf->inverseTransform[3][i] = -(float)xyz[i];
    flushSdfSSBO();
  }
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_sphere_set_material(ObsAgentEngine *engine,
                                             ObsAgentSdfRef ref,
                                             const double rgba[4],
                                             double roughness, double metallic,
                                             ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!rgba || !unit_factor(roughness) || !unit_factor(metallic))
    return status(error, OBS_AGENT_INVALID_ARGUMENT, "invalid PBR factors");
  for (unsigned i = 0; i < 4; ++i)
    if (!unit_factor(rgba[i]))
      return status(error, OBS_AGENT_INVALID_ARGUMENT, "invalid RGBA factor");
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return status(error, OBS_AGENT_BUSY,
                  "engine GPU mutations are not admitted");
  SdfPrimitive *sdf = NULL;
  gate = find_sphere(ref, &sdf, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  bool changed =
      sdf->roughness != (float)roughness || sdf->metallic != (float)metallic;
  for (unsigned i = 0; i < 4; ++i)
    changed |= sdf->color[i] != (float)rgba[i];
  if (changed) {
    for (unsigned i = 0; i < 4; ++i)
      sdf->color[i] = (float)rgba[i];
    sdf->roughness = (float)roughness;
    sdf->metallic = (float)metallic;
    flushSdfSSBO();
  }
  return status(error, OBS_AGENT_OK, "");
}

ObsAgentStatus obs_agent_scene_sphere_count(ObsAgentEngine *engine,
                                            uint32_t *out,
                                            ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!out || scene->sdf_count > MAX_SDF_PRIMITIVES ||
      scene->sdf_count > scene->sdf_capacity)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "invalid scene count output");
  *out = scene->sdf_count;
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_scene_sphere_at(ObsAgentEngine *engine, uint32_t index,
                                         ObsAgentSphere *out,
                                         ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!out || !scene->sdfs || index >= scene->sdf_count ||
      scene->sdf_count > scene->sdf_capacity ||
      scene->sdf_count > MAX_SDF_PRIMITIVES ||
      scene->sdfs[index].type != SDF_TYPE_SPHERE)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "invalid scene sphere index");
  const SdfPrimitive *sdf = &scene->sdfs[index];
  ObsAgentSphere v = {0};
  for (unsigned i = 0; i < 3; ++i) {
    v.position[i] = -(double)sdf->inverseTransform[3][i];
    if (!isfinite(v.position[i]))
      return status(error, OBS_AGENT_STALE_REFERENCE,
                    "invalid scene transform");
  }
  v.radius = sdf->size[0];
  for (unsigned i = 0; i < 4; ++i)
    v.rgba[i] = sdf->color[i];
  v.roughness = sdf->roughness;
  v.metallic = sdf->metallic;
  *out = v;
  return status(error, OBS_AGENT_OK, "");
}

ObsAgentStatus
obs_agent_scene_pick_ray(ObsAgentEngine *engine, const double origin[3],
                         const double direction[3], double max_distance,
                         ObsAgentRayHit *out, ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!origin || !direction || !out || !isfinite(max_distance) ||
      max_distance <= 0 || max_distance > (double)FLT_MAX)
    return status(error, OBS_AGENT_INVALID_ARGUMENT, "invalid ray or output");
  double length = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (!world_coord(origin[i]) || !world_coord(direction[i]))
      return status(error, OBS_AGENT_INVALID_ARGUMENT,
                    "nonfinite ray component");
    length = hypot(length, direction[i]);
  }
  if (length == 0 || !isfinite(length))
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "zero/invalid ray direction");
  ObsSculptRayHit hit;
  if (!obs_sculpt_ray_cast(origin, direction, max_distance, &hit))
    return status(error, OBS_AGENT_NO_HIT, "no translated sphere intersected");
  ObsAgentRayHit result = {.ref = {.ticket = hit.ticket},
                           .index = hit.index,
                           .distance = hit.distance};
  for (unsigned i = 0; i < 3; ++i) {
    result.position[i] = hit.position[i];
    result.normal[i] = hit.normal[i];
  }
  *out = result;
  return status(error, OBS_AGENT_OK, "");
}

/* Real scene edit journal, also used by compiled Monad host callbacks. */
static ObsAgentStatus inspector_status(ObsSdfEditStatus result,
                                       ObsAgentError *error) {
  switch (result) {
  case OBS_SDF_EDIT_OK:
    return status(error, OBS_AGENT_OK, "");
  case OBS_SDF_EDIT_STALE:
    return status(error, OBS_AGENT_STALE_REFERENCE,
                  "selected SDF capability has expired");
  case OBS_SDF_EDIT_EMPTY:
    return status(error, OBS_AGENT_HISTORY_EMPTY,
                  "no edit available in this direction");
  case OBS_SDF_EDIT_FULL:
    return status(error, OBS_AGENT_RESOURCE_LIMIT,
                  "preview group reached 64 edits");
  case OBS_SDF_EDIT_CONFLICT:
    return status(error, OBS_AGENT_EDIT_CONFLICT,
                  "object changed since recorded edit; undo/redo refused");
  default:
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "invalid inspector edit or selection");
  }
}
static ObsAgentStatus edit_admit(ObsAgentEngine *engine, ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return status(error, OBS_AGENT_BUSY, "inspector GPU mutation not admitted");
  return OBS_AGENT_OK;
}
ObsAgentStatus obs_agent_scene_select(ObsAgentEngine *engine,
                                      ObsAgentSdfRef ref,
                                      ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_select(ref.ticket), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_selected(ObsAgentEngine *engine,
                                        ObsAgentSdfRef *out,
                                        ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!out)
    return status(error, OBS_AGENT_INVALID_ARGUMENT,
                  "missing selection output");
  uint64_t selected = 0;
  gate = inspector_status(obs_sdf_inspector_selected(&selected), error);
  if (gate == OBS_AGENT_OK)
    out->ticket = selected;
  return gate;
}
ObsAgentStatus obs_agent_scene_edit_selected(ObsAgentEngine *engine,
                                             uint32_t fields,
                                             const ObsAgentSphere *values,
                                             ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!values || !fields ||
      (fields & ~(OBS_AGENT_EDIT_POSITION | OBS_AGENT_EDIT_MATERIAL)))
    return status(error, OBS_AGENT_INVALID_ARGUMENT, "invalid edit fields");
  ObsSdfEditValues desired = {.roughness = values->roughness,
                              .metallic = values->metallic};
  for (unsigned i = 0; i < 3; ++i)
    desired.xyz[i] = values->position[i];
  for (unsigned i = 0; i < 4; ++i)
    desired.rgba[i] = values->rgba[i];
  return inspector_status(obs_sdf_inspector_edit(fields, &desired), error);
}
ObsAgentStatus obs_agent_scene_undo(ObsAgentEngine *engine,
                                    ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_undo(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_undo_selected(ObsAgentEngine *engine,
                                    ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
      ? inspector_status(obs_sdf_inspector_undo_selected(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_undo_only(ObsAgentEngine *engine,
                                         ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_undo_only(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_command_boundary(ObsAgentEngine *engine,
                                                ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  obs_sdf_inspector_command_boundary();
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_scene_begin_group(ObsAgentEngine *engine,
                                           ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_begin_group(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_end_group(ObsAgentEngine *engine,
                                         ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_end_group(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_abort_group(ObsAgentEngine *engine,
                                           ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_abort_group(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_redo(ObsAgentEngine *engine,
                                    ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  return gate == OBS_AGENT_OK
             ? inspector_status(obs_sdf_inspector_redo(), error)
             : gate;
}
ObsAgentStatus obs_agent_scene_history(ObsAgentEngine *engine,
                                       ObsAgentEditHistory *out,
                                       ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (!out)
    return status(error, OBS_AGENT_INVALID_ARGUMENT, "missing history output");
  const ObsSdfInspectorState h = obs_sdf_inspector_state();
  *out = (ObsAgentEditHistory){.selected = {.ticket = h.selection},
                               .undo_count = h.undo_count,
                               .redo_count = h.redo_count};
  return status(error, OBS_AGENT_OK, "");
}
ObsAgentStatus obs_agent_scene_discard_history(ObsAgentEngine *engine,
                                               ObsAgentError *error) {
  ObsAgentStatus gate = edit_admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  return inspector_status(obs_sdf_inspector_clear(), error);
}

ObsAgentStatus obs_agent_close(ObsAgentEngine *engine, ObsAgentError *error) {
  ObsAgentStatus gate = admit(engine, error);
  if (gate != OBS_AGENT_OK)
    return gate;
  if (context.device && vkDeviceWaitIdle(context.device) != VK_SUCCESS)
    return status(error, OBS_AGENT_SHUTDOWN_FAILED,
                  "GPU quiescence failed; session retained for retry");
  obs_monad_engine_stop();
  if (obs_monad_engine_ready())
    return status(
        error, OBS_AGENT_SHUTDOWN_FAILED,
        "compiler runtime still owns live callbacks; session retained");
  cleanup(&context);
  s_agent = NULL;
  free(engine);
  return status(error, OBS_AGENT_OK, "");
}
