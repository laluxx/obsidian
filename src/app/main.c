#define _POSIX_C_SOURCE 200809L
/// main — app subsystem
#include "add_menu.h"
#include "bindings.h"
#include "camera.h"
#include "common.h"
#include "context.h"
#include "easing.h"
#include "editor.h"
#include "editor_frames.h"
#include "font.h"
#include "gizmo.h"
#include "keychords.h"
#include "renderer.h"
#include "theme.h"
#include "vertico.h"
#include "vulkan_setup.h"
#include "window.h"
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../monad/runtime.h"
#include "benchmark_manifest.h"
#include "engine_bench.h"
#include "gltf_async.h"
#include "gltf_loader.h"
#include "headless_job.h"
#include "live_editor.h"
#include "middle_orbit.h"
#include "obj.h"
#include "repl_panel.h"
#include "scene_buffers.h"
#include "sdf.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/// Implementation

vec4 red = {1.0f, 0.0f, 0.0f, 1.0f};
vec4 green = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};
static bool s_headless_test_matcap;
static ObsObjAsync *s_headless_obj_async;
static bool s_headless_test_obj_async;
static bool s_headless_obj_passed;

static bool parse_bench_count(const char *name, uint32_t fallback,
                              uint32_t minimum, uint32_t maximum,
                              uint32_t *out) {
  const char *value = getenv(name);
  if (!value) {
    *out = fallback;
    return true;
  }
  if (!value[0] || value[0] == '-')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno || !end || *end || parsed < minimum || parsed > maximum)
    return false;
  *out = (uint32_t)parsed;
  return true;
}

static bool parse_bench_float(const char *name, float *out) {
  const char *value = getenv(name);
  if (!value)
    return false;
  errno = 0;
  char *end = NULL;
  float parsed = strtof(value, &end);
  if (errno || end == value || !end || *end || !isfinite(parsed) ||
      parsed < 0.0f)
    return false;
  *out = parsed;
  return true;
}

static bool parse_dimension(const char *text, uint32_t *out) {
  if (!text || !*text || *text == '-')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !end || *end || value == 0 || value > 8192)
    return false;
  *out = (uint32_t)value;
  return true;
}

static void record_benchmark_work_stages(ObsEngineBench *bench,
                                         const VulkanContext *ctx) {
  if (!bench->work_counters_enabled)
    return;
  for (uint32_t stage = 0; stage < OBS_GPU_PROFILE_STAGE_COUNT; ++stage) {
    bench->work_draw_calls_by_stage[stage] +=
        ctx->profileDrawCallsByStage[stage];
    bench->work_indirect_draws_by_stage[stage] +=
        ctx->profileIndirectDrawsByStage[stage];
    bench->work_vertices_by_stage[stage] += ctx->profileVerticesByStage[stage];
    bench->work_indices_by_stage[stage] += ctx->profileIndicesByStage[stage];
    bench->work_mesh_task_workgroups_by_stage[stage] +=
        ctx->profileMeshTaskWorkgroupsByStage[stage];
    bench->work_dispatches_by_stage[stage] +=
        ctx->profileDispatchesByStage[stage];
    bench->work_compute_workgroups_by_stage[stage] +=
        ctx->profileComputeWorkgroupsByStage[stage];
  }
}

static void draw_headless_sample(void *userdata) {
  (void)userdata;
  if (s_headless_test_obj_async && !s_headless_obj_passed &&
      s_headless_obj_async) {
    ObsObjAsyncStatus status = OBS_OBJ_ASYNC_PENDING;
    int32_t mesh_index = -1;
    for (unsigned attempt = 0;
         attempt < 5000 && status == OBS_OBJ_ASYNC_PENDING; ++attempt) {
      status = obs_obj_async_poll(s_headless_obj_async, &mesh_index);
      if (status == OBS_OBJ_ASYNC_PENDING) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&delay, NULL);
      }
    }
    s_headless_obj_passed = status == OBS_OBJ_ASYNC_PUBLISHED &&
                            mesh_index >= 0 &&
                            (size_t)mesh_index < scene->meshes.count &&
                            scene->meshes.items[mesh_index].vertexCount == 3;
    if (s_headless_obj_passed)
      fprintf(stdout,
              "Async OBJ worker parse, renderer-thread upload and scene "
              "publication PASS\n");
    else
      fprintf(stderr, "Async OBJ load did not publish the fixture triangle\n");
  }
  cube((vec3){0.0f, 0.0f, 0.0f}, 3.0f, WHITE);
  if (s_headless_test_matcap && scene->meshes.count > 0) {
    scene->meshes.items[scene->meshes.count - 1].matcap_mode = 1;
    markMeshesSSBODirty(&context);
  }
  const float axis_length = 10000.0f;
  line((vec3){-axis_length, 0, 0}, (vec3){axis_length, 0, 0}, CT.x);
  line((vec3){0, -axis_length, 0}, (vec3){0, axis_length, 0}, CT.y);
  line((vec3){0, 0, -axis_length}, (vec3){0, 0, axis_length}, CT.z);
  quad2D((vec2){40, 40}, (vec2){80, 60}, RED);
}

static bool headless_middle_orbit_fixture(void) {
  Camera saved = camera;
  ObsMiddleOrbitDrag drag = {0};
  vec3 pivot = {0.0f, 0.0f, 0.0f};
  const float distance = glm_vec3_distance(saved.position, pivot);
  obs_middle_orbit_begin(&drag, 100.0, 100.0);
  bool valid =
      obs_middle_orbit_motion(&drag, 100.0, 100.0, &camera, (vec3){0, 0, 0}) &&
      fabsf(camera.yaw - saved.yaw) < 0.001f &&
      fabsf(camera.pitch - saved.pitch) < 0.001f;

  valid = valid &&
          obs_middle_orbit_motion(&drag, 110.0, 120.0, &camera, pivot) &&
          fabsf(camera.yaw - (saved.yaw + glm_deg(0.1f))) < 0.02f &&
          fabsf(camera.pitch - (saved.pitch - glm_deg(0.2f))) < 0.02f &&
          camera.position[1] > saved.position[1] &&
          fabsf(glm_vec3_distance(camera.position, pivot) - distance) < 0.001f;

  valid = valid &&
          obs_middle_orbit_motion(&drag, 100.0, 100.0, &camera, pivot) &&
          fabsf(camera.yaw - saved.yaw) < 0.02f &&
          fabsf(camera.pitch - saved.pitch) < 0.02f;

  obs_middle_orbit_end(&drag);
  const Camera released = camera;
  valid = valid &&
          !obs_middle_orbit_motion(&drag, 120.0, 120.0, &camera, pivot) &&
          fabsf(camera.yaw - released.yaw) < 0.001f &&
          fabsf(camera.pitch - released.pitch) < 0.001f;

  obs_middle_orbit_begin(&drag, 500.0, 500.0);
  valid = valid &&
          obs_middle_orbit_motion(&drag, 500.0, 1500.0, &camera, pivot) &&
          fabsf(camera.pitch + 89.9f) < 0.02f;
  valid = valid &&
          obs_middle_orbit_motion(&drag, 1500.0, 1500.0, &camera, pivot) &&
          camera.yaw >= -180.0f && camera.yaw <= 180.0f;

  obs_middle_orbit_end(&drag);
  camera = saved;
  obs_middle_orbit_begin(&drag, 40.0, 40.0);
  valid = valid && obs_middle_orbit_motion(&drag, 60.0, 50.0, &camera, pivot) &&
          fabsf(camera.yaw - saved.yaw) > 0.5f &&
          fabsf(camera.pitch - saved.pitch) > 0.5f;
  obs_middle_orbit_end(&drag);
  if (!valid) {
    fprintf(stderr, "Headless middle-orbit trace violated camera invariants\n");
    return false;
  }
  fprintf(stdout, "Monad middle-orbit trace: anchor, both drag signs, release, "
                  "pitch clamp, yaw wrap PASS\n");
  return true;
}

static void benchmark_capture_gpu_memory(ObsEngineBench *bench, bool initial) {
  VkDeviceSize usage[VK_MAX_MEMORY_HEAPS] = {0};
  VkDeviceSize budget[VK_MAX_MEMORY_HEAPS] = {0};
  uint32_t heap_count = 0;
  if (!obs_vk_memory_budget_snapshot(&context, &heap_count, usage, budget))
    return;
  bench->gpu_memory_budget_supported = true;
  bench->gpu_memory_heap_count = heap_count < OBS_ENGINE_BENCH_MAX_GPU_HEAPS
                                     ? heap_count
                                     : OBS_ENGINE_BENCH_MAX_GPU_HEAPS;
  for (uint32_t i = 0; i < bench->gpu_memory_heap_count; ++i) {
    if (initial)
      bench->gpu_memory_initial_usage[i] = usage[i];
    else {
      bench->gpu_memory_final_usage[i] = usage[i];
      bench->gpu_memory_final_budget[i] = budget[i];
    }
  }
}

static void benchmark_capture_engine_gpu_allocations(ObsEngineBench *bench,
                                                     bool initial) {
  ObsVkMemoryStats stats = {0};
  if (!obs_vk_memory_tracker_snapshot(context.device, &stats)) {
    bench->engine_gpu_allocations_sampled = false;
    return;
  }
  uint32_t heap_count = stats.heap_count < OBS_ENGINE_BENCH_MAX_GPU_HEAPS
                            ? stats.heap_count
                            : OBS_ENGINE_BENCH_MAX_GPU_HEAPS;
  bench->engine_gpu_heap_count = heap_count;
  if (initial) {
    bench->engine_gpu_allocations_sampled = true;
    for (uint32_t i = 0; i < heap_count; ++i) {
      bench->engine_gpu_initial_bytes[i] = stats.live_bytes[i];
      bench->engine_gpu_initial_allocations[i] = stats.live_allocations[i];
    }
    bench->allocation_owner_count = stats.owner_count;
    memcpy(bench->allocation_owners, stats.owners,
           (size_t)stats.owner_count * sizeof(stats.owners[0]));
    return;
  }
  if (!bench->engine_gpu_allocations_sampled)
    return;
  for (uint32_t i = 0; i < heap_count; ++i) {
    bench->engine_gpu_final_bytes[i] = stats.live_bytes[i];
    bench->engine_gpu_peak_bytes[i] = stats.peak_bytes[i];
    bench->engine_gpu_final_allocations[i] = stats.live_allocations[i];
    bench->engine_gpu_peak_allocations[i] = stats.peak_allocations[i];
  }
  bench->allocation_owner_count = stats.owner_count;
  memcpy(bench->allocation_owners, stats.owners,
         (size_t)stats.owner_count * sizeof(stats.owners[0]));
}

typedef struct {
  const char *path;
  bool uses_camera_policy;
  bool antialiasing_transition_test;
  monad_value_t *render_policy;
} ObsRawAgentFrame;

static monad_status_t headless_antialiasing_set(ObsRawAgentFrame *job,
                                                int64_t requested) {
  ObsMonadRuntime *host = obs_monad_engine();
  if (!host || !job || !job->render_policy)
    return MONAD_ERROR_INVALID_ARGUMENT;
  monad_value_t *value = requested == 0 ? monad_value_nil(host->space)
                                        : monad_value_i64_immediate(requested);
  if (!value)
    return MONAD_ERROR_INVALID_ARGUMENT;
  monad_value_t *arguments[] = {job->render_policy, value};
  monad_value_t *result = NULL;
  monad_error_t error = {0};
  monad_status_t status = monad_dynamic_call_named(
      host->space, "render-antialiasing!", 2, arguments, &result, &error);
  if (result)
    monad_value_release(result);
  monad_value_release(value);
  if (status != MONAD_OK) {
    fprintf(stderr, "AA Monad setter rejected request %" PRId64 ": %s (%s)\n",
            requested, monad_status_name(status),
            error.message ? error.message : "no diagnostic");
    return status;
  }

  monad_value_t *query_arguments[] = {job->render_policy};
  monad_value_t *current = NULL;
  status = monad_dynamic_call_named(host->space, "render-antialiasing", 1,
                                    query_arguments, &current, &error);
  if (status != MONAD_OK || !current) {
    if (current)
      monad_value_release(current);
    return status == MONAD_OK ? MONAD_ERROR_INTERNAL : status;
  }
  bool matches = false;
  if (context.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT) {
    matches = monad_value_kind(current) == MONAD_VALUE_NIL;
  } else {
    int64_t actual = 0;
    matches = monad_value_as_i64(current, &actual, &error) == MONAD_OK &&
              actual == (int64_t)context.rasterizationSamples;
  }
  monad_value_release(current);
  return matches ? MONAD_OK : MONAD_ERROR_INTERNAL;
}

/* Called by the bounded agent driver; these are the real engine entry points,
 * not a parallel test renderer or a script interpreter. */
static bool render_agent_frame(uint32_t index, bool last, void *userdata) {
  ObsRawAgentFrame *job = userdata;
  if (job->antialiasing_transition_test) {
    static const int64_t requests[] = {2, 4, 8, 0};
    if (index == 0) {
      VkSampleCountFlagBits initial = context.rasterizationSamples;
      if (headless_antialiasing_set(job, 3) == MONAD_OK ||
          context.rasterizationSamples != initial) {
        fprintf(stderr, "Invalid AA request 3 changed renderer state\n");
        return false;
      }
    }
    if (index >= sizeof(requests) / sizeof(requests[0])) {
      fprintf(stderr, "AA transition test needs exactly four frames\n");
      return false;
    }
    VkSampleCountFlagBits previous = context.rasterizationSamples;
    monad_status_t status = headless_antialiasing_set(job, requests[index]);
    bool applied = status == MONAD_OK;
    VkSampleCountFlagBits requested =
        requests[index] == 0 ? VK_SAMPLE_COUNT_1_BIT
                             : (VkSampleCountFlagBits)requests[index];
    if (applied && context.rasterizationSamples != requested) {
      fprintf(stderr, "AA request %" PRId64 " selected the wrong mode\n",
              requests[index]);
      return false;
    }
    if (!applied && context.rasterizationSamples != previous) {
      fprintf(stderr, "Rejected AA request changed renderer state\n");
      return false;
    }
    if (applied)
      fprintf(stdout, "AA transition %" PRId64 "x: applied\n", requests[index]);
    else if (status == MONAD_ERROR_INVALID_ARGUMENT)
      fprintf(stdout, "AA transition %" PRId64 "x: unsupported/rejected\n",
              requests[index]);
    else {
      fprintf(stderr, "AA setter failed with status %s\n",
              monad_status_name(status));
      return false;
    }
  }
  if (job->uses_camera_policy && !obs_monad_camera_step_checked())
    return false;
  return last ? obs_headless_render_raw(job->path, draw_headless_sample, NULL)
              : obs_headless_render_frame(draw_headless_sample, NULL);
}

static bool headless_editor_host_call(const char *name, size_t argc,
                                      monad_value_t *const *args,
                                      monad_value_t **result) {
  ObsMonadRuntime *host = obs_monad_engine();
  monad_error_t error = {0};
  if (!host || !result ||
      monad_dynamic_call_named(host->space, name, argc, args, result, &error) !=
          MONAD_OK) {
    fprintf(stderr, "Headless editor host call %s failed: %s\n",
            name ? name : "(null)",
            error.message ? error.message : "no diagnostic");
    return false;
  }
  return true;
}

static bool headless_editor_host_bool(const char *name, size_t argc,
                                      monad_value_t *const *args, bool *value) {
  monad_value_t *result = NULL;
  if (!value || !headless_editor_host_call(name, argc, args, &result))
    return false;
  bool valid = monad_value_kind(result) == MONAD_VALUE_BOOL;
  if (valid)
    *value = monad_value_is_true(result) != 0;
  monad_value_release(result);
  if (!valid)
    fprintf(stderr, "Headless editor host call %s did not return Bool\n", name);
  return valid;
}

static bool headless_editor_host_i64(const char *name, size_t argc,
                                     monad_value_t *const *args,
                                     int64_t *value) {
  monad_value_t *result = NULL;
  monad_error_t error = {0};
  if (!value || !headless_editor_host_call(name, argc, args, &result))
    return false;
  bool valid = monad_value_as_i64(result, value, &error) == MONAD_OK;
  monad_value_release(result);
  if (!valid)
    fprintf(stderr, "Headless editor host call %s did not return Int: %s\n",
            name, error.message ? error.message : "no diagnostic");
  return valid;
}

static bool headless_editor_transform_contract(void) {
  const Camera original_camera = camera;
  const float viewport_width = (float)context.drawExtent.width;
  const float viewport_height = (float)context.drawExtent.height;
  vec3 camera_position = {0.0f, 0.0f, 10.0f};
  camera_init(&camera, camera_position, -90.0f, 0.0f,
              viewport_width / viewport_height);
  Mesh fixture = {0};
  vec3 initial_position = {3.0f, 4.0f, -2.0f};
  glm_mat4_identity(fixture.model);
  glm_mat4_identity(fixture.local_transform);
  glm_translate(fixture.model, initial_position);
  glm_translate(fixture.local_transform, initial_position);
  fixture.visible = true;
  fixture.megaBaseVertex = UINT32_MAX;

  const size_t mesh_index = scene->meshes.count;
  if (mesh_index > INT64_MAX) {
    camera = original_camera;
    return false;
  }
  meshes_add(&scene->meshes, fixture);
  if (scene->meshes.count != mesh_index + 1) {
    fprintf(stderr, "Headless editor transform fixture mesh append failed\n");
    camera = original_camera;
    return false;
  }

  bool ok = true;
  monad_value_t *editor_ref = NULL;
  monad_value_t *selection_ref = NULL;
  monad_value_t *mesh_index_value = NULL;
  monad_value_t *select_args[2] = {0};
  monad_value_t *transform_mode = NULL;
  monad_value_t *transform_args[2] = {0};
  monad_value_t *transform_result = NULL;
  monad_error_t error = {0};

  if (!headless_editor_host_call("editor", 0, NULL, &editor_ref) ||
      !headless_editor_host_call("editor-selection", 1, &editor_ref,
                                 &selection_ref) ||
      monad_value_make_i64(obs_monad_engine()->space, (int64_t)mesh_index,
                           &mesh_index_value, &error) != MONAD_OK) {
    ok = false;
    goto cleanup;
  }
  select_args[0] = selection_ref;
  select_args[1] = mesh_index_value;
  bool selected = false;
  if (!headless_editor_host_bool("selection-mesh!", 2, select_args,
                                 &selected) ||
      !selected) {
    fprintf(stderr, "Monad selection-mesh! did not select fixture mesh\n");
    ok = false;
    goto cleanup;
  }
  int64_t selected_index = -1;
  if (!headless_editor_host_i64("selection-mesh-index", 1, &selection_ref,
                                &selected_index) ||
      selected_index != (int64_t)mesh_index) {
    fprintf(stderr, "Monad selection view returned the wrong mesh identity\n");
    ok = false;
    goto cleanup;
  }

  /* Exercise the standard compiler/object-space editor capability in
   * addition to the live keymap evaluator used by editor.mon. */
  transform_args[0] = editor_ref;
  if (monad_value_make_i64(obs_monad_engine()->space,
                           EDITOR_TRANSFORM_TRANSLATE, &transform_mode,
                           &error) != MONAD_OK) {
    ok = false;
    goto cleanup;
  }
  transform_args[1] = transform_mode;
  if (!headless_editor_host_call("editor-transform-begin!", 2, transform_args,
                                 &transform_result) ||
      monad_value_kind(transform_result) != MONAD_VALUE_BOOL ||
      !monad_value_is_true(transform_result)) {
    fprintf(stderr,
            "Typed Monad editor transform capability failed to begin\n");
    ok = false;
    goto cleanup;
  }
  monad_value_release(transform_result);
  transform_result = NULL;
  monad_value_t *cancel_value =
      monad_value_bool(obs_monad_engine()->space, false);
  monad_value_t *commit_args[2] = {editor_ref, cancel_value};
  bool cancelled = false;
  if (!cancel_value ||
      !headless_editor_host_bool("editor-transform-finish!", 2, commit_args,
                                 &cancelled) ||
      !cancelled) {
    fprintf(stderr,
            "Typed Monad editor transform capability failed to cancel\n");
    ok = false;
    monad_value_release(cancel_value);
    goto cleanup;
  }
  monad_value_release(cancel_value);

  const double mouse_start_x = viewport_width * 0.5;
  const double mouse_start_y = viewport_height * 0.5;
  const double mouse_move_x = mouse_start_x + viewport_width * 0.16;
  editor_mouse_move(mouse_start_x, mouse_start_y);
  KeyChordBinding *grab = keychord_find_binding(&keymap, "g");
  if (!grab || grab->action_type != ACTION_MONAD_PROC) {
    fprintf(stderr, "Monad global g transform command is missing\n");
    ok = false;
    goto cleanup;
  }
  keychord_invoke_binding(grab);
  KeyChordBinding *axis_x =
      obs_monad_live_editor_find_binding("transform", "x");
  KeyChordBinding *snap_2 =
      obs_monad_live_editor_find_binding("transform", "2");
  if (!editor_transform_active() || !axis_x || !snap_2) {
    fprintf(stderr,
            "Monad transform map did not enter the x/2 command state\n");
    ok = false;
    goto cleanup;
  }
  keychord_invoke_binding(axis_x);
  keychord_invoke_binding(snap_2);
  editor_mouse_move(mouse_move_x, mouse_start_y);
  editor_update();

  Mesh *selected_mesh = &scene->meshes.items[mesh_index];
  const float dx = selected_mesh->model[3][0] - initial_position[0];
  const bool snapped =
      fabsf(dx) > 0.001f && fabsf(dx / 2.0f - roundf(dx / 2.0f)) < 0.001f;
  if (!snapped ||
      fabsf(selected_mesh->model[3][1] - initial_position[1]) > 0.001f ||
      fabsf(selected_mesh->model[3][2] - initial_position[2]) > 0.001f) {
    fprintf(stderr,
            "Monad g x 2 did not produce a nonzero X-only two-meter snap "
            "(delta %.3f, %.3f, %.3f)\n",
            dx, selected_mesh->model[3][1] - initial_position[1],
            selected_mesh->model[3][2] - initial_position[2]);
    ok = false;
    goto cleanup;
  }

  KeyChordBinding *origin =
      obs_monad_live_editor_find_binding("transform", "0");
  if (!origin) {
    fprintf(stderr, "Monad transform map is missing g 0\n");
    ok = false;
    goto cleanup;
  }
  keychord_invoke_binding(origin);
  selected_mesh = &scene->meshes.items[mesh_index];
  if (editor_transform_active() || fabsf(selected_mesh->model[3][0]) > 0.001f ||
      fabsf(selected_mesh->model[3][1]) > 0.001f ||
      fabsf(selected_mesh->model[3][2]) > 0.001f) {
    fprintf(stderr, "Monad g 0 did not commit the mesh origin at world zero\n");
    ok = false;
    goto cleanup;
  }

  grab = keychord_find_binding(&keymap, "g");
  axis_x = obs_monad_live_editor_find_binding("transform", "x");
  KeyChordBinding *cancel =
      obs_monad_live_editor_find_binding("transform", "C-g");
  editor_mouse_move(mouse_move_x, mouse_start_y);
  if (!grab || !axis_x || !cancel) {
    fprintf(stderr, "Monad transform map is missing cancel/axis bindings\n");
    ok = false;
    goto cleanup;
  }
  keychord_invoke_binding(grab);
  keychord_invoke_binding(axis_x);
  editor_mouse_move(mouse_move_x + viewport_width * 0.08, mouse_start_y);
  editor_update();
  keychord_invoke_binding(cancel);
  selected_mesh = &scene->meshes.items[mesh_index];
  if (editor_transform_active() || fabsf(selected_mesh->model[3][0]) > 0.001f ||
      fabsf(selected_mesh->model[3][1]) > 0.001f ||
      fabsf(selected_mesh->model[3][2]) > 0.001f) {
    fprintf(stderr, "Monad C-g failed to roll back the editor transform\n");
    ok = false;
  }

cleanup:
  if (editor_transform_active())
    editor_transform_finish(false);
  if (selection_ref) {
    monad_value_t *deselect_value = NULL;
    monad_value_t *deselect_args[2] = {selection_ref, NULL};
    if (monad_value_make_i64(obs_monad_engine()->space, -1, &deselect_value,
                             &error) == MONAD_OK) {
      deselect_args[1] = deselect_value;
      bool deselected = false;
      ok &= headless_editor_host_bool("selection-mesh!", 2, deselect_args,
                                      &deselected) &&
            deselected;
    } else
      ok = false;
    monad_value_release(deselect_value);
  }
  monad_value_release(transform_result);
  monad_value_release(transform_mode);
  monad_value_release(mesh_index_value);
  monad_value_release(selection_ref);
  monad_value_release(editor_ref);
  meshes_remove(&scene->meshes, mesh_index);
  camera = original_camera;
  return ok;
}

static bool headless_editor_frame_selection_contract(void) {
  extern bool is_framing;
  extern CameraFrameTransition frame_transition;
  extern float frame_anim_time;
  extern vec3 orbitPivot;
  extern float orbitDistance;
  const Camera previous_camera = camera;
  const CameraFrameTransition previous_transition = frame_transition;
  const bool previous_framing = is_framing;
  const float previous_animation_time = frame_anim_time;
  const float previous_orbit_distance = orbitDistance;
  vec3 previous_orbit_pivot;
  glm_vec3_copy(orbitPivot, previous_orbit_pivot);

  camera_init(&camera, (vec3){0.0f, 0.0f, 12.0f}, -90.0f, 0.0f, 4.0f / 3.0f);
  vec3 initial_front;
  glm_vec3_copy(camera.front, initial_front);
  KeyChordBinding *binding = keychord_find_binding(&keymap, "f");
  bool ok = binding && binding->action_type == ACTION_MONAD_PROC &&
            binding->managed_monad;
  if (ok)
    keychord_invoke_binding(binding);
  const float alignment = glm_vec3_dot(initial_front, camera.front);
  if (!ok || !isfinite(alignment) || alignment < 0.99999f) {
    fprintf(stderr, "Monad f changed the initial camera direction (dot %.7f)\n",
            alignment);
    ok = false;
  }

  camera = previous_camera;
  frame_transition = previous_transition;
  is_framing = previous_framing;
  frame_anim_time = previous_animation_time;
  orbitDistance = previous_orbit_distance;
  glm_vec3_copy(previous_orbit_pivot, orbitPivot);
  return ok;
}

static bool headless_editor_sdf_selection_contract(void) {
  extern int g_selected_sdf_index;
  const Camera previous_camera = camera;
  extern bool is_framing;
  extern CameraFrameTransition frame_transition;
  extern float frame_anim_time;
  extern vec3 orbitPivot;
  extern float orbitDistance;
  const CameraFrameTransition previous_transition = frame_transition;
  const bool previous_framing = is_framing;
  const float previous_frame_anim_time = frame_anim_time;
  const float previous_orbit_distance = orbitDistance;
  const int previous_hierarchy_selection = editor.hierarchy.selected_index;
  vec3 previous_orbit_pivot;
  glm_vec3_copy(orbitPivot, previous_orbit_pivot);
  Scene *previous_scene = scene;
  const int previous_sdf = g_selected_sdf_index;
  const int previous_mesh = editor.inspector.selected_mesh_index;
  const VkExtent2D previous_extent = context.drawExtent;
  const bool previous_gizmo_active = gizmo.active;
  Scene fixture = {0};
  scene = &fixture;
  scene_init(&fixture);
  g_selected_sdf_index = -1;
  editor.inspector.selected_mesh_index = -1;
  camera_init(&camera, (vec3){0.0f, 0.0f, 12.0f}, -90.0f, 0.0f,
              640.0f / 480.0f);
  context.drawExtent = (VkExtent2D){640, 480};

  bool ok = obs_sdf_add_checked(&fixture, "Focus SDF", SDF_TYPE_BOX,
                                (vec3){7.0f, 3.0f, -2.0f},
                                (vec4){1.0f, 2.0f, 0.5f, 0.0f},
                                (vec4){0.8f, 0.2f, 0.2f, 1.0f}) == 0;
  if (ok) {
    g_selected_sdf_index = 0;
    editor_frame_selected();
    extern CameraFrameTransition frame_transition;
    if (glm_vec3_distance(frame_transition.target_look_at,
                          (vec3){7.0f, 3.0f, -2.0f}) > 0.0001f) {
      fprintf(stderr, "F did not frame the selected SDF center\n");
      ok = false;
    }
  }

  g_selected_sdf_index = -1;
  if (ok) {
    ok = obs_sdf_add_checked(&fixture, "Box Select A", SDF_TYPE_SPHERE,
                             (vec3){-2.0f, 0.0f, 0.0f},
                             (vec4){0.7f, 0.0f, 0.0f, 0.0f},
                             (vec4){0.2f, 0.8f, 0.2f, 1.0f}) == 1 &&
         obs_sdf_add_checked(&fixture, "Box Select B", SDF_TYPE_BOX,
                             (vec3){2.0f, 0.0f, 0.0f},
                             (vec4){0.5f, 0.5f, 0.5f, 0.0f},
                             (vec4){0.2f, 0.2f, 0.8f, 1.0f}) == 2;
  }
  if (ok) {
    ok = hierarchy_select_additive_sdf(1) && hierarchy_select_additive_sdf(2) &&
         fixture.tree.nodes[3].selected && fixture.tree.nodes[5].selected &&
         g_selected_sdf_index == 2 && editor.hierarchy.selected_index == 5;
    if (!ok)
      fprintf(
          stderr,
          "Shift-additive SDF selection did not preserve prior selection\n");
  }
  if (ok) {
    editor_selection_box_begin(0.0f, 0.0f);
    editor_selection_box_update(640.0f, 480.0f);
    editor_selection_box_end(640.0f, 480.0f);
    ok = fixture.tree.nodes[3].selected && fixture.tree.nodes[5].selected &&
         editor.inspector.selected_mesh_index == -1 &&
         g_selected_sdf_index == 1 && !gizmo.active;
    if (!ok)
      fprintf(stderr,
              "Background box selection did not select both visible SDFs\n");
  }
  if (ok) {
    editor_selection_box_begin(227.0f, 0.0f);
    editor_selection_box_update(220.0f, 480.0f);
    editor_selection_box_end(220.0f, 480.0f);
    ok = fixture.tree.nodes[3].selected && !fixture.tree.nodes[5].selected &&
         g_selected_sdf_index == 1 && gizmo.active;
    if (!ok)
      fprintf(stderr,
              "Right-to-left marquee did not select intersecting bounds\n");
  }
  if (ok) {
    editor_selection_box_begin(220.0f, 0.0f);
    editor_selection_box_update(227.0f, 480.0f);
    editor_selection_box_end(227.0f, 480.0f);
    ok = !fixture.tree.nodes[3].selected && !fixture.tree.nodes[5].selected &&
         g_selected_sdf_index == -1;
    if (!ok)
      fprintf(stderr,
              "Left-to-right marquee did not require complete containment\n");
  }
  if (ok) {
    editor_selection_box_begin(0.0f, 0.0f);
    editor_selection_box_update(640.0f, 480.0f);
    editor_selection_box_end(640.0f, 480.0f);
    ok = fixture.tree.nodes[3].selected && fixture.tree.nodes[5].selected &&
         g_selected_sdf_index == 1 && !gizmo.active;
  }

  scene = previous_scene;
  scene_cleanup(&fixture);
  flushSdfSSBO();
  camera = previous_camera;
  frame_transition = previous_transition;
  is_framing = previous_framing;
  frame_anim_time = previous_frame_anim_time;
  orbitDistance = previous_orbit_distance;
  glm_vec3_copy(previous_orbit_pivot, orbitPivot);
  editor.hierarchy.selected_index = previous_hierarchy_selection;
  context.drawExtent = previous_extent;
  gizmo.active = previous_gizmo_active;
  g_selected_sdf_index = previous_sdf;
  editor.inspector.selected_mesh_index = previous_mesh;
  return ok;
}

static bool headless_editor_monad_hierarchy_selection_contract(void) {
  ObsMonadHierarchySelectionAction action;
  int32_t anchor = 0;
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false)) {
    fprintf(stderr, "Monad hierarchy anchor initial read failed\n");
    return false;
  }
  const int32_t initial_anchor = anchor;
  anchor = -2;
  if (obs_monad_live_editor_hierarchy_anchor(&anchor, true)) {
    fprintf(stderr, "Monad accepted an invalid hierarchy anchor\n");
    return false;
  }
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false) ||
      anchor != initial_anchor) {
    fprintf(stderr, "Monad invalid hierarchy anchor changed stored state\n");
    return false;
  }
  anchor = 10;
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, true)) {
    fprintf(stderr, "Monad hierarchy anchor setup failed\n");
    return false;
  }
  bool ok =
      obs_monad_live_editor_hierarchy_selection(13, false, true, 0, &action);
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false)) {
    fprintf(stderr, "Monad hierarchy range anchor query failed\n");
    return false;
  }
  if (!ok || action != OBS_MONAD_HIERARCHY_SELECT_RANGE || anchor != 10) {
    fprintf(stderr, "Monad hierarchy Shift-range policy failed\n");
    return false;
  }
  int32_t first_row = -1;
  int32_t last_row = -1;
  ok = obs_monad_live_editor_hierarchy_range_bounds(4, 1, 5, &first_row,
                                                    &last_row);
  if (!ok || first_row != 1 || last_row != 4) {
    fprintf(stderr, "Monad reverse hierarchy range bounds failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_range_bounds(-1, 2, 5, &first_row,
                                                    &last_row);
  if (!ok || first_row != -1 || last_row != -1) {
    fprintf(stderr, "Monad invalid hierarchy range was not rejected\n");
    return false;
  }
  bool clear_others = false;
  bool next_selected = false;
  bool next_selected_group = false;
  ok = obs_monad_live_editor_hierarchy_node_selection(
      OBS_MONAD_HIERARCHY_SELECT_SINGLE, false, true, &clear_others,
      &next_selected, &next_selected_group);
  if (!ok || !clear_others || !next_selected || !next_selected_group) {
    fprintf(stderr, "Monad single hierarchy selection effect failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_node_selection(
      OBS_MONAD_HIERARCHY_SELECT_TOGGLE, true, true, &clear_others,
      &next_selected, &next_selected_group);
  if (!ok || clear_others || next_selected || next_selected_group) {
    fprintf(stderr, "Monad hierarchy deselection toggle effect failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_node_selection(
      OBS_MONAD_HIERARCHY_SELECT_APPEND, false, false, &clear_others,
      &next_selected, &next_selected_group);
  if (!ok || clear_others || !next_selected || next_selected_group) {
    fprintf(stderr, "Monad hierarchy append effect failed\n");
    return false;
  }
  ObsMonadGltfSelectionTransition gltf_transition;
  ok = obs_monad_live_editor_hierarchy_gltf_selection_transition(
      2, 3, false, &gltf_transition);
  if (!ok || gltf_transition != OBS_MONAD_GLTF_SELECTION_NONE) {
    fprintf(stderr, "Monad partial glTF selection promotion failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_gltf_selection_transition(
      3, 3, false, &gltf_transition);
  if (!ok || gltf_transition != OBS_MONAD_GLTF_SELECTION_PROMOTE) {
    fprintf(stderr, "Monad complete glTF selection promotion failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_gltf_selection_transition(
      2, 3, true, &gltf_transition);
  if (!ok || gltf_transition != OBS_MONAD_GLTF_SELECTION_DEMOTE) {
    fprintf(stderr, "Monad glTF group selection demotion precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_gltf_selection_transition(
      0, 0, false, &gltf_transition);
  if (!ok || gltf_transition != OBS_MONAD_GLTF_SELECTION_NONE) {
    fprintf(stderr, "Monad empty glTF was promoted unexpectedly\n");
    return false;
  }
  float scroll_target = -1.0f;
  ok = obs_monad_live_editor_tree_selection_scroll_target(
      180.0f, 30.0f, 100.0f, 50.0f, &scroll_target);
  if (!ok || fabsf(scroll_target - 130.0f) > 0.0001f) {
    fprintf(stderr, "Monad hierarchy below-viewport scroll target failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_selection_scroll_target(
      90.0f, 30.0f, 100.0f, 50.0f, &scroll_target);
  if (!ok || fabsf(scroll_target - 50.0f) > 0.0001f) {
    fprintf(stderr, "Monad visible hierarchy row changed scroll target\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_selection_scroll_target(
      10.0f, 30.0f, 100.0f, 30.0f, &scroll_target);
  if (!ok || fabsf(scroll_target) > 0.0001f) {
    fprintf(stderr, "Monad hierarchy scroll target did not clamp to zero\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_scroll_center_target(100.0f, 80.0f,
                                                       &scroll_target);
  if (!ok || fabsf(scroll_target - 60.0f) > 0.0001f) {
    fprintf(stderr, "Monad search-result center target failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_scroll_center_target(20.0f, 80.0f,
                                                       &scroll_target);
  if (!ok || fabsf(scroll_target) > 0.0001f) {
    fprintf(stderr,
            "Monad search-result center target did not clamp to zero\n");
    return false;
  }
  float beam_row = -1.0f;
  ok = obs_monad_live_editor_tree_search_beam_row(2.0f, 6.0f, 0.5f, &beam_row);
  if (!ok || fabsf(beam_row - 5.5f) > 0.0001f) {
    fprintf(stderr, "Monad tree-search beam interpolation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_search_beam_row(2.0f, 6.0f, -0.2f, &beam_row);
  if (!ok || fabsf(beam_row - 2.0f) > 0.0001f) {
    fprintf(stderr, "Monad tree-search beam lower endpoint clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_search_beam_row(2.0f, 6.0f, 1.2f, &beam_row);
  if (!ok || fabsf(beam_row - 6.0f) > 0.0001f) {
    fprintf(stderr, "Monad tree-search beam upper endpoint clamp failed\n");
    return false;
  }
  int search_index = -9;
  ok = obs_monad_live_editor_filesystem_search_next_index(1, 3, 1,
                                                          &search_index);
  if (!ok || search_index != 2) {
    fprintf(stderr, "Monad filesystem search next-index policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_search_next_index(2, 3, 1,
                                                          &search_index);
  if (!ok || search_index != 0) {
    fprintf(stderr, "Monad filesystem search next wrap failed\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_search_next_index(0, 3, -1,
                                                          &search_index);
  if (!ok || search_index != 2) {
    fprintf(stderr, "Monad filesystem search previous wrap failed\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_search_next_index(-1, 3, 1,
                                                          &search_index);
  if (!ok || search_index != 0) {
    fprintf(stderr, "Monad filesystem search initial result failed\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_search_next_index(0, 0, -1,
                                                          &search_index);
  if (!ok || search_index != -1) {
    fprintf(stderr, "Monad empty filesystem search navigation failed\n");
    return false;
  }
  float text_scroll_target = -1.0f;
  ok = obs_monad_live_editor_text_editor_wheel_scroll_target(
      20.0f, 2.0f, 10.0f, &text_scroll_target);
  if (!ok || fabsf(text_scroll_target) > 0.0001f) {
    fprintf(stderr, "Monad text-editor wheel-scroll lower bound failed\n");
    return false;
  }
  ok = obs_monad_live_editor_text_editor_wheel_scroll_target(
      20.0f, -1.0f, 10.0f, &text_scroll_target);
  if (!ok || fabsf(text_scroll_target - 50.0f) > 0.0001f) {
    fprintf(stderr, "Monad text-editor wheel-scroll line step failed\n");
    return false;
  }
  int gizmo_hover_part = 8;
  ok = obs_monad_live_editor_gizmo_rotation_hover_priority(8, true,
                                                           &gizmo_hover_part);
  if (!ok || gizmo_hover_part != 9) {
    fprintf(stderr, "Monad gizmo trackball-over-arc priority failed\n");
    return false;
  }
  ok = obs_monad_live_editor_gizmo_rotation_hover_priority(8, false,
                                                           &gizmo_hover_part);
  if (!ok || gizmo_hover_part != 8) {
    fprintf(stderr, "Monad gizmo arc hover preservation failed\n");
    return false;
  }
  int gizmo_drag = GIZMO_PART_NONE;
  ok = obs_monad_live_editor_gizmo_pointer_transition(
      GIZMO_PART_NONE, GIZMO_PART_RY, true, false, true, &gizmo_drag);
  if (!ok || gizmo_drag != GIZMO_PART_RY) {
    fprintf(stderr, "Monad gizmo drag activation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_gizmo_pointer_transition(
      gizmo_drag, GIZMO_PART_RY, true, false, false, &gizmo_drag);
  if (!ok || gizmo_drag != GIZMO_PART_NONE) {
    fprintf(stderr, "Monad gizmo inactive press retained a stale drag\n");
    return false;
  }
  int gizmo_press_action = 0;
  ok = obs_monad_live_editor_gizmo_press_action(GIZMO_PART_RY, true,
                                                &gizmo_press_action);
  if (!ok || gizmo_press_action != 1) {
    fprintf(stderr, "Monad gizmo press did not prioritize handle drag\n");
    return false;
  }
  ok = obs_monad_live_editor_gizmo_press_action(GIZMO_PART_NONE, true,
                                                &gizmo_press_action);
  if (!ok || gizmo_press_action != 2) {
    fprintf(stderr, "Monad gizmo press did not select hovered bone\n");
    return false;
  }
  ok = obs_monad_live_editor_gizmo_press_action(GIZMO_PART_NONE, false,
                                                &gizmo_press_action);
  if (!ok || gizmo_press_action != 3) {
    fprintf(stderr, "Monad gizmo press did not fall through to scene pick\n");
    return false;
  }
  gizmo_drag = GIZMO_PART_RY;
  ok = obs_monad_live_editor_gizmo_pointer_transition(
      gizmo_drag, GIZMO_PART_NONE, false, true, false, &gizmo_drag);
  if (!ok || gizmo_drag != GIZMO_PART_NONE) {
    fprintf(stderr, "Monad gizmo drag release failed\n");
    return false;
  }
  int window_action = -1;
  ok = obs_monad_live_editor_ui_window_pointer_action(0, 0, true, true, true,
                                                      true, &window_action);
  if (!ok || window_action != 0) {
    fprintf(stderr, "Monad popup-window non-left press precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_pointer_action(1, 1, true, true, true,
                                                      false, &window_action);
  if (!ok || window_action != 1) {
    fprintf(stderr, "Monad popup-window close-button precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_pointer_action(1, 1, false, true, true,
                                                      false, &window_action);
  if (!ok || window_action != 2) {
    fprintf(stderr, "Monad popup-window title drag action failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_pointer_action(1, 0, false, false, true,
                                                      true, &window_action);
  if (!ok || window_action != 4) {
    fprintf(stderr, "Monad popup-window drag release action failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_pointer_action(1, 1, false, false, false,
                                                      false, &window_action);
  if (!ok || window_action != 3) {
    fprintf(stderr, "Monad popup-window outside-click action failed\n");
    return false;
  }
  float window_anim = -1.0f;
  ok = obs_monad_live_editor_ui_window_animation_time_step(0.8f, 0.1f, false,
                                                           &window_anim);
  if (!ok || fabsf(window_anim - 1.0f) > 0.0001f) {
    fprintf(stderr, "Monad popup-window opening animation clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_animation_time_step(0.2f, 0.1f, true,
                                                           &window_anim);
  if (!ok || fabsf(window_anim) > 0.0001f) {
    fprintf(stderr, "Monad popup-window closing animation clamp failed\n");
    return false;
  }
  float window_position = -1.0f;
  ok = obs_monad_live_editor_ui_window_clamp_axis_position(
      2.0f, 5.0f, 95.0f, 10.0f, 10.0f, &window_position);
  if (!ok || fabsf(window_position - 15.0f) > 0.0001f) {
    fprintf(stderr, "Monad popup-window lower-edge clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_clamp_axis_position(
      95.0f, 5.0f, 95.0f, 10.0f, 10.0f, &window_position);
  if (!ok || fabsf(window_position - 85.0f) > 0.0001f) {
    fprintf(stderr, "Monad popup-window upper-edge clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_clamp_axis_position(
      2.0f, 5.0f, 20.0f, 10.0f, 10.0f, &window_position);
  if (!ok || fabsf(window_position - 2.0f) > 0.0001f) {
    fprintf(stderr, "Monad oversized-window drag policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_ui_window_clamp_axis_position(
      17.0f, 20.0f, 5.0f, 2.0f, 2.0f, &window_position);
  if (!ok || fabsf(window_position - 17.0f) > 0.0001f) {
    fprintf(stderr, "Monad popup-window inverted-bound preservation failed\n");
    return false;
  }
  float animated_scroll = -1.0f;
  float animation_time = -1.0f;
  ok = obs_monad_live_editor_tree_scroll_animation_step(
      0.0f, 100.0f, 0.0f, 0.0f, 1.0f / 6.0f, &animated_scroll, &animation_time);
  if (!ok || fabsf(animated_scroll - 87.5f) > 0.0001f ||
      fabsf(animation_time - 0.5f) > 0.0001f) {
    fprintf(stderr, "Monad tree cubic scroll interpolation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_scroll_animation_step(
      0.0f, 100.0f, 50.0f, 0.9f, 0.1f, &animated_scroll, &animation_time);
  if (!ok || fabsf(animated_scroll - 100.0f) > 0.0001f ||
      fabsf(animation_time - 1.0f) > 0.0001f) {
    fprintf(stderr, "Monad tree scroll completion clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_scroll_animation_step(
      0.0f, 100.0f, 37.0f, 1.0f, 0.1f, &animated_scroll, &animation_time);
  if (!ok || fabsf(animated_scroll - 37.0f) > 0.0001f ||
      fabsf(animation_time - 1.0f) > 0.0001f) {
    fprintf(stderr, "Monad completed tree scroll state changed unexpectedly\n");
    return false;
  }
  bool bounds_match = false;
  ok = obs_monad_live_editor_viewport_marquee_bounds_match(
      20.0f, 30.0f, 40.0f, 50.0f, 20.0f, 30.0f, 40.0f, 50.0f, true,
      &bounds_match);
  if (!ok || !bounds_match) {
    fprintf(stderr, "Monad marquee containment boundary match failed\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_bounds_match(
      19.0f, 30.0f, 40.0f, 50.0f, 20.0f, 30.0f, 40.0f, 50.0f, true,
      &bounds_match);
  if (!ok || bounds_match) {
    fprintf(stderr, "Monad marquee containment included a partial overlap\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_bounds_match(
      19.0f, 30.0f, 40.0f, 50.0f, 20.0f, 30.0f, 40.0f, 50.0f, false,
      &bounds_match);
  if (!ok || !bounds_match) {
    fprintf(stderr, "Monad marquee intersection excluded an overlap\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_bounds_match(
      41.0f, 30.0f, 50.0f, 50.0f, 20.0f, 30.0f, 40.0f, 50.0f, false,
      &bounds_match);
  if (!ok || bounds_match) {
    fprintf(stderr, "Monad marquee accepted a disjoint rectangle\n");
    return false;
  }
  ObsMonadMarqueeTarget marquee_target = OBS_MONAD_MARQUEE_TARGET_NONE;
  int marquee_target_index = -2;
  bool marquee_gizmo_active = true;
  ok = obs_monad_live_editor_viewport_marquee_focus(
      1, 7, -1, &marquee_target, &marquee_target_index, &marquee_gizmo_active);
  if (!ok || marquee_target != OBS_MONAD_MARQUEE_TARGET_MESH ||
      marquee_target_index != 7 || !marquee_gizmo_active) {
    fprintf(stderr, "Monad marquee single-mesh focus result failed\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_focus(
      2, 7, -1, &marquee_target, &marquee_target_index, &marquee_gizmo_active);
  if (!ok || marquee_target != OBS_MONAD_MARQUEE_TARGET_MESH ||
      marquee_target_index != 7 || marquee_gizmo_active) {
    fprintf(stderr, "Monad marquee multi-mesh focus result failed\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_focus(
      1, -1, 3, &marquee_target, &marquee_target_index, &marquee_gizmo_active);
  if (!ok || marquee_target != OBS_MONAD_MARQUEE_TARGET_SDF ||
      marquee_target_index != 3 || !marquee_gizmo_active) {
    fprintf(stderr, "Monad marquee single-SDF focus result failed\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_focus(
      0, -1, -1, &marquee_target, &marquee_target_index, &marquee_gizmo_active);
  if (!ok || marquee_target != OBS_MONAD_MARQUEE_TARGET_NONE ||
      marquee_target_index != -1 || marquee_gizmo_active) {
    fprintf(stderr, "Monad marquee empty selection result failed\n");
    return false;
  }
  bool marquee_active = true;
  ok = obs_monad_live_editor_viewport_marquee_drag_active(2.99f, -2.99f,
                                                          &marquee_active);
  if (!ok || marquee_active) {
    fprintf(stderr, "Monad marquee activated below the drag threshold\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_drag_active(3.0f, 0.0f,
                                                          &marquee_active);
  if (!ok || !marquee_active) {
    fprintf(stderr, "Monad marquee ignored threshold horizontal drag\n");
    return false;
  }
  ok = obs_monad_live_editor_viewport_marquee_drag_active(0.0f, -3.0f,
                                                          &marquee_active);
  if (!ok || !marquee_active) {
    fprintf(stderr, "Monad marquee ignored threshold vertical drag\n");
    return false;
  }
  float filesystem_scroll = 0.0f;
  ok = obs_monad_live_editor_filesystem_tree_wheel_scroll(
      0.0f, -1.0f, 12, 4.0f, 20.0f, &filesystem_scroll);
  if (!ok || fabsf(filesystem_scroll + 20.0f) > 0.0001f) {
    fprintf(stderr, "Monad filesystem wheel did not scroll one row\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_tree_wheel_scroll(
      -150.0f, -1.0f, 12, 4.0f, 20.0f, &filesystem_scroll);
  if (!ok || fabsf(filesystem_scroll + 160.0f) > 0.0001f) {
    fprintf(stderr, "Monad filesystem wheel exceeded scroll bounds\n");
    return false;
  }
  ok = obs_monad_live_editor_filesystem_tree_wheel_scroll(
      0.0f, 1.0f, 12, 4.0f, 20.0f, &filesystem_scroll);
  if (!ok || fabsf(filesystem_scroll) > 0.0001f) {
    fprintf(stderr, "Monad filesystem wheel exceeded upper scroll bound\n");
    return false;
  }
  float animation_scroll = 0.0f;
  ok = obs_monad_live_editor_animation_track_wheel_scroll(
      50.0f, -1.0f, 5.0f, 20.0f, &animation_scroll);
  if (!ok || fabsf(animation_scroll - 90.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation wheel step was incorrect\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_track_wheel_scroll(
      10.0f, 10.0f, 5.0f, 20.0f, &animation_scroll);
  if (!ok || fabsf(animation_scroll) > 0.0001f) {
    fprintf(stderr, "Monad animation wheel exceeded upper scroll bound\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_track_wheel_scroll(
      90.0f, -10.0f, 5.0f, 20.0f, &animation_scroll);
  if (!ok || fabsf(animation_scroll - 100.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation wheel exceeded lower scroll bound\n");
    return false;
  }
  float animation_view_start = -1.0f;
  float animation_view_end = -1.0f;
  ok = obs_monad_live_editor_animation_view_pan(
      0.0f, 5.0f, 10.0f, 1.0f, &animation_view_start, &animation_view_end);
  if (!ok || fabsf(animation_view_start - 0.25f) > 0.0001f ||
      fabsf(animation_view_end - 5.25f) > 0.0001f) {
    fprintf(stderr, "Monad animation timeline pan policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_view_pan(
      0.0f, 5.0f, 4.0f, 100.0f, &animation_view_start, &animation_view_end);
  if (!ok || fabsf(animation_view_start) > 0.0001f ||
      fabsf(animation_view_end - 4.2f) > 0.0001f) {
    fprintf(stderr, "Monad animation timeline bounds clamp failed\n");
    return false;
  }
  int keyframe_action = -1;
  ok = obs_monad_live_editor_animation_keyframe_click_action(8.0f, false,
                                                             &keyframe_action);
  if (!ok || keyframe_action != 1) {
    fprintf(stderr, "Monad animation keyframe hit boundary failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_keyframe_click_action(-8.01f, false,
                                                             &keyframe_action);
  if (!ok || keyframe_action != 0) {
    fprintf(stderr, "Monad animation keyframe miss boundary failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_keyframe_click_action(-8.0f, true,
                                                             &keyframe_action);
  if (!ok || keyframe_action != 2) {
    fprintf(stderr, "Monad keyframe inclusive hit radius failed\n");
    return false;
  }
  float transport_time = -1.0f;
  bool transport_playing = false;
  ok = obs_monad_live_editor_animation_transport_transition(
      0, 4.0f, 10.0f, false, &transport_time, &transport_playing);
  if (!ok || fabsf(transport_time - 4.0f) > 0.0001f || !transport_playing) {
    fprintf(stderr, "Monad animation play transition failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_transport_transition(
      0, 4.0f, 10.0f, true, &transport_time, &transport_playing);
  if (!ok || fabsf(transport_time - 4.0f) > 0.0001f || transport_playing) {
    fprintf(stderr, "Monad animation pause transition failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_transport_transition(
      1, 4.0f, 10.0f, true, &transport_time, &transport_playing);
  if (!ok || fabsf(transport_time) > 0.0001f || transport_playing) {
    fprintf(stderr, "Monad animation stop transition failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_transport_transition(
      2, 4.0f, 10.0f, true, &transport_time, &transport_playing);
  if (!ok || fabsf(transport_time) > 0.0001f || !transport_playing) {
    fprintf(stderr, "Monad animation start transition failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_transport_transition(
      3, 4.0f, 10.0f, false, &transport_time, &transport_playing);
  if (!ok || fabsf(transport_time - 10.0f) > 0.0001f || transport_playing) {
    fprintf(stderr, "Monad animation end transition failed\n");
    return false;
  }
  float playback_view_start = -1.0f;
  float playback_view_end = -1.0f;
  ok = obs_monad_live_editor_animation_playback_view_transition(
      0.2f, 10.0f, 2.0f, 4.0f, true, true, &playback_view_start,
      &playback_view_end);
  if (!ok || fabsf(playback_view_start) > 0.0001f ||
      fabsf(playback_view_end - 2.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation playback loop reset failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_playback_view_transition(
      3.9f, 10.0f, 2.0f, 4.0f, false, true, &playback_view_start,
      &playback_view_end);
  if (!ok || fabsf(playback_view_start - 3.7f) > 0.0001f ||
      fabsf(playback_view_end - 5.7f) > 0.0001f) {
    fprintf(stderr, "Monad animation playback auto-follow failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_playback_view_transition(
      9.8f, 10.0f, 0.0f, 10.0f, false, true, &playback_view_start,
      &playback_view_end);
  if (!ok || fabsf(playback_view_start) > 0.0001f ||
      fabsf(playback_view_end - 10.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation full-view preservation failed\n");
    return false;
  }
  float pointer_time = -1.0f;
  ok = obs_monad_live_editor_animation_pointer_time(50.0f, 100.0f, 0.0f, 10.0f,
                                                    10.0f, &pointer_time);
  if (!ok || fabsf(pointer_time - 5.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation pointer-time mapping failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_time(-10.0f, 100.0f, 0.0f, 10.0f,
                                                    10.0f, &pointer_time);
  if (!ok || fabsf(pointer_time) > 0.0001f) {
    fprintf(stderr, "Monad animation pointer-time lower clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_time(110.0f, 100.0f, 0.0f, 10.0f,
                                                    10.0f, &pointer_time);
  if (!ok || fabsf(pointer_time - 10.0f) > 0.0001f) {
    fprintf(stderr, "Monad animation pointer-time upper clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_time(110.0f, 0.0f, 4.0f, 8.0f,
                                                    10.0f, &pointer_time);
  if (!ok || fabsf(pointer_time - 4.0f) > 0.0001f) {
    fprintf(stderr, "Monad zero-width playhead policy failed\n");
    return false;
  }
  bool playhead_dragging = false;
  ok = obs_monad_live_editor_animation_playhead_drag_transition(
      false, true, false, true, &playhead_dragging);
  if (!ok || !playhead_dragging) {
    fprintf(stderr, "Monad playhead drag activation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_playhead_drag_transition(
      false, true, false, false, &playhead_dragging);
  if (!ok || playhead_dragging) {
    fprintf(stderr, "Monad playhead outside-ruler press failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_playhead_drag_transition(
      true, false, false, false, &playhead_dragging);
  if (!ok || !playhead_dragging) {
    fprintf(stderr, "Monad playhead drag continuation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_playhead_drag_transition(
      true, false, true, true, &playhead_dragging);
  if (!ok || playhead_dragging) {
    fprintf(stderr, "Monad playhead drag release failed\n");
    return false;
  }
  ObsMonadImagePointerState image_pointer = {0};
  ObsMonadImagePointerState next_image_pointer = {0};
  ok = obs_monad_live_editor_image_pointer_transition(
      1, &image_pointer, 10.0, 20.0, &next_image_pointer);
  if (!ok || !next_image_pointer.dragging ||
      fabs(next_image_pointer.last_x - 10.0) > 0.0001 ||
      fabs(next_image_pointer.last_y - 20.0) > 0.0001) {
    fprintf(stderr, "Monad image-editor drag activation failed\n");
    return false;
  }
  image_pointer = next_image_pointer;
  ok = obs_monad_live_editor_image_pointer_transition(
      3, &image_pointer, 15.0, 17.0, &next_image_pointer);
  if (!ok || !next_image_pointer.dragging ||
      fabsf(next_image_pointer.pan_x - 5.0f) > 0.0001f ||
      fabsf(next_image_pointer.pan_y + 3.0f) > 0.0001f ||
      fabs(next_image_pointer.last_x - 15.0) > 0.0001 ||
      fabs(next_image_pointer.last_y - 17.0) > 0.0001) {
    fprintf(stderr, "Monad image-editor drag pan mapping failed\n");
    return false;
  }
  image_pointer = next_image_pointer;
  ok = obs_monad_live_editor_image_pointer_transition(
      2, &image_pointer, 15.0, 17.0, &next_image_pointer);
  if (!ok || next_image_pointer.dragging ||
      fabsf(next_image_pointer.pan_x - 5.0f) > 0.0001f ||
      fabsf(next_image_pointer.pan_y + 3.0f) > 0.0001f) {
    fprintf(stderr, "Monad image-editor drag release failed\n");
    return false;
  }
  image_pointer = next_image_pointer;
  ok = obs_monad_live_editor_image_pointer_transition(
      3, &image_pointer, 30.0, 40.0, &next_image_pointer);
  if (!ok || next_image_pointer.dragging ||
      fabsf(next_image_pointer.pan_x - 5.0f) > 0.0001f ||
      fabsf(next_image_pointer.pan_y + 3.0f) > 0.0001f) {
    fprintf(stderr, "Monad image-editor inactive motion changed pan\n");
    return false;
  }
  float image_zoom = 1.0f;
  ok = obs_monad_live_editor_image_zoom_level(1.0f, 2.0f, &image_zoom);
  if (!ok || fabsf(image_zoom - 2.0f) > 0.0001f) {
    fprintf(stderr, "Monad image-editor zoom multiplication failed\n");
    return false;
  }
  ok = obs_monad_live_editor_image_zoom_level(1.0f, 0.001f, &image_zoom);
  if (!ok || fabsf(image_zoom - 0.05f) > 0.0001f) {
    fprintf(stderr, "Monad image-editor lower zoom clamp failed\n");
    return false;
  }
  ok = obs_monad_live_editor_image_zoom_level(16.0f, 4.0f, &image_zoom);
  if (!ok || fabsf(image_zoom - 32.0f) > 0.0001f) {
    fprintf(stderr, "Monad image-editor upper zoom clamp failed\n");
    return false;
  }
  float image_pan_x = -1.0f;
  float image_pan_y = -1.0f;
  ok = obs_monad_live_editor_image_zoom_pan(10.0f, 20.0f, 2.0f, 4.0f, 50.0f,
                                            50.0f, 70.0f, 60.0f, &image_pan_x,
                                            &image_pan_y);
  if (!ok || fabsf(image_pan_x) > 0.0001f ||
      fabsf(image_pan_y - 30.0f) > 0.0001f) {
    fprintf(stderr, "Monad pointer-anchored image zoom pan failed\n");
    return false;
  }
  int color_picker_action = -1;
  ok = obs_monad_live_editor_color_picker_pointer_action(true, true,
                                                         &color_picker_action);
  if (!ok || color_picker_action != 1) {
    fprintf(stderr, "Monad color-picker hue-ring precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_color_picker_pointer_action(false, true,
                                                         &color_picker_action);
  if (!ok || color_picker_action != 2) {
    fprintf(stderr, "Monad color-picker triangle hit routing failed\n");
    return false;
  }
  ok = obs_monad_live_editor_color_picker_pointer_action(false, false,
                                                         &color_picker_action);
  if (!ok || color_picker_action != 0) {
    fprintf(stderr, "Monad color-picker miss policy failed\n");
    return false;
  }
  int color_picker_drag = -1;
  ok = obs_monad_live_editor_color_picker_drag_transition(0, 1, false,
                                                          &color_picker_drag);
  if (!ok || color_picker_drag != 1) {
    fprintf(stderr, "Monad color-picker hue drag activation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_color_picker_drag_transition(
      color_picker_drag, 2, false, &color_picker_drag);
  if (!ok || color_picker_drag != 2) {
    fprintf(stderr, "Monad color-picker SV drag transition failed\n");
    return false;
  }
  ok = obs_monad_live_editor_color_picker_drag_transition(
      color_picker_drag, 0, true, &color_picker_drag);
  if (!ok || color_picker_drag != 0) {
    fprintf(stderr, "Monad color-picker drag release failed\n");
    return false;
  }
  bool text_selecting = false;
  int text_pointer_action = -1;
  ok = obs_monad_live_editor_text_pointer_transition(
      false, true, false, true, &text_pointer_action, &text_selecting);
  if (!ok || text_pointer_action != 1 || !text_selecting) {
    fprintf(stderr,
            "Monad text pointer placement/selection activation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_text_pointer_transition(
      text_selecting, true, false, false, &text_pointer_action,
      &text_selecting);
  if (!ok || text_pointer_action != 0 || !text_selecting) {
    fprintf(stderr, "Monad invalid text hit changed selection state\n");
    return false;
  }
  ok = obs_monad_live_editor_text_pointer_transition(
      text_selecting, false, true, false, &text_pointer_action,
      &text_selecting);
  if (!ok || text_pointer_action != 2 || text_selecting) {
    fprintf(stderr, "Monad text selection release failed\n");
    return false;
  }

  int panel_title_action = -1;
  ok = obs_monad_live_editor_bottom_panel_title_action(
      true, true, true, false, false, &panel_title_action);
  if (!ok || panel_title_action != 1) {
    fprintf(stderr, "Monad bottom-panel close precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_bottom_panel_title_action(
      false, true, true, false, false, &panel_title_action);
  if (!ok || panel_title_action != 5) {
    fprintf(stderr, "Monad bottom-panel transport routing failed\n");
    return false;
  }
  ok = obs_monad_live_editor_bottom_panel_title_action(
      false, true, false, true, false, &panel_title_action);
  if (!ok || panel_title_action != 2) {
    fprintf(stderr, "Monad bottom-panel expand action failed\n");
    return false;
  }
  ok = obs_monad_live_editor_bottom_panel_title_action(
      false, true, false, false, true, &panel_title_action);
  if (!ok || panel_title_action != 3) {
    fprintf(stderr, "Monad bottom-panel double-click collapse failed\n");
    return false;
  }
  ok = obs_monad_live_editor_bottom_panel_title_action(
      false, true, false, false, false, &panel_title_action);
  if (!ok || panel_title_action != 4) {
    fprintf(stderr, "Monad bottom-panel resize action failed\n");
    return false;
  }
  ok = obs_monad_live_editor_bottom_panel_title_action(
      false, false, false, false, true, &panel_title_action);
  if (!ok || panel_title_action != 0) {
    fprintf(stderr, "Monad bottom-panel non-title click routing failed\n");
    return false;
  }
  int animation_title_action = -1;
  ok = obs_monad_live_editor_animation_title_action(true, true, true,
                                                    &animation_title_action);
  if (!ok || animation_title_action != 1) {
    fprintf(stderr, "Monad animation stop-button precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_title_action(false, true, true,
                                                    &animation_title_action);
  if (!ok || animation_title_action != 2) {
    fprintf(stderr, "Monad animation play-button precedence failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_title_action(false, false, true,
                                                    &animation_title_action);
  if (!ok || animation_title_action != 3) {
    fprintf(stderr, "Monad animation close-button routing failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_title_action(false, false, false,
                                                    &animation_title_action);
  if (!ok || animation_title_action != 0) {
    fprintf(stderr, "Monad animation titlebar miss routing failed\n");
    return false;
  }
  int animation_pointer_action = -1;
  ok = obs_monad_live_editor_animation_pointer_action(
      2, true, true, true, &animation_pointer_action);
  if (!ok || animation_pointer_action != 2) {
    fprintf(stderr, "Monad animation title control priority failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_action(
      0, true, true, true, &animation_pointer_action);
  if (!ok || animation_pointer_action != 4) {
    fprintf(stderr, "Monad animation ruler priority failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_action(
      0, false, true, true, &animation_pointer_action);
  if (!ok || animation_pointer_action != 5) {
    fprintf(stderr, "Monad animation keyframe priority failed\n");
    return false;
  }
  ok = obs_monad_live_editor_animation_pointer_action(
      0, false, false, true, &animation_pointer_action);
  if (!ok || animation_pointer_action != 6) {
    fprintf(stderr, "Monad animation track-row routing failed\n");
    return false;
  }
  bool message_dragging = false;
  ok = obs_monad_live_editor_message_drag_transition(false, true, false, true,
                                                     &message_dragging);
  if (!ok || !message_dragging) {
    fprintf(stderr, "Monad message drag activation failed\n");
    return false;
  }
  ok = obs_monad_live_editor_message_drag_transition(
      message_dragging, false, false, false, &message_dragging);
  if (!ok || !message_dragging) {
    fprintf(stderr, "Monad message drag movement state was not preserved\n");
    return false;
  }
  ok = obs_monad_live_editor_message_drag_transition(
      message_dragging, false, true, false, &message_dragging);
  if (!ok || message_dragging) {
    fprintf(stderr, "Monad message drag release failed\n");
    return false;
  }
  ObsMonadTreeRowAction row_action;
  ok = obs_monad_live_editor_tree_row_action(true, true, true, 18.0, 0.0, 0.0,
                                             20.0, true, &row_action);
  if (!ok || row_action != OBS_MONAD_TREE_ROW_TOGGLE_VISIBILITY) {
    fprintf(stderr,
            "Monad tree visibility-click precedence failed (ok=%d action=%d)\n",
            ok, (int)row_action);
    return false;
  }
  ok = obs_monad_live_editor_tree_row_action(true, true, true, 17.0, 0.0, 40.0,
                                             20.0, true, &row_action);
  if (!ok || row_action != OBS_MONAD_TREE_ROW_EXPAND) {
    fprintf(stderr,
            "Monad tree expansion-click policy failed (ok=%d action=%d)\n", ok,
            (int)row_action);
    return false;
  }
  ok = obs_monad_live_editor_tree_row_action(true, true, false, 18.0, 0.0, 40.0,
                                             20.0, true, &row_action);
  if (!ok || row_action != OBS_MONAD_TREE_ROW_SELECT) {
    fprintf(stderr, "Monad tree row selection policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_tree_row_action(true, false, true, 18.0, 0.0, 0.0,
                                             20.0, true, &row_action);
  if (!ok || row_action != OBS_MONAD_TREE_ROW_DRAG) {
    fprintf(stderr, "Monad tree drag precedence failed (ok=%d action=%d)\n", ok,
            (int)row_action);
    return false;
  }
  ok = obs_monad_live_editor_tree_row_action(false, false, true, 18.0, 0.0, 0.0,
                                             20.0, true, &row_action);
  if (!ok || row_action != OBS_MONAD_TREE_ROW_NONE) {
    fprintf(stderr, "Monad idle tree row produced an event\n");
    return false;
  }

  ok = obs_monad_live_editor_hierarchy_selection(11, true, false, -1, &action);
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false)) {
    fprintf(stderr, "Monad hierarchy toggle anchor query failed\n");
    return false;
  }
  if (!ok || action != OBS_MONAD_HIERARCHY_SELECT_TOGGLE || anchor != 11) {
    fprintf(stderr, "Monad hierarchy Ctrl toggle transition failed\n");
    return false;
  }

  ok = obs_monad_live_editor_hierarchy_selection(12, false, false, -1, &action);
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false)) {
    fprintf(stderr, "Monad hierarchy single-selection anchor query failed\n");
    return false;
  }
  if (!ok || action != OBS_MONAD_HIERARCHY_SELECT_SINGLE || anchor != 12) {
    fprintf(stderr, "Monad hierarchy single-selection transition failed\n");
    return false;
  }

  ok = obs_monad_live_editor_hierarchy_selection(13, false, true, -1, &action);
  if (!obs_monad_live_editor_hierarchy_anchor(&anchor, false)) {
    fprintf(stderr, "Monad hierarchy append anchor query failed\n");
    return false;
  }
  if (!ok || action != OBS_MONAD_HIERARCHY_SELECT_APPEND || anchor != 12) {
    fprintf(stderr, "Monad hierarchy unanchored Shift policy failed\n");
    return false;
  }

  bool target_visible = false;
  ok = obs_monad_live_editor_hierarchy_visibility(false, &target_visible);
  if (!ok || !target_visible) {
    fprintf(stderr, "Monad hierarchy show-all visibility policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_visibility(true, &target_visible);
  if (!ok || target_visible) {
    fprintf(stderr, "Monad hierarchy hide-all visibility policy failed\n");
    return false;
  }
  bool target_expanded = false;
  ok = obs_monad_live_editor_hierarchy_expansion(true, &target_expanded);
  if (!ok || target_expanded) {
    fprintf(stderr, "Monad hierarchy collapse policy failed\n");
    return false;
  }
  ok = obs_monad_live_editor_hierarchy_expansion(false, &target_expanded);
  if (!ok || !target_expanded) {
    fprintf(stderr, "Monad hierarchy expand policy failed\n");
    return false;
  }
  static const struct {
    const char *subject;
    ObsMonadInspectorMode expected;
  } inspector_modes[] = {
      {"skeleton", OBS_MONAD_INSPECTOR_MODE_SKELETON},
      {"collider", OBS_MONAD_INSPECTOR_MODE_COLLIDER},
      {"material", OBS_MONAD_INSPECTOR_MODE_MATERIAL},
      {"mesh", OBS_MONAD_INSPECTOR_MODE_DEFAULT},
      {"sdf", OBS_MONAD_INSPECTOR_MODE_DEFAULT},
      {"group", OBS_MONAD_INSPECTOR_MODE_DEFAULT},
  };
  for (size_t i = 0; i < sizeof(inspector_modes) / sizeof(inspector_modes[0]);
       ++i) {
    ObsMonadInspectorMode mode;
    if (!obs_monad_live_editor_inspector_mode(inspector_modes[i].subject,
                                              &mode) ||
        mode != inspector_modes[i].expected) {
      fprintf(stderr, "Monad hierarchy inspector mode policy failed\n");
      return false;
    }
    if (!obs_monad_live_editor_inspector_current_mode(&mode) ||
        mode != inspector_modes[i].expected) {
      fprintf(stderr, "Monad inspector mode state transition failed\n");
      return false;
    }
  }
  static const struct {
    const char *subject;
    ObsMonadInspectorMode mode;
    ObsMonadInspectorDeleteAction expected;
  } inspector_delete_actions[] = {
      {"default", OBS_MONAD_INSPECTOR_MODE_DEFAULT,
       OBS_MONAD_INSPECTOR_DELETE_WHOLE_OBJECT},
      {"skeleton", OBS_MONAD_INSPECTOR_MODE_SKELETON,
       OBS_MONAD_INSPECTOR_DELETE_SKELETON},
      {"collider", OBS_MONAD_INSPECTOR_MODE_COLLIDER,
       OBS_MONAD_INSPECTOR_DELETE_COLLIDER},
      {"material", OBS_MONAD_INSPECTOR_MODE_MATERIAL,
       OBS_MONAD_INSPECTOR_DELETE_MATERIAL},
  };
  for (size_t i = 0; i < sizeof(inspector_delete_actions) /
                             sizeof(inspector_delete_actions[0]);
       ++i) {
    ObsMonadInspectorMode mode;
    ObsMonadInspectorDeleteAction action;
    if (!obs_monad_live_editor_inspector_mode(
            inspector_delete_actions[i].subject, &mode) ||
        mode != inspector_delete_actions[i].mode)
      return false;
    if (!obs_monad_live_editor_inspector_delete_action(true, &action) ||
        action != inspector_delete_actions[i].expected) {
      fprintf(stderr, "Monad inspector delete-action policy failed\n");
      return false;
    }
    if (!obs_monad_live_editor_inspector_delete_action(false, &action) ||
        action != OBS_MONAD_INSPECTOR_DELETE_WHOLE_OBJECT) {
      fprintf(stderr, "Monad invalid-mesh delete fallback policy failed\n");
      return false;
    }
  }
  return true;
}

static bool headless_editor_marginalia_contract(void) {
  char annotation[256];
  if (!obs_monad_live_editor_marginalia_scene("cached", "3 meshes · scene.glb",
                                              annotation, sizeof(annotation)) ||
      strcmp(annotation, "cached · 3 meshes · scene.glb") != 0) {
    fprintf(stderr, "Monad Marginalia scene annotation formatting failed\n");
    return false;
  }
  if (!obs_monad_live_editor_marginalia_material(
          "mesh", "R 0.50 · M 0.25 · #804020", annotation,
          sizeof(annotation)) ||
      strcmp(annotation, "mesh · R 0.50 · M 0.25 · #804020") != 0) {
    fprintf(stderr, "Monad Marginalia material annotation formatting failed\n");
    return false;
  }

  KeyChordBinding *help = keychord_find_binding(&keymap, "C-h v");
  if (!help || help->action_type != ACTION_MONAD_PROC || !help->managed_monad) {
    fprintf(stderr, "Monad C-h v variable help binding is missing\n");
    return false;
  }
  keychord_invoke_binding(help);
  bool found_width = false;
  for (size_t i = 0; vertico.is_active && i < vertico.candidate_count; ++i) {
    const VerticoCandidate *candidate = &vertico.candidates[i];
    if (strcmp(candidate->text, "vertico-width") == 0 &&
        strstr(candidate->annotation, "640") != NULL &&
        strstr(candidate->annotation, "Int") != NULL) {
      found_width = true;
      break;
    }
  }
  if (vertico.is_active)
    vertico_quit();
  if (!found_width) {
    fprintf(stderr,
            "Monad C-h v did not show the typed vertico-width variable\n");
    return false;
  }
  return true;
}

static bool headless_vertico_selection_visible(size_t selected_index) {
  const int32_t offset = vertico_scroll_offset();
  const int32_t visible_count = vertico_visible_count();
  return offset >= 0 && visible_count > 0 && (size_t)offset <= selected_index &&
         selected_index - (size_t)offset < (size_t)visible_count;
}

static int run_headless_raw(const char *path, const char *eval_source,
                            const char *eval_file, const char *receipt_path,
                            uint32_t width, uint32_t height, uint32_t frames,
                            bool middle_orbit_test, bool editor_panels_test,
                            bool antialiasing_transition_test, bool test_matcap,
                            bool test_no_sun, const char *test_obj_async_path) {
  /* A pre-existing success receipt must never survive an unsuccessful
   * new run and be mistaken for this invocation's result. */
  if (receipt_path && unlink(receipt_path) != 0 && errno != ENOENT) {
    fprintf(stderr, "Cannot clear stale agent receipt: %s\n", receipt_path);
    return EXIT_FAILURE;
  }
  if (!obs_headless_init(width, height, true)) {
    fprintf(stderr, "Failed to initialize headless Vulkan renderer\n");
    return EXIT_FAILURE;
  }
  if (test_no_sun)
    CTX_LIGHTING(&context)->sun.color[3] = 0.0f;
  s_headless_test_matcap = test_matcap;
  s_headless_test_obj_async = test_obj_async_path != NULL;
  s_headless_obj_passed = false;
  bool test_keymap_initialized = false;
  if (editor_panels_test) {
    init_free_type();
    initThemes();
    ease_init();
    keymap_init(&keymap);
    test_keymap_initialized = true;
    editor_init();
    if (!editor.initialized) {
      fprintf(stderr, "Headless editor initialization failed\n");
      keymap_free(&keymap);
      cleanup(&context);
      return EXIT_FAILURE;
    }
    gizmo_init();
    vertico_init();
    add_menu_init();
    if (!obs_register_editor_commands()) {
      fprintf(stderr, "Could not register headless live editor commands\n");
      add_menu_cleanup();
      vertico_cleanup();
      editor_cleanup();
      keymap_free(&keymap);
      cleanup(&context);
      return EXIT_FAILURE;
    }
  }
  bool monad_started = false;
  bool ready = true;
  if (eval_source || eval_file || middle_orbit_test || editor_panels_test ||
      antialiasing_transition_test) {
    monad_started = obs_monad_engine_start();
    if (!monad_started) {
      ready = false;
    } else if (eval_source || eval_file) {
      monad_error_t error = {0};
      monad_source_result_t *result = NULL;
      ready = eval_file ? obs_monad_load_file(obs_monad_engine(), eval_file,
                                              &result, &error)
                        : obs_monad_load_source(obs_monad_engine(),
                                                "obsidian://headless-eval",
                                                eval_source, &result, &error);
      if (!ready) {
        const size_t count =
            result ? monad_source_result_diagnostic_count(result) : 0;
        for (size_t i = 0; i < count; ++i) {
          const char *code = monad_source_result_diagnostic_code(result, i);
          const char *message =
              monad_source_result_diagnostic_message(result, i);
          fprintf(stderr, "Obsidian Monad --eval [%s]: %s\n",
                  code ? code : "diagnostic",
                  message ? message : "no diagnostic message");
        }
        if (!count)
          fprintf(stderr, "Obsidian Monad --eval: %s\n",
                  error.message ? error.message : "source was not published");
        if (result)
          monad_source_result_destroy(result);
      } else if (result) {
        monad_source_result_destroy(result);
      }
    } else if (middle_orbit_test || editor_panels_test ||
               antialiasing_transition_test) {
      obs_monad_engine_boot_from_environment();
      ObsMonadReloadReport report = {0};
      obs_monad_engine_reload_report(&report);
      if (report.state != OBS_MONAD_RELOAD_SUCCEEDED) {
        fprintf(stderr, "Headless Monad editor module did not publish: %s\n",
                report.message);
        ready = false;
      }
    }
    if (ready && (eval_source || eval_file)) {
      monad_i64_function_t camera_step = NULL;
      monad_error_t error = {0};
      if (monad_find_i64_function(obs_monad_engine()->runtime,
                                  "obsidian_camera_step_mm", &camera_step,
                                  &error) != MONAD_OK ||
          !camera_step) {
        fprintf(stderr, "Obsidian Monad --eval must define "
                        "obsidian_camera_step_mm : Int -> Int\n");
        ready = false;
      }
    }
  }
  if (ready && middle_orbit_test)
    ready = headless_middle_orbit_fixture();
  if (ready && editor_panels_test) {
    static const char *const keys[] = {"C-a", "C-e", "C-c r"};
    static const char *const labels[] = {"hierarchy", "inspector", "REPL"};
    for (size_t i = 0; i < 3; ++i) {
      KeyChordBinding *binding = keychord_find_binding(&keymap, keys[i]);
      if (!binding || binding->action_type != ACTION_MONAD_PROC ||
          !binding->managed_monad) {
        fprintf(
            stderr,
            "Headless live editor contract: missing Monad %s binding (%s)\n",
            labels[i], keys[i]);
        ready = false;
        break;
      }
    }
    if (ready) {
      if (editor_panel_is_open(PANEL_LEFT) ||
          editor_panel_is_open(PANEL_RIGHT) || obs_repl_panel_active()) {
        fprintf(stderr,
                "Headless editor contract: panels not initially closed\n");
        ready = false;
      } else {
        KeyChordBinding *binding = keychord_find_binding(&keymap, keys[0]);
        keychord_invoke_binding(binding);
        if (!editor_panel_is_open(PANEL_LEFT) ||
            editor.panels[PANEL_LEFT].target_t != 1.0f) {
          fprintf(stderr, "Monad hierarchy command failed to open panel\n");
          ready = false;
        }
      }
    }
    if (ready) {
      KeyChordBinding *binding = keychord_find_binding(&keymap, keys[0]);
      keychord_invoke_binding(binding);
      if (editor_panel_is_open(PANEL_LEFT) ||
          editor.panels[PANEL_LEFT].target_t != 0.0f) {
        fprintf(stderr,
                "Monad hierarchy command failed to close panel "
                "(open=%d target=%.3f)\n",
                editor_panel_is_open(PANEL_LEFT),
                editor.panels[PANEL_LEFT].target_t);
        ready = false;
      }
    }
    if (ready) {
      KeyChordBinding *binding = keychord_find_binding(&keymap, keys[1]);
      keychord_invoke_binding(binding);
      if (!editor_panel_is_open(PANEL_RIGHT) ||
          editor.panels[PANEL_RIGHT].target_t != 1.0f) {
        fprintf(stderr, "Monad inspector command failed to open panel\n");
        ready = false;
      } else {
        keychord_invoke_binding(binding);
        if (editor_panel_is_open(PANEL_RIGHT) ||
            editor.panels[PANEL_RIGHT].target_t != 0.0f) {
          fprintf(stderr, "Monad inspector command failed to close panel\n");
          ready = false;
        }
      }
    }
    if (ready) {
      KeyChordBinding *binding = keychord_find_binding(&keymap, keys[2]);
      keychord_invoke_binding(binding);
      if (!obs_repl_panel_active()) {
        fprintf(stderr, "Monad REPL command failed to open panel\n");
        ready = false;
      } else {
        keychord_invoke_binding(binding);
        if (obs_repl_panel_active()) {
          fprintf(stderr, "Monad REPL command failed to close panel\n");
          ready = false;
        }
      }
    }
    if (ready)
      fprintf(stdout, "Live Monad keymap: C-a hierarchy, C-e inspector, "
                      "C-c r REPL open/close PASS\n");
    if (ready) {
      struct {
        const char *map;
        const char *notation;
        const char *command;
        const char *label;
      } const expected[] = {
          {"global", "g", "editor-start-grab-command", "global translate"},
          {"global", "r", "editor-start-rotate-command", "global rotate"},
          {"global", "q", "gizmo-mode-rotate-command", "gizmo rotate"},
          {"global", "w", "gizmo-mode-translate-command", "gizmo translate"},
          {"global", "e", "gizmo-mode-scale-command", "gizmo scale"},
          {"global", "TAB", "editor-toggle-skybox-command", "skybox toggle"},
          {"global", "t", "editor-toggle-ibl-command", "IBL toggle"},
          {"global", "l", "editor-toggle-shadows-command", "shadow toggle"},
          {"global", "C-t", "editor-toggle-culling-freeze-command",
           "culling toggle"},
          {"global", "f", "editor-frame-selected-command", "frame selection"},
          {"global", "C-h v", "editor-show-variables-command", "variable help"},
          {"global", "C-h C-f", "editor-show-materials-command",
           "material browser"},
          {"global", "C-x b", "editor-show-scene-buffers-command",
           "scene buffer chooser"},
          {"global", "C-x 5 2", "editor-frame-create-command",
           "new editor frame"},
          {"global", "C-x 5 0", "editor-frame-close-command",
           "close editor frame"},
          {"global", "C-x 5 b", "editor-open-frame-scene-buffers-command",
           "scene buffer in new frame"},
          {"transform", "r", "editor-grab-rotate-command", "transform rotate"},
          {"transform", "x", "editor-grab-lock-x-command",
           "transform X constraint"},
          {"transform", "0", "editor-grab-origin-command",
           "transform world origin"},
          {"transform", "1", "editor-grab-snap-1-command",
           "transform one-meter snap"},
          {"transform", "2", "editor-grab-snap-2-command",
           "transform two-meter snap"},
          {"search", "C-g", "editor-search-cancel-command", "search cancel"},
          {"add-menu", "C-n", "add-menu-next-command", "Add Menu next"},
          {"add-menu", "C-p", "add-menu-previous-command", "Add Menu previous"},
          {"add-menu", "C-c n", "add-menu-last-command", "Add Menu last"},
          {"add-menu", "C-c p", "add-menu-first-command", "Add Menu first"},
          {"animation", "C-g", "animation-close-command", "animation close"},
          {"repl", "RET", "repl-submit-command", "REPL submit"},
          {"text", "C-n", "text-next-line-command", "text next-line"},
          {"vertico", "C-n", "vertico-next-command", "Vertico next"},
      };
      for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        KeyChordBinding *binding = obs_monad_live_editor_find_binding(
            expected[i].map, expected[i].notation);
        if (!binding || binding->action_type != ACTION_MONAD_PROC ||
            !binding->managed_monad || !binding->command_name ||
            strcmp(binding->command_name, expected[i].command) != 0) {
          fprintf(stderr,
                  "Live Monad editor contract: missing %s binding (%s %s)\n",
                  expected[i].label, expected[i].map, expected[i].notation);
          ready = false;
          break;
        }
      }
    }
    if (ready) {
      ready = headless_editor_frame_selection_contract();
      if (ready)
        fprintf(stdout,
                "Monad f camera framing: first press preserves view direction "
                "PASS\n");
    }
    if (ready) {
      ready = headless_editor_sdf_selection_contract();
      if (ready)
        fprintf(stdout,
                "F frames selected SDFs; background marquee selects multiple "
                "SDFs PASS\n");
    }
    if (ready) {
      ready = headless_editor_monad_hierarchy_selection_contract();
      if (ready)
        fprintf(
            stdout,
            "Monad hierarchy policy: selection actions/effects/ranges/"
            "auto-scroll/search-centering/beam/navigation/"
            "scroll-animation/animation-transport, "
            "visibility, expansion, text-wheel-target/gizmo-hover/"
            "popup-window-input/animation/drag-bounds/"
            "bottom-panel-title; "
            "marquee bounds containment/"
            "intersection/3px-drag edges, filesystem wheel and animation "
            "track-scroll/view-pan/keyframe-toggle policies PASS; "
            "glTF group promotion and inspector mode/Delete routing PASS\n");
    }
    if (ready) {
      ready = headless_editor_marginalia_contract();
      if (ready)
        fprintf(stdout,
                "Monad Marginalia and C-h v: scene annotations and typed "
                "variable values PASS\n");
    }
    if (ready) {
      ready = headless_editor_transform_contract();
      if (ready)
        fprintf(stdout, "Monad editor capability and transforms: selection, "
                        "g x 2 snap, g 0 origin, C-g rollback PASS\n");
    }
    if (ready) {
      KeyChordBinding *open_repl = keychord_find_binding(&keymap, "C-c r");
      KeyChordBinding *close_repl =
          obs_monad_live_editor_find_binding("repl", "C-g");
      if (!open_repl || !close_repl) {
        fprintf(stderr,
                "Live Monad editor contract: missing modal REPL bindings\n");
        ready = false;
      } else {
        keychord_invoke_binding(open_repl);
        keychord_invoke_binding(close_repl);
        if (obs_repl_panel_active()) {
          fprintf(stderr,
                  "Monad REPL modal keymap failed to close its owner\n");
          ready = false;
        }
      }
    }
    if (ready) {
      KeyChordBinding *rotate = keychord_find_binding(&keymap, "r");
      KeyChordBinding *gizmo_rotate = keychord_find_binding(&keymap, "q");
      if (!rotate || rotate->action_type != ACTION_MONAD_PROC ||
          !gizmo_rotate || gizmo_rotate->action_type != ACTION_MONAD_PROC) {
        fprintf(stderr,
                "Live Monad editor contract: missing rotate/gizmo key\n");
        ready = false;
      } else {
        keychord_invoke_binding(gizmo_rotate);
        if (gizmo.mode != GIZMO_MODE_ROTATE) {
          fprintf(stderr, "Monad q command did not select rotate gizmo mode\n");
          ready = false;
        } else {
          KeyChordBinding *translate = keychord_find_binding(&keymap, "w");
          KeyChordBinding *scale = keychord_find_binding(&keymap, "e");
          if (!translate || !scale) {
            fprintf(stderr, "Monad gizmo mode commands are incomplete\n");
            ready = false;
          } else {
            keychord_invoke_binding(translate);
            if (gizmo.mode != GIZMO_MODE_TRANSLATE) {
              fprintf(stderr,
                      "Monad w command did not select translate gizmo mode\n");
              ready = false;
            }
            if (ready) {
              keychord_invoke_binding(scale);
              if (gizmo.mode != GIZMO_MODE_SCALE) {
                fprintf(stderr,
                        "Monad e command did not select scale gizmo mode\n");
                ready = false;
              }
            }
            if (ready)
              keychord_invoke_binding(gizmo_rotate);
          }
        }
      }
    }
    if (ready) {
      struct {
        const char *key;
        bool *setting;
        const char *label;
      } const render_toggles[] = {
          {"TAB", &skyboxEnabled, "skybox"},
          {"t", &iblLightingEnabled, "IBL"},
          {"l", &shadowsEnabled, "shadows"},
          {"C-t", &cullingFrozen, "culling freeze"},
      };
      for (size_t i = 0; i < sizeof(render_toggles) / sizeof(render_toggles[0]);
           ++i) {
        KeyChordBinding *binding =
            keychord_find_binding(&keymap, render_toggles[i].key);
        const bool initial = *render_toggles[i].setting;
        if (!binding || binding->action_type != ACTION_MONAD_PROC) {
          fprintf(stderr, "Monad render toggle missing: %s (%s)\n",
                  render_toggles[i].label, render_toggles[i].key);
          ready = false;
          break;
        }
        keychord_invoke_binding(binding);
        if (*render_toggles[i].setting == initial) {
          fprintf(stderr,
                  "Monad %s command did not change its render setting\n",
                  render_toggles[i].label);
          ready = false;
          break;
        }
        keychord_invoke_binding(binding);
        if (*render_toggles[i].setting != initial) {
          fprintf(stderr,
                  "Monad %s command did not restore its render setting\n",
                  render_toggles[i].label);
          ready = false;
          break;
        }
      }
    }
    if (ready) {
      add_menu_open(32.0, 32.0);
      const int32_t count = add_menu_visible_count();
      KeyChordBinding *next = keychord_find_binding(&keymap, "C-n");
      KeyChordBinding *previous = keychord_find_binding(&keymap, "C-p");
      KeyChordBinding *last = keychord_find_binding(&keymap, "C-c n");
      KeyChordBinding *first = keychord_find_binding(&keymap, "C-c p");
      if (!add_menu_is_open() || count < 2 || add_menu_selection_index() != 0 ||
          !next || !previous || !last || !first) {
        fprintf(
            stderr,
            "Headless Monad Add Menu did not open with navigable candidates\n");
        ready = false;
      } else {
        keychord_invoke_binding(next);
        if (add_menu_selection_index() != 1) {
          fprintf(stderr, "Monad Add Menu next did not advance selection\n");
          ready = false;
        }
        if (ready) {
          keychord_invoke_binding(next);
          if (add_menu_selection_index() != 1) {
            fprintf(stderr,
                    "Monad Add Menu next exceeded the last candidate\n");
            ready = false;
          }
        }
        if (ready) {
          keychord_invoke_binding(previous);
          if (add_menu_selection_index() != 0) {
            fprintf(stderr, "Monad Add Menu previous did not return to the "
                            "first candidate\n");
            ready = false;
          }
        }
        if (ready) {
          keychord_invoke_binding(last);
          if (add_menu_selection_index() != count - 1) {
            fprintf(stderr,
                    "Monad Add Menu last did not select the final candidate\n");
            ready = false;
          }
        }
        if (ready) {
          keychord_invoke_binding(first);
          if (add_menu_selection_index() != 0) {
            fprintf(
                stderr,
                "Monad Add Menu first did not select the first candidate\n");
            ready = false;
          }
        }
      }
      add_menu_close();
      if (add_menu_is_open()) {
        fprintf(stderr, "Headless Monad Add Menu failed to close\n");
        ready = false;
      }
      if (ready)
        fprintf(stdout,
                "Live Monad Add Menu: bounded next/previous/first/last PASS\n");
    }
    if (ready) {
      static const char *const candidates[] = {
          "candidate-00", "candidate-01", "candidate-02", "candidate-03",
          "candidate-04", "candidate-05", "candidate-06", "candidate-07",
          "candidate-08", "candidate-09", "candidate-10", "candidate-11",
      };
      vertico_clear_candidates();
      for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        vertico_add_candidate(candidates[i], NULL, NULL);
      vertico_activate("headless", NULL);
      KeyChordMap *active_map = keymap_stack_get_local();
      KeyChordBinding *next = keychord_find_binding(active_map, "C-n");
      KeyChordBinding *previous = keychord_find_binding(active_map, "C-p");
      KeyChordBinding *last = keychord_find_binding(active_map, "C-c n");
      KeyChordBinding *first = keychord_find_binding(active_map, "C-c p");
      if (!vertico.is_active || !next || !previous || !last || !first ||
          vertico.width != 640.0f || vertico.top || vertico.count != 10 ||
          vertico_filtered_count() !=
              sizeof(candidates) / sizeof(candidates[0])) {
        fprintf(stderr, "Headless Monad Vertico did not apply its default "
                        "layout or bindings\n");
        ready = false;
      } else {
        for (int i = 0; i < 10 && ready; ++i) {
          keychord_invoke_binding(next);
          if (vertico_selection_index() != (size_t)(i + 1)) {
            fprintf(stderr, "Monad Vertico next failed at candidate %d\n",
                    i + 1);
            ready = false;
          }
        }
        if (ready &&
            (vertico_selection_index() != 10 || vertico_scroll_offset() <= 0 ||
             !headless_vertico_selection_visible(10))) {
          fprintf(stderr,
                  "Monad Vertico next failed to scroll the visible window\n");
          ready = false;
        }
        if (ready) {
          keychord_invoke_binding(last);
          if (vertico_selection_index() != 11 ||
              !headless_vertico_selection_visible(11)) {
            fprintf(
                stderr,
                "Monad Vertico last failed to select the final candidate\n");
            ready = false;
          }
        }
        if (ready) {
          keychord_invoke_binding(previous);
          keychord_invoke_binding(first);
          if (vertico_selection_index() != 0 || vertico_scroll_offset() != 0) {
            fprintf(stderr, "Monad Vertico previous/first failed to restore "
                            "the first candidate\n");
            ready = false;
          }
        }
      }
      vertico_quit();
      vertico_clear_candidates();
      if (ready)
        fprintf(stdout, "Live Monad Vertico: default layout, bounded "
                        "navigation and scrolling PASS\n");
    }
    if (ready) {
      const char *changed_error = NULL;
      if (obs_monad_live_editor_reload_file(obs_monad_engine(),
                                            "monad/obsidian/editor-orbit.mon",
                                            &changed_error)) {
        fprintf(stderr,
                "Monad editor accepted an unverified replacement image\n");
        ready = false;
      } else {
        KeyChordBinding *preserved_q = keychord_find_binding(&keymap, "q");
        KeyChordBinding *preserved_modal =
            obs_monad_live_editor_find_binding("repl", "C-g");
        if (!changed_error || !preserved_q || !preserved_modal) {
          fprintf(stderr,
                  "Rejected editor replacement changed the active keymaps\n");
          ready = false;
        } else {
          keychord_invoke_binding(preserved_q);
          if (gizmo.mode != GIZMO_MODE_ROTATE) {
            fprintf(stderr,
                    "Rejected editor replacement broke gizmo dispatch\n");
            ready = false;
          }
        }
      }
    }
    if (ready) {
      const char *reload_error = NULL;
      if (!obs_monad_live_editor_reload_file(
              obs_monad_engine(), "monad/obsidian/editor.mon", &reload_error)) {
        fprintf(stderr, "Monad editor keymap reload failed: %s\n",
                reload_error ? reload_error : "unknown error");
        ready = false;
      } else {
        KeyChordBinding *reloaded_q = keychord_find_binding(&keymap, "q");
        KeyChordBinding *reloaded_modal =
            obs_monad_live_editor_find_binding("repl", "C-g");
        if (!reloaded_q || !reloaded_modal || !reloaded_q->managed_monad ||
            !reloaded_modal->managed_monad) {
          fprintf(stderr, "Monad editor reload lost a live keymap binding\n");
          ready = false;
        } else {
          keychord_invoke_binding(reloaded_q);
          if (gizmo.mode != GIZMO_MODE_ROTATE) {
            fprintf(stderr, "Preserved Monad gizmo command did not dispatch\n");
            ready = false;
          } else {
            fprintf(stdout,
                    "Live editor reload guard: identical source retained; "
                    "changed source rejected without losing commands PASS\n");
          }
        }
      }
    }
  }
  /* A single published Monad generation drives the production headless
   * renderer for every step. Source recompilation never occurs per frame. */
  if (antialiasing_transition_test && frames != 4) {
    fprintf(stderr, "--test-antialiasing requires --frames 4\n");
    ready = false;
  }
  ObsRawAgentFrame job = {.path = path,
                          .uses_camera_policy = eval_source || eval_file,
                          .antialiasing_transition_test =
                              antialiasing_transition_test};
  if (ready && test_obj_async_path) {
    s_headless_obj_async = obs_obj_async_create();
    if (!s_headless_obj_async ||
        !obs_obj_async_request(s_headless_obj_async, scene, test_obj_async_path,
                               (vec4){1.0f, 1.0f, 1.0f, 1.0f})) {
      fprintf(stderr, "Could not start headless asynchronous OBJ fixture\n");
      ready = false;
    }
  }
  if (ready && antialiasing_transition_test) {
    monad_error_t error = {0};
    if (monad_dynamic_call_named(obs_monad_engine()->space, "render", 0, NULL,
                                 &job.render_policy, &error) != MONAD_OK ||
        !job.render_policy) {
      fprintf(stderr, "Could not acquire render policy for AA smoke: %s\n",
              error.message ? error.message : "no diagnostic");
      ready = false;
    }
  }
  uint32_t completed = 0;
  bool image_rendered =
      ready &&
      obs_headless_execute_frames(frames, render_agent_frame, &job, &completed);
  bool rendered = image_rendered;
  if (ready && test_obj_async_path && !s_headless_obj_passed) {
    fprintf(stderr, "Headless asynchronous OBJ acceptance failed\n");
    rendered = false;
  }
  if (ready && !rendered)
    fprintf(stderr, "Headless frame %u/%u failed\n", completed + 1, frames);
  if (rendered && receipt_path) {
    const ObsAgentReceipt receipt = {.image_path = path,
                                     .width = width,
                                     .height = height,
                                     .completed_frames = frames,
                                     .source = eval_file ? OBS_AGENT_SOURCE_FILE
                                               : eval_source
                                                   ? OBS_AGENT_SOURCE_INLINE
                                                   : OBS_AGENT_SOURCE_NONE};
    if (!obs_headless_write_receipt(receipt_path, &receipt)) {
      fprintf(stderr, "Failed to write headless agent receipt: %s\n",
              receipt_path);
      rendered = false;
    }
  }
  if (context.device)
    (void)vkDeviceWaitIdle(context.device);
  obs_obj_async_destroy(s_headless_obj_async);
  s_headless_obj_async = NULL;
  s_headless_test_obj_async = false;
  if (job.render_policy)
    monad_value_release(job.render_policy);
  if (monad_started)
    obs_monad_engine_stop();
  if (editor_panels_test) {
    add_menu_cleanup();
    vertico_cleanup();
  }
  if (editor_panels_test && editor.initialized)
    editor_cleanup();
  if (test_keymap_initialized)
    keymap_free(&keymap);
  cleanup(&context);
  if (!rendered)
    return EXIT_FAILURE;
  fprintf(stdout, "Wrote %ux%u raw RGBA8 image after %u frame(s): %s\n", width,
          height, frames, path);
  return EXIT_SUCCESS;
}

static int run_headless_benchmark(const char *report_path,
                                  const char *scene_path, uint32_t width,
                                  uint32_t height) {
  const char *monad_path = getenv("OBSIDIAN_MONAD_BOOT");
  const bool monad_configured = monad_path && *monad_path;
  const bool prepare_only =
      getenv("OBSIDIAN_BENCHMARK_PREPARE") &&
      strcmp(getenv("OBSIDIAN_BENCHMARK_PREPARE"), "1") == 0;
  const char *timestamp_mode = getenv("OBSIDIAN_BENCH_GPU_TIMESTAMPS");
  const bool capture_gpu_timestamps =
      !timestamp_mode || strcmp(timestamp_mode, "0") != 0;
  if (!report_path || (!scene_path && !monad_configured)) {
    fprintf(stderr, "Invalid headless benchmark configuration\n");
    return EXIT_FAILURE;
  }

  ObsEngineBench bench = {0};
  int result = EXIT_FAILURE;
  const uint64_t process_start_ns = obs_engine_bench_now_ns();
  const uint64_t process_cpu_start_ns = obs_engine_bench_cpu_now_ns();
  bool monad_started = false;
  bool monad_published = false;
  bool renderer_ready = false;
  ObsBenchmarkManifest manifest = {0};
  bool manifest_loaded = false;
  uint32_t measured = 0, warmup = 0;
  const char *selected_scene = scene_path;
  const char *cache_mode =
      getenv("OBSIDIAN_BENCH_SCENE_CACHE") &&
              strcmp(getenv("OBSIDIAN_BENCH_SCENE_CACHE"), "off") == 0
          ? "cold"
          : "warm";
  float benchmark_sun_intensity = -1.0f;
  const bool override_sun_intensity =
      getenv("OBSIDIAN_BENCH_SUN_INTENSITY") != NULL;
  if (override_sun_intensity &&
      !parse_bench_float("OBSIDIAN_BENCH_SUN_INTENSITY",
                         &benchmark_sun_intensity)) {
    fprintf(stderr, "Invalid OBSIDIAN_BENCH_SUN_INTENSITY\n");
    goto cleanup;
  }

  if (monad_configured) {
    if (!obs_monad_engine_start())
      goto cleanup;
    monad_started = true;
    obs_monad_engine_boot_from_environment();
    ObsMonadReloadReport reload = {0};
    obs_monad_engine_reload_report(&reload);
    monad_published = reload.state == OBS_MONAD_RELOAD_SUCCEEDED;
    if (!monad_published) {
      fprintf(stderr, "Headless benchmark Monad module did not publish\n");
      goto cleanup;
    }
    manifest_loaded = obs_benchmark_manifest_get(&manifest);
    if (manifest_loaded) {
      if (scene_path && strcmp(scene_path, manifest.scene) != 0) {
        fprintf(stderr, "CLI scene conflicts with the Monad manifest\n");
        goto cleanup;
      }
      selected_scene = manifest.scene;
      width = manifest.width;
      height = manifest.height;
      measured = manifest.measured_frames;
      warmup = manifest.warmup_frames;
      cache_mode =
          manifest.cache_mode == OBS_BENCHMARK_CACHE_COLD ? "cold" : "warm";
      if (setenv("OBSIDIAN_BENCH_SCENE_CACHE",
                 manifest.cache_mode == OBS_BENCHMARK_CACHE_COLD ? "off" : "on",
                 1) != 0) {
        fprintf(stderr, "Could not apply Monad benchmark cache policy\n");
        goto cleanup;
      }
    }
  }
  if (!selected_scene) {
    fprintf(stderr,
            "Headless Monad module did not define a benchmark manifest\n");
    goto cleanup;
  }
  if (!manifest_loaded && (!parse_bench_count("OBSIDIAN_BENCHMARK_FRAMES", 600,
                                              1, 100000, &measured) ||
                           !parse_bench_count("OBSIDIAN_BENCHMARK_WARMUP", 120,
                                              0, 100000, &warmup) ||
                           warmup > 100000 - measured)) {
    fprintf(stderr, "Invalid headless benchmark frame counts\n");
    goto cleanup;
  }
  if (!prepare_only &&
      !obs_engine_bench_init(&bench, warmup, measured, report_path)) {
    fprintf(stderr, "Could not allocate headless benchmark report\n");
    goto cleanup;
  }
  bench.process_start_ns = process_start_ns;
  bench.process_cpu_start_ns = process_cpu_start_ns;
  obs_engine_bench_profile_activate(&bench);
  bench.monad_boot_configured = monad_configured;
  bench.monad_boot_published = monad_published;
  if (manifest_loaded) {
    snprintf(bench.benchmark_name, sizeof(bench.benchmark_name), "%s",
             manifest.name);
    bench.manifest_min_mesh_count = manifest.min_mesh_count;
  }
  snprintf(bench.scene_path, sizeof(bench.scene_path), "%s", selected_scene);
  snprintf(bench.scene_cache_mode, sizeof(bench.scene_cache_mode), "%s",
           cache_mode);
  snprintf(bench.render_mode, sizeof(bench.render_mode), "%s", "headless");
  bench.render_width = width;
  bench.render_height = height;

  uint64_t init_start = obs_engine_bench_now_ns();
  if (!obs_headless_init(width, height, false)) {
    fprintf(stderr, "Failed to initialize headless Vulkan renderer\n");
    goto cleanup;
  }
  renderer_ready = true;
  if (override_sun_intensity)
    CTX_LIGHTING(&context)->sun.color[3] = benchmark_sun_intensity;
  bench.directional_sun_intensity = CTX_LIGHTING(&context)->sun.color[3];
  bench.directional_shadows_enabled =
      obs_lighting_needs_shadow_map(CTX_LIGHTING(&context), shadowsEnabled);
  benchmark_capture_gpu_memory(&bench, true);
  benchmark_capture_engine_gpu_allocations(&bench, true);
  bench.gpu_debug_labels_enabled = context.debugLabelsEnabled;
  const char *work_counters_env = getenv("OBSIDIAN_BENCH_WORK_COUNTERS");
  bench.work_counters_enabled =
      work_counters_env && strcmp(work_counters_env, "1") == 0;
  context.profileWorkCountersEnabled = bench.work_counters_enabled;
  bench.window_init_ns = obs_engine_bench_now_ns() - init_start;
  bench.window_ready_ns = obs_engine_bench_now_ns();
  obs_frame_profile_enable(true);
  bench.gpu_timestamps_supported = obs_gpu_frame_profile_supported(
      &bench.gpu_timestamp_valid_bits, &bench.gpu_timestamp_period_ns);
  bench.gpu_timestamp_capture_enabled = capture_gpu_timestamps &&
                                        bench.gpu_timestamps_supported &&
                                        obs_gpu_frame_profile_enable();
  VkPhysicalDeviceProperties gpu_properties = {0};
  vkGetPhysicalDeviceProperties(context.physicalDevice, &gpu_properties);
  snprintf(bench.gpu_device_name, sizeof(bench.gpu_device_name), "%s",
           gpu_properties.deviceName);
  bench.gpu_vendor_id = gpu_properties.vendorID;
  bench.gpu_device_id = gpu_properties.deviceID;
  bench.gpu_driver_version = gpu_properties.driverVersion;
  bench.gpu_api_version = gpu_properties.apiVersion;
  bench.gpu_queue_family = context.graphicsQueueFamily;
  if (!bench.gpu_timestamp_period_ns)
    bench.gpu_timestamp_period_ns = gpu_properties.limits.timestampPeriod;

  struct stat scene_stat;
  if (stat(selected_scene, &scene_stat) == 0 && S_ISREG(scene_stat.st_mode))
    bench.scene_source_file_bytes = (uint64_t)scene_stat.st_size;
  uint64_t load_start = obs_engine_bench_now_ns();
  uint64_t load_cpu_start = obs_engine_bench_cpu_now_ns();
  size_t mesh_start = scene->meshes.count;
  size_t instance_start = scene->gltf_instance_count;
  bool loaded = load_gltf(selected_scene, scene);
  bench.scene_load_wall_ns = obs_engine_bench_now_ns() - load_start;
  bench.app_setup_ns = bench.scene_load_wall_ns;
  bench.scene_load_cpu_ns = obs_engine_bench_cpu_now_ns() - load_cpu_start;
  bench.scene_geometry_upload_gpu_ns = context.profileGeometryUploadGpuNs;
  bench.scene_geometry_upload_gpu_sampled =
      context.profileGeometryUploadGpuSampled;
  if (!loaded || scene->meshes.count <= mesh_start ||
      scene->gltf_instance_count <= instance_start) {
    fprintf(stderr, "Headless benchmark scene has no loaded geometry: %s\n",
            selected_scene);
    goto cleanup;
  }
  bench.scene_loaded = true;
  bench.scene_mesh_count = (uint32_t)(scene->meshes.count - mesh_start);
  bench.scene_gltf_instance_count =
      (uint32_t)(scene->gltf_instance_count - instance_start);
  for (size_t i = instance_start; i < scene->gltf_instance_count; ++i)
    bench.scene_texture_count += scene->gltf_instances[i].texture_count;
  for (size_t i = mesh_start; i < scene->meshes.count; ++i) {
    if (scene->meshes.items[i].aoIndex >= 0)
      ++bench.scene_ao_mesh_count;
    bench.scene_vertex_count += scene->meshes.items[i].vertexCount;
    bench.scene_index_count += scene->meshes.items[i].indexCount;
    bench.scene_meshlet_count += scene->meshes.items[i].meshletCount;
  }
  bench.gpu_geometry_buffers_sampled = true;
  bench.gpu_vertex_buffer_capacity_bytes = context.megaVertexBufferSize;
  bench.gpu_vertex_buffer_used_bytes =
      (uint64_t)context.megaVertexBufferOffset * sizeof(Vertex);
  bench.gpu_index_buffer_capacity_bytes = context.megaIndexBufferSize;
  bench.gpu_index_buffer_used_bytes =
      (uint64_t)context.megaIndexBufferOffset * sizeof(uint32_t);
  if (manifest_loaded && bench.scene_mesh_count < manifest.min_mesh_count) {
    fprintf(stderr, "Manifest expected at least %u meshes; loaded %u\n",
            manifest.min_mesh_count, bench.scene_mesh_count);
    goto cleanup;
  }

  if (prepare_only) {
    result = EXIT_SUCCESS;
    goto cleanup;
  }

  const uint32_t total = warmup + measured;
  for (uint64_t sequence = 0; sequence < total; ++sequence) {
    uint64_t frame_start = obs_engine_bench_now_ns();
    if (!obs_headless_render_frame(NULL, NULL)) {
      fprintf(stderr, "Headless Vulkan frame failed at sequence %" PRIu64 "\n",
              sequence);
      goto cleanup;
    }
    ObsFrameProfile profile = {0};
    obs_frame_profile_get(&profile);
    ObsEngineBenchFrame frame = {
        .total_ns = obs_engine_bench_now_ns() - frame_start,
        .begin_fence_wait_ns = profile.begin_fence_wait_ns,
        .renderer_setup_ns = profile.renderer_setup_ns,
        .uniform_update_ns = profile.uniform_update_ns,
        .monad_callback_ns = profile.monad_callback_ns,
        .mesh_sync_ns = profile.mesh_sync_ns,
        .command_record_ns = profile.command_record_ns,
        .command_render_graph_ns = profile.command_render_graph_ns,
        .command_transparency_ns = profile.command_transparency_ns,
        .command_ui_ns = profile.command_ui_ns,
        .queue_submit_ns = profile.queue_submit_ns,
        .gpu_completion_wait_ns = profile.image_fence_wait_ns,
        .vertex2d_count = profile.vertex2d_count,
        .vertex2d_data_bytes = profile.vertex2d_data_bytes,
        .work_counters_sampled = bench.work_counters_enabled,
        .draw_calls = context.profileDrawCalls,
        .indirect_draws = context.profileIndirectDraws,
        .submitted_vertices = context.profileSubmittedVertices,
        .submitted_indices = context.profileSubmittedIndices,
        .mesh_task_workgroups = context.profileMeshTaskWorkgroups,
        .compute_dispatches = context.profileComputeDispatches,
        .compute_workgroups = context.profileComputeWorkgroups,
    };
    obs_engine_bench_record_frame(&bench, sequence, &frame);
    if (sequence >= warmup)
      record_benchmark_work_stages(&bench, &context);
    ObsGpuFrameProfile sample = {0};
    while (obs_gpu_frame_profile_take(&sample))
      obs_engine_bench_record_gpu(&bench, sample.frame_sequence, &sample);
  }
  if (!obs_engine_bench_done(&bench)) {
    fprintf(stderr, "Headless benchmark captured %u/%u frames\n",
            bench.captured_frames, measured);
    goto cleanup;
  }
  result = EXIT_SUCCESS;

cleanup:
  if (renderer_ready) {
    uint64_t idle_start = obs_engine_bench_now_ns();
    (void)vkDeviceWaitIdle(context.device);
    bench.device_wait_idle_ns = obs_engine_bench_now_ns() - idle_start;
    obs_gpu_frame_profile_flush();
    ObsGpuFrameProfile sample = {0};
    while (obs_gpu_frame_profile_take(&sample))
      obs_engine_bench_record_gpu(&bench, sample.frame_sequence, &sample);
    benchmark_capture_gpu_memory(&bench, false);
    benchmark_capture_engine_gpu_allocations(&bench, false);
    obs_gpu_frame_profile_destroy();
    uint64_t cleanup_start = obs_engine_bench_now_ns();
    cleanup(&context);
    bench.cleanup_ns = obs_engine_bench_now_ns() - cleanup_start;
  }
  if (monad_started)
    obs_monad_engine_stop();
  bench.process_end_ns = obs_engine_bench_now_ns();
  uint64_t cpu_end = obs_engine_bench_cpu_now_ns();
  bench.process_cpu_ns = cpu_end >= bench.process_cpu_start_ns
                             ? cpu_end - bench.process_cpu_start_ns
                             : 0;
  obs_engine_bench_capture_memory(&bench);
  if (result == EXIT_SUCCESS && !prepare_only &&
      !obs_engine_bench_write(&bench)) {
    fprintf(stderr, "Failed to write headless benchmark report\n");
    result = EXIT_FAILURE;
  }
  obs_engine_bench_profile_activate(NULL);
  obs_engine_bench_destroy(&bench);
  return result;
}

static void draw_demo_overlays(Font *font) {
  fps(font, 200, 200, RED);
  const float length = 10000.0f;
  line((vec3){-length, 0.0f, 0.0f}, (vec3){length, 0.0f, 0.0f}, CT.x);
  line((vec3){0.0f, -length, 0.0f}, (vec3){0.0f, length, 0.0f}, CT.y);
  line((vec3){0.0f, 0.0f, -length}, (vec3){0.0f, 0.0f, length}, CT.z);
}

static void poll_async_scene(ObsAsyncGltf *async, bool enabled,
                             uint64_t *ticket) {
  if (!enabled || !async || !ticket || !*ticket)
    return;
  ObsGltfAsyncStatus state = obs_gltf_async_poll(async, *ticket);
  if (state == OBS_GLTF_ASYNC_PUBLISHED || state == OBS_GLTF_ASYNC_FAILED ||
      state == OBS_GLTF_ASYNC_CANCELLED || state == OBS_GLTF_ASYNC_NEEDS_COOK) {
    fprintf(stderr, "[SceneLoad] ticket %llu finished: %d\n",
            (unsigned long long)*ticket, (int)state);
    if (state == OBS_GLTF_ASYNC_NEEDS_COOK)
      fprintf(stderr,
              "[SceneLoad] validated uncached glTF: %u primitives, %" PRIu64
              " triangles; async cooker unavailable, scene unchanged\n",
              async->source_plan.primitives, async->source_plan.triangles);
    *ticket = 0;
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    const char *output = NULL;
    const char *benchmark_report = NULL;
    const char *benchmark_scene = NULL;
    const char *monad_boot = NULL;
    const char *eval_source = NULL;
    const char *eval_file = NULL;
    const char *agent_report = NULL;
    const char *test_obj_async_path = NULL;
    bool middle_orbit_test = false;
    bool editor_panels_test = false;
    bool antialiasing_transition_test = false;
    bool test_matcap = false;
    bool test_no_sun = false;
    uint32_t width = 800, height = 600, frames = 1;
    for (int i = 1; i < argc; ++i) {
      if ((strcmp(argv[i], "--headless-raw") == 0 ||
           strcmp(argv[i], "--output") == 0) &&
          i + 1 < argc) {
        output = argv[++i];
      } else if (strcmp(argv[i], "--headless-benchmark") == 0 && i + 1 < argc) {
        benchmark_report = argv[++i];
      } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
        benchmark_scene = argv[++i];
      } else if (strcmp(argv[i], "--monad-boot") == 0 && i + 1 < argc) {
        monad_boot = argv[++i];
      } else if ((strcmp(argv[i], "--eval") == 0 ||
                  strcmp(argv[i], "-e") == 0) &&
                 i + 1 < argc) {
        eval_source = argv[++i];
      } else if (!strncmp(argv[i], "--eval=", 7)) {
        eval_source = argv[i] + 7;
      } else if (strcmp(argv[i], "--eval-file") == 0 && i + 1 < argc) {
        eval_file = argv[++i];
      } else if (strcmp(argv[i], "--agent-report") == 0 && i + 1 < argc) {
        agent_report = argv[++i];
      } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
        if (!obs_headless_parse_frames(argv[++i], &frames)) {
          fprintf(stderr, "Invalid --frames (expected 1..10000)\n");
          return EXIT_FAILURE;
        }
      } else if (strcmp(argv[i], "--middle-orbit-test") == 0) {
        middle_orbit_test = true;
      } else if (strcmp(argv[i], "--editor-panels-test") == 0) {
        editor_panels_test = true;
      } else if (strcmp(argv[i], "--test-antialiasing") == 0) {
        antialiasing_transition_test = true;
      } else if (strcmp(argv[i], "--test-matcap") == 0) {
        test_matcap = true;
      } else if (strcmp(argv[i], "--test-no-sun") == 0) {
        test_no_sun = true;
      } else if (strcmp(argv[i], "--test-obj-async") == 0 && i + 1 < argc) {
        test_obj_async_path = argv[++i];
      } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
        if (!parse_dimension(argv[++i], &width)) {
          fprintf(stderr, "Invalid --width (expected 1..8192)\n");
          return EXIT_FAILURE;
        }
      } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
        if (!parse_dimension(argv[++i], &height)) {
          fprintf(stderr, "Invalid --height (expected 1..8192)\n");
          return EXIT_FAILURE;
        }
      } else {
        fprintf(stderr,
                "Usage: %s (--output PATH | --headless-raw PATH | "
                "--headless-benchmark REPORT [--scene GLTF | "
                "--monad-boot MODULE]) [--eval MONAD-SOURCE] "
                "[--eval-file MODULE] [--frames 1..10000] "
                "[--agent-report JSON] [--middle-orbit-test] "
                "[--editor-panels-test] [--test-antialiasing] [--test-matcap] "
                "[--test-no-sun] "
                "[--test-obj-async PATH] "
                "[--width N] [--height N]\n",
                argv[0]);
        return EXIT_FAILURE;
      }
    }
    if (benchmark_report) {
      if (output || eval_source || eval_file || middle_orbit_test ||
          editor_panels_test || antialiasing_transition_test ||
          test_obj_async_path || agent_report || frames != 1 ||
          (!benchmark_scene && !monad_boot) || !*benchmark_report ||
          (monad_boot && setenv("OBSIDIAN_MONAD_BOOT", monad_boot, 1) != 0)) {
        fprintf(stderr, "Invalid headless benchmark arguments\n");
        return EXIT_FAILURE;
      }
      return run_headless_benchmark(benchmark_report, benchmark_scene, width,
                                    height);
    }
    if (!output || !*output || benchmark_scene || monad_boot ||
        (eval_source && eval_file) ||
        (middle_orbit_test && editor_panels_test) ||
        (antialiasing_transition_test &&
         (middle_orbit_test || editor_panels_test || eval_source || eval_file ||
          agent_report)) ||
        (eval_file && !*eval_file) ||
        (test_obj_async_path && !*test_obj_async_path) ||
        (agent_report &&
         (!*agent_report || strcmp(agent_report, output) == 0))) {
      fprintf(stderr,
              "Usage: %s (--output PATH | --headless-raw PATH | "
              "--headless-benchmark REPORT [--scene GLTF | "
              "--monad-boot MODULE]) [--eval MONAD-SOURCE] "
              "[--eval-file MODULE] [--frames 1..10000] "
              "[--agent-report JSON] [--middle-orbit-test] "
              "[--editor-panels-test] [--test-antialiasing] [--test-matcap] "
              "[--test-no-sun] "
              "[--test-obj-async PATH] "
              "[--width N] [--height N]\n",
              argv[0]);
      return EXIT_FAILURE;
    }
    return run_headless_raw(output, eval_source, eval_file, agent_report, width,
                            height, frames, middle_orbit_test,
                            editor_panels_test, antialiasing_transition_test,
                            test_matcap, test_no_sun, test_obj_async_path);
  }
  const char *bench_frames_env = getenv("OBSIDIAN_BENCHMARK_FRAMES");
  bool benchmark_enabled = bench_frames_env != NULL;
  ObsEngineBench benchmark = {0};
  if (benchmark_enabled) {
    uint32_t warmup_frames = 120;
    uint32_t measured_frames = 0;
    const char *report_path = getenv("OBSIDIAN_BENCHMARK_REPORT");
    if (!parse_bench_count("OBSIDIAN_BENCHMARK_FRAMES", 0, 1, 100000,
                           &measured_frames) ||
        !parse_bench_count("OBSIDIAN_BENCHMARK_WARMUP", 120, 0, 100000,
                           &warmup_frames) ||
        warmup_frames > 100000 - measured_frames ||
        !obs_engine_bench_init(&benchmark, warmup_frames, measured_frames,
                               report_path ? report_path
                                           : "build/bench/engine.json")) {
      fprintf(stderr, "Invalid engine benchmark configuration\n");
      return EXIT_FAILURE;
    }
    obs_engine_bench_profile_activate(&benchmark);
  }

  uint64_t init_start = benchmark_enabled ? obs_engine_bench_now_ns() : 0;
  initWindow(WIDTH, HEIGHT, "Obsidian Engine");
  if (benchmark_enabled) {
    benchmark_capture_gpu_memory(&benchmark, true);
    benchmark_capture_engine_gpu_allocations(&benchmark, true);
    snprintf(benchmark.render_mode, sizeof(benchmark.render_mode), "%s",
             "window");
    benchmark.render_width = WIDTH;
    benchmark.render_height = HEIGHT;
    benchmark.window_init_ns = obs_engine_bench_now_ns() - init_start;
    benchmark.window_ready_ns = obs_engine_bench_now_ns();
    obs_frame_profile_enable(true);
    const char *timestamp_mode = getenv("OBSIDIAN_BENCH_GPU_TIMESTAMPS");
    const bool capture_gpu_timestamps =
        !timestamp_mode || strcmp(timestamp_mode, "0") != 0;
    benchmark.gpu_timestamps_supported =
        obs_gpu_frame_profile_supported(&benchmark.gpu_timestamp_valid_bits,
                                        &benchmark.gpu_timestamp_period_ns);
    benchmark.gpu_timestamp_capture_enabled =
        capture_gpu_timestamps && benchmark.gpu_timestamps_supported &&
        obs_gpu_frame_profile_enable();
    benchmark.gpu_debug_labels_enabled = context.debugLabelsEnabled;
    VkPhysicalDeviceProperties gpu_properties = {0};
    vkGetPhysicalDeviceProperties(context.physicalDevice, &gpu_properties);
    snprintf(benchmark.gpu_device_name, sizeof(benchmark.gpu_device_name), "%s",
             gpu_properties.deviceName);
    benchmark.gpu_vendor_id = gpu_properties.vendorID;
    benchmark.gpu_device_id = gpu_properties.deviceID;
    benchmark.gpu_driver_version = gpu_properties.driverVersion;
    benchmark.gpu_api_version = gpu_properties.apiVersion;
    benchmark.gpu_queue_family = context.graphicsQueueFamily;
    if (!benchmark.gpu_timestamp_period_ns)
      benchmark.gpu_timestamp_period_ns = gpu_properties.limits.timestampPeriod;
    benchmark.monad_boot_configured = getenv("OBSIDIAN_MONAD_BOOT") != NULL;
    if (benchmark.monad_boot_configured) {
      ObsMonadReloadReport boot_report = {0};
      obs_monad_engine_reload_report(&boot_report);
      benchmark.monad_boot_published =
          boot_report.state == OBS_MONAD_RELOAD_SUCCEEDED;
    }
  }

  uint64_t app_setup_start = benchmark_enabled ? obs_engine_bench_now_ns() : 0;

  /* load_obj("./assets/obj/teapot.obj", "teapot",  red); */
  /* load_obj("./assets/obj/cow.obj", "cow", blue); */

  /* load_gltf("./assets/gltf/TwoSidedPlane/glTF/TwoSidedPlane.gltf", &scene);
   * // FAIL TODO CULL and toggle switch in the inspector for backface culling
   */

  /* load_gltf("./assets/gltf/VertexColorTest.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/xmp.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene); //
   * PASS */
  /* load_gltf("./assets/gltf/MetalRoughSpheres.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/AnimatedMorphCube.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/AnimatedMorphSphere.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/AlphaBlendModeTest.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/UnlitTest.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Unicode❤♻Test.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/SimpleMorph/glTF/SimpleMorph.gltf", &scene); //
   * PASS */
  /* load_gltf("./assets/gltf/MaterialsVariantsShoe.glb", &scene); // TODO
   * variants in the editor */
  /* load_gltf("./assets/gltf/BoxAnimated.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Box.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Corset.glb", &scene); // PASS (but it's really
   * small) */
  /* load_gltf("./assets/gltf/CesiumMan.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Fox.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/RecursiveSkeletons.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/RiggedFigure.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Sponza/glTF/Sponza.gltf", &scene); // PASS */
  /* load_gltf("./assets/gltf/CarbonFibre.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/MorphStressTest.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Avocado.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/Lantern.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/TextureCoordinateTest.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/AnimatedColorsCube.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/CubeVisibility.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/EmissiveStrengthTest.glb", &scene); // FAIL also
   * the sun goes thorugh walls */
  /* load_gltf("./assets/gltf/InterpolationTest.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/DragonAttenuation.glb", &scene); // PASS */
  /* load_gltf("./assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &scene);
   * // PASS */
  /* load_gltf("./assets/gltf/MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb",
   * &scene); // PASS */
  /* load_gltf("./assets/gltf/DragonDispersion.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/DispersionTest.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/IridescenceMetallicSpheres/glTF/IridescenceMetallicSpheres.gltf",
   * &scene); // FAIL KHR_materials_iridescence */
  /* load_gltf("./assets/gltf/IORTestGrid.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/TransmissionTest.glb", &scene); // FAIL */
  /* load_gltf("./assets/gltf/NodePerformanceTest.glb", &scene); // FAIL */

  Font *jetbrains = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 81);

  // Bake and load the HDR Environment Map
  loadIBL(&context, "./assets/hdr/monochrome_studio_02_4k.hdr");
  /* loadIBL(&context, "./assets/hdr/meadow_2_4k.hdr"); */
  /* loadIBL(&context, "./assets/hdr/ferndale_studio_05_4k.hdr"); */
  reset_material();

  /* Optional production async path; uncached supported static scenes are
   * cooked by the worker rather than by the render-thread legacy loader. */
  ObsAsyncGltf async_gltf = {0};
  bool async_gltf_enabled = false;
  uint64_t async_gltf_ticket = 0;
  const char *async_source = getenv("OBSIDIAN_ASYNC_GLTF");
  if (async_source && *async_source &&
      obs_gltf_async_init(&async_gltf, scene, &context)) {
    async_gltf_enabled = true;
    async_gltf_ticket = obs_gltf_async_request(&async_gltf, async_source);
    if (!async_gltf_ticket)
      fprintf(stderr, "[SceneLoad] could not queue '%s'\n", async_source);
  }

  // Variables for delta time
  double lastFrameTime = glfwGetTime();

  /* Blender's startup scene: one unit-radius cube centered at the origin. */
  cube((vec3){0.0f, 0.0f, 0.0f}, 2.0f, WHITE);
  const char *bench_scene =
      benchmark_enabled ? getenv("OBSIDIAN_BENCH_SCENE") : NULL;
  if (bench_scene && *bench_scene) {
    if (strlen(bench_scene) >= sizeof(benchmark.scene_path)) {
      fprintf(stderr, "OBSIDIAN_BENCH_SCENE path is too long\n");
      return EXIT_FAILURE;
    }
    snprintf(benchmark.scene_path, sizeof(benchmark.scene_path), "%s",
             bench_scene);
    const char *cache_mode = getenv("OBSIDIAN_BENCH_SCENE_CACHE");
    snprintf(benchmark.scene_cache_mode, sizeof(benchmark.scene_cache_mode),
             "%s",
             cache_mode && strcmp(cache_mode, "off") == 0 ? "cold" : "warm");
    struct stat scene_stat;
    if (stat(bench_scene, &scene_stat) == 0 && S_ISREG(scene_stat.st_mode))
      benchmark.scene_source_file_bytes = (uint64_t)scene_stat.st_size;
    size_t mesh_start = scene->meshes.count;
    size_t gltf_start = scene->gltf_instance_count;
    uint64_t scene_load_start = obs_engine_bench_now_ns();
    uint64_t scene_load_cpu_start = obs_engine_bench_cpu_now_ns();
    context.profileGeometryUploadGpuNs = 0;
    context.profileGeometryUploadGpuSampled = false;
    bool scene_loaded = load_gltf(bench_scene, scene);
    benchmark.scene_load_wall_ns = obs_engine_bench_now_ns() - scene_load_start;
    benchmark.scene_load_cpu_ns =
        obs_engine_bench_cpu_now_ns() - scene_load_cpu_start;
    benchmark.scene_geometry_upload_gpu_ns = context.profileGeometryUploadGpuNs;
    benchmark.scene_geometry_upload_gpu_sampled =
        context.profileGeometryUploadGpuSampled;
    if (!scene_loaded || scene->meshes.count < mesh_start ||
        scene->gltf_instance_count < gltf_start) {
      fprintf(stderr, "Benchmark scene failed to load: %s\n", bench_scene);
      return EXIT_FAILURE;
    }
    benchmark.scene_loaded = true;
    benchmark.scene_mesh_count = (uint32_t)(scene->meshes.count - mesh_start);
    benchmark.scene_gltf_instance_count =
        (uint32_t)(scene->gltf_instance_count - gltf_start);
    for (size_t i = gltf_start; i < scene->gltf_instance_count; ++i)
      benchmark.scene_texture_count += scene->gltf_instances[i].texture_count;
    for (size_t i = mesh_start; i < scene->meshes.count; ++i) {
      benchmark.scene_vertex_count += scene->meshes.items[i].vertexCount;
      benchmark.scene_index_count += scene->meshes.items[i].indexCount;
    }
    if (!benchmark.scene_mesh_count || !benchmark.scene_vertex_count) {
      fprintf(stderr, "Benchmark scene loaded no renderable meshes: %s\n",
              bench_scene);
      return EXIT_FAILURE;
    }
  }
  if (benchmark_enabled)
    benchmark.app_setup_ns = obs_engine_bench_now_ns() - app_setup_start;

  if (obs_editor_enabled()) {
    if (!obs_scene_buffers_init(scene, "Scene", bench_scene) ||
        !obs_editor_frames_init(context.primary)) {
      fprintf(stderr, "Could not initialize scene buffers/editor frames\n");
      editor_cleanup();
      cleanup(&context);
      return EXIT_FAILURE;
    }
  }

  while (!windowShouldClose() &&
         (!benchmark_enabled || !obs_engine_bench_done(&benchmark))) {
    const bool editor_frames_active = obs_editor_frames_first() != NULL;
    if (editor_frames_active)
      obs_editor_frames_begin_iteration();
    poll_async_scene(&async_gltf, async_gltf_enabled, &async_gltf_ticket);
    ObsEditorFrame *frame =
        editor_frames_active ? obs_editor_frames_first() : NULL;
    bool rendered_any = false;
    do {
      ObsEditorFrame *next = frame ? obs_editor_frame_next(frame) : NULL;
      if (frame) {
        obs_editor_frame_activate(frame);
        ObsWindow *frame_window = obs_editor_frame_window(frame);
        if (obs_editor_frame_is_closing(frame) || !frame_window ||
            frame_window->suspended) {
          frame = next;
          continue;
        }
      }
      uint64_t frame_start = benchmark_enabled ? obs_engine_bench_now_ns() : 0;
      beginFrame();
      uint64_t app_update_end =
          benchmark_enabled ? obs_engine_bench_now_ns() : 0;

      // 3D GEOMETRY
      /* vec3 v0 = { -0.03f, -0.03f, 0.0f }; */
      /* vec3 v1 = {  0.03f, -0.03f, 0.0f }; */
      /* vec3 v2 = {  0.03f,  0.03f, 0.0f }; */
      /* vec3 v3 = { -0.03f,  0.03f, 0.0f }; */
      /* vec3 center = { 0.0f, 0.0f, 0.0f }; */
      /* triangle(v0, center, v1, RED); */
      /* triangle(v1, center, v2, GREEN); */
      /* triangle(v2, center, v3, BLUE); */
      /* triangle(v3, center, v0, YELLOW); */

      // 64x64 is smooth and highly detailed, generating 24,576 dynamic vertices
      // per frame!
      /* set_material(&rockMat); */
      /* sphere((vec3){0.0f, 0.0f, 30.0f}, 5.0f, 64, 64, WHITE); */
      /* reset_material(); */

      // Translate the cube directly in front of the camera so we can actually
      // see it!
      /* if (scene.meshes.count > 0) { */
      /*     mat4 offset_transform; */
      /*     glm_mat4_identity(offset_transform); */
      /*     glm_translate(offset_transform, (vec3){0.0f, 3.0f, 5.0f}); // X=0,
       * Y=3 (Camera Height), Z=5 (Forward) */

      /*     mat4 temp; */
      /*     glm_mat4_copy(scene.meshes.items[0].model, temp); */
      /*     glm_mat4_mul(offset_transform, temp, scene.meshes.items[0].model);
       */
      /*     markMeshesSSBODirty(&context); */
      /* } */

      // TODO
      /* set_material(&rockMat); */
      /* cube((vec3){0.0f, 0.0f, 10.0f}, 3.0f, WHITE); */
      /* reset_material(); */

      /* text(jetbrains,
       * "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~",
       * 150, 50, WHITE); */

      // Draw 3D text at world position
      /* text3D(jetbrains, "Hello 3D!", (vec3){0.0f, 5.0f, 10.0f}, 6, WHITE); */
      /* text3D(jetbrains, "Press SPACE", (vec3){0.0f, 4.5f, 10.0f}, 6, RED); */

      // 2D GEOMETRY
      /* quad2D((vec2){10, 10}, (vec2){50, 50}, BLUE); */
      /* quad2D((vec2){70, 10}, (vec2){50, 50}, WHITE); */
      /* quad2D((vec2){10, 70}, (vec2){50, 50}, RED); */
      /* quad2D((vec2){70, 70}, (vec2){50, 50}, GREEN); */

      if (!frame || !obs_editor_frame_is_2d_only(frame))
        draw_demo_overlays(jetbrains);

      const bool frame_is_primary =
          !frame || obs_editor_frame_is_primary(frame);
      endFrame();
      rendered_any = true;
      frame = next;
      if (benchmark_enabled && frame_is_primary) {
        uint64_t frame_end = obs_engine_bench_now_ns();
        ObsFrameProfile profile = {0};
        obs_frame_profile_get(&profile);
        ObsEngineBenchFrame sample = {
            .total_ns = frame_end - frame_start,
            .app_update_ns = app_update_end - frame_start,
            .app_draw_ns = frame_end - app_update_end,
            .event_poll_ns = profile.event_poll_ns,
            .begin_fence_wait_ns = profile.begin_fence_wait_ns,
            .swapchain_maintenance_ns = profile.swapchain_maintenance_ns,
            .renderer_setup_ns = profile.renderer_setup_ns,
            .uniform_update_ns = profile.uniform_update_ns,
            .frame_capability_ns = profile.frame_capability_ns,
            .monad_callback_ns = profile.monad_callback_ns,
            .scene_animation_ns = profile.scene_animation_ns,
            .physics_ns = profile.physics_ns,
            .camera_ns = profile.camera_ns,
            .mesh_sync_ns = profile.mesh_sync_ns,
            .camera_animation_ns = profile.camera_animation_ns,
            .editor_ns = profile.editor_ns,
            .geometry_upload_ns = profile.geometry_upload_ns,
            .acquire_image_ns = profile.acquire_image_ns,
            .image_fence_wait_ns = profile.image_fence_wait_ns,
            .command_record_ns = profile.command_record_ns,
            .command_render_graph_ns = profile.command_render_graph_ns,
            .command_transparency_ns = profile.command_transparency_ns,
            .command_ui_ns = profile.command_ui_ns,
            .queue_submit_ns = profile.queue_submit_ns,
            .queue_present_ns = profile.queue_present_ns,
            .vertex2d_data_bytes = profile.vertex2d_data_bytes,
            .vertex2d_count = profile.vertex2d_count};
        obs_engine_bench_record_frame(&benchmark, profile.frame_sequence,
                                      &sample);
        if (profile.frame_sequence >= benchmark.warmup_frames)
          record_benchmark_work_stages(&benchmark, &context);
        ObsGpuFrameProfile gpu_sample;
        while (obs_gpu_frame_profile_take(&gpu_sample))
          obs_engine_bench_record_gpu(&benchmark, gpu_sample.frame_sequence,
                                      &gpu_sample);
      }
    } while (frame);
    if (editor_frames_active)
      obs_editor_frames_end_iteration();
    if (editor_frames_active && !rendered_any)
      glfwWaitEventsTimeout(0.05);
  }

  uint64_t shutdown_start = benchmark_enabled ? obs_engine_bench_now_ns() : 0;
  VkResult shutdown_idle = vkDeviceWaitIdle(context.device);
  if (async_gltf_enabled && (shutdown_idle != VK_SUCCESS ||
                             !obs_gltf_async_shutdown_quiesced(&async_gltf))) {
    /* Fail closed: do not destroy resources with unproven GPU ownership. */
    fprintf(stderr, "[SceneLoad] unsafe GPU retirement; skipping teardown\n");
    return EXIT_FAILURE;
  }
  if (shutdown_idle != VK_SUCCESS || !editor_async_gltf_shutdown_quiesced() ||
      !editor_async_obj_shutdown_quiesced()) {
    fprintf(stderr, "[SceneLoad] unsafe editor asset-load retirement\n");
    return EXIT_FAILURE;
  }
  if (benchmark_enabled) {
    benchmark.device_wait_idle_ns = obs_engine_bench_now_ns() - shutdown_start;
    benchmark_capture_gpu_memory(&benchmark, false);
    benchmark_capture_engine_gpu_allocations(&benchmark, false);
    obs_gpu_frame_profile_flush();
    ObsGpuFrameProfile gpu_sample;
    while (obs_gpu_frame_profile_take(&gpu_sample))
      obs_engine_bench_record_gpu(&benchmark, gpu_sample.frame_sequence,
                                  &gpu_sample);
    obs_gpu_frame_profile_destroy();
  }
  uint64_t cleanup_start = benchmark_enabled ? obs_engine_bench_now_ns() : 0;
  obs_editor_frames_shutdown();
  vertico_cleanup();
  editor_cleanup();
  cleanup(&context);
  if (benchmark_enabled) {
    benchmark.cleanup_ns = obs_engine_bench_now_ns() - cleanup_start;
    benchmark.process_end_ns = obs_engine_bench_now_ns();
    uint64_t process_cpu_end_ns = obs_engine_bench_cpu_now_ns();
    benchmark.process_cpu_ns =
        process_cpu_end_ns >= benchmark.process_cpu_start_ns
            ? process_cpu_end_ns - benchmark.process_cpu_start_ns
            : 0;
    obs_engine_bench_capture_memory(&benchmark);
    bool report_written = obs_engine_bench_write(&benchmark);
    obs_engine_bench_profile_activate(NULL);
    obs_engine_bench_destroy(&benchmark);
    if (!report_written) {
      fprintf(stderr, "Failed to write engine benchmark report\n");
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}
