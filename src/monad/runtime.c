/// runtime.c — Embedded Monad lifecycle and live module loading
/// Implementation
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "effect_policy.h"
#include "hotreload.h"
#include "hotreload_stats.h"
#include "bindings.h"
#include "repl_service.h"
#include "native_read.h"
#include "dispatch_metrics.h"
#include "source_stage.h"
#include "semantic_preflight.h"
#include "camera.h"
#include "benchmark_manifest.h"
#include "live_editor.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ObsMonadRuntime s_engine;
static pthread_t s_owner;
static uint64_t s_next_generation;
static bool s_camera_policy_available;
static bool s_middle_orbit_policy_available;
static ObsReloadStats s_reload_stats;
static ObsPublishMetrics s_publish_metrics;
static ObsMonadReloadReport s_report;

static void refresh_camera_policy(monad_t *runtime) {
    monad_i64_function_t next = NULL;
    monad_error_t error = {0};
    s_camera_policy_available =
        runtime && monad_find_i64_function(runtime,
            "obsidian_camera_step_mm", &next, &error) == MONAD_OK && next;
    monad_i64_function_t yaw = NULL;
    monad_i64_function_t pitch = NULL;
    s_middle_orbit_policy_available = runtime &&
        monad_find_i64_function(runtime, "obsidian_middle_orbit_yaw_urad",
                                &yaw, &error) == MONAD_OK && yaw &&
        monad_find_i64_function(runtime, "obsidian_middle_orbit_pitch_urad",
                                &pitch, &error) == MONAD_OK && pitch;
}
static ObsReloadWatcher s_reload_watch;
static ObsSourceStager s_source_stage;
static uint64_t s_source_ticket;
static bool s_source_stage_enabled;
static ObsPreflightReport s_last_preflight;

/* Defined below the engine module loader. */
static bool boot_load_staged(const ObsSourceSnapshot *snapshot);

/// Reload configuration
static bool watch_enabled(void) {
    const char *setting = getenv("OBSIDIAN_MONAD_WATCH");
    return setting && (strcmp(setting, "1") == 0 ||
                       strcmp(setting, "true") == 0);
}

static const char *boot_module_path(void) {
    const char *path = getenv("OBSIDIAN_MONAD_BOOT");
    return path && *path ? path : "monad/obsidian/editor-orbit.mon";
}


static void reload_report(ObsMonadReloadState state, const char *detail) {
    s_report.state = state;
    snprintf(s_report.message, sizeof(s_report.message), "%s",
             detail ? detail : "no diagnostics supplied");
}

void obs_monad_engine_reload_report(ObsMonadReloadReport *out) {
    if (out && obs_monad_on_owner_thread()) *out = s_report;
}

void obs_monad_engine_reload_stats(ObsReloadStats *out) {
    if (out && obs_monad_on_owner_thread()) *out = s_reload_stats;
}

void obs_monad_engine_publish_metrics(ObsPublishMetrics *out) {
    if (out && obs_monad_on_owner_thread()) *out = s_publish_metrics;
}
void obs_monad_engine_preflight_report(ObsPreflightReport *out) {
    if (out && obs_monad_on_owner_thread()) *out = s_last_preflight;
}
bool obs_monad_source_worker_ready(void) {
    return obs_monad_on_owner_thread() && s_source_stage_enabled;
}

bool obs_monad_source_snapshot_ready(void) {
    return obs_monad_on_owner_thread() && s_source_stage_enabled &&
        s_source_ticket &&
        s_engine.requests.pending_id[OBS_PUBLISH_BOOT] == s_source_ticket &&
        obs_source_stage_ready(&s_source_stage, s_source_ticket);
}

/// Owner-thread publication checkpoint
void obs_monad_engine_request_reload(void) {
    if (obs_monad_on_owner_thread())
        (void)obs_publish_queue_request_at(&s_engine.requests, OBS_PUBLISH_BOOT, obs_reload_clock_ns());
}

void obs_monad_engine_process_pending(void) {
    if (!obs_monad_on_owner_thread() || s_engine.requests.busy) return;
    const char *path = boot_module_path();
    if (watch_enabled() && path && *path &&
        obs_reload_poll(&s_reload_watch, path, obs_reload_clock_ns(),
                        UINT64_C(250000000), UINT64_C(350000000))) {
        /* Acknowledge AFTER successful queue admission, not after compilation.
         * Otherwise a stable change is requeued every frame while the worker
         * is preparing it, endlessly superseding the same source revision. */
        if (obs_publish_queue_request_at(&s_engine.requests, OBS_PUBLISH_BOOT, obs_reload_clock_ns()))
            obs_reload_acknowledge(&s_reload_watch);
    }

    ObsReplModel *repl = obs_monad_repl_model();
    if (repl->has_pending && !repl->executing)
        (void)obs_publish_queue_request_at(&s_engine.requests, OBS_PUBLISH_REPL, obs_reload_clock_ns());

    const uint64_t pending_boot = s_engine.requests.pending_id[OBS_PUBLISH_BOOT];
    if (s_source_stage_enabled && pending_boot && path && *path &&
        s_source_ticket != pending_boot) {
        s_source_ticket = obs_source_stage_request(&s_source_stage,
            pending_boot, path) ? pending_boot : 0;
    }
    /* An unprepared boot source must not block an already queued REPL command.
     * The owner thread neither waits for disk I/O nor touches a worker buffer. */
    unsigned ready = 1u << OBS_PUBLISH_REPL;
    if (!s_source_stage_enabled || !pending_boot || !path || !*path ||
        s_source_ticket != pending_boot ||
        obs_source_stage_ready(&s_source_stage, pending_boot))
        ready |= 1u << OBS_PUBLISH_BOOT;
    if (s_engine.publication.active_calls ||
        s_engine.publication.phase == OBS_LIVE_LOADING) return;
    ObsPublishTicket ticket = {0};
    if (!obs_publish_queue_begin_ready(&s_engine.requests, ready, &ticket)) return;
    const uint64_t checkpoint_start = obs_reload_clock_ns();
    const uint64_t queued_ns = s_engine.requests.active_since_ns;
    bool success = false;
    if (ticket.kind == OBS_PUBLISH_BOOT) {
        if (!path || !*path) {
            reload_report(OBS_MONAD_RELOAD_FAILED,
                          "OBSIDIAN_MONAD_BOOT is not configured");
        } else if (s_source_stage_enabled && s_source_ticket == ticket.id) {
            ObsSourceSnapshot snapshot = {0};
            if (obs_source_stage_take(&s_source_stage, ticket.id, &snapshot)) {
                s_last_preflight = snapshot.preflight;
                if (snapshot.status == OBS_STAGE_OK &&
                    (strcmp(snapshot.path, path) != 0 ||
                     !obs_source_snapshot_current(&snapshot))) {
                    /* The immutable snapshot is obsolete, not a candidate.
                     * Queue a fresh read; never compile stale disk contents. */
                    (void)obs_publish_queue_request(&s_engine.requests,
                                                    OBS_PUBLISH_BOOT);
                } else {
                    success = boot_load_staged(&snapshot);
                }
                obs_source_snapshot_dispose(&snapshot);
            }
        } else {
            const uint64_t prior_published = s_report.published;
            obs_monad_engine_boot_from_environment();
            success = s_report.published > prior_published &&
                      s_report.state == OBS_MONAD_RELOAD_SUCCEEDED;
        }
    } else {
        uint64_t previous_rejections = repl->rejected;
        obs_monad_repl_pump(&s_engine);
        success = !repl->has_pending && !repl->executing &&
                  repl->rejected == previous_rejections;
    }
    const uint64_t checkpoint_end = obs_reload_clock_ns();
    (void)obs_publish_metrics_record(&s_publish_metrics, ticket.kind,
        queued_ns, checkpoint_start, checkpoint_end, success);
    (void)obs_publish_queue_finish(&s_engine.requests, ticket, success);
}


/* The first guaranteed native ABI path: compiled Monad decides the camera
 * step; C only applies the resulting bounded host mutation.  It uses the
 * same monad_find_i64_function contract exercised by Glemax's host tests. */
bool obs_monad_camera_step_checked(void) {
    if (!obs_monad_on_owner_thread() || !s_camera_policy_available) return false;
    const bool profiling = obs_dispatch_metrics_enabled();
    const uint64_t started = profiling ? obs_dispatch_clock_ns() : 0;
    ObsNativeRead read = {0};
    monad_error_t error = {0};
    if (!obs_native_read_begin(&read, s_engine.runtime,
                               &s_engine.publication, &error)) {
        if (profiling) {
            const uint64_t end = obs_dispatch_clock_ns();
            obs_dispatch_metrics_record(OBS_DISPATCH_CAMERA,
                                        end - started, 0, end - started, false);
        }
        return false;
    }
    /* A pinned compiler-owned generation covers lookup and invocation. */
    monad_i64_function_t current = NULL;
    const bool found = obs_native_read_find_i64(&read, "obsidian_camera_step_mm",
                                                 &current, &error);
    const uint64_t guest_start = profiling ? obs_dispatch_clock_ns() : 0;
    bool valid = false;
    if (found) {
        const int64_t step = current(0);
        if (step >= -1000000 && step <= 1000000) {
            const float new_x = camera.position[0] + (float)step / 1000.0f;
            if (isfinite(new_x) && fabsf(new_x) <= 1.0e8f) {
                camera.position[0] = new_x;
                camera_update(&camera);
                valid = true;
            }
        }
    }
    const uint64_t guest_end = profiling ? obs_dispatch_clock_ns() : 0;
    obs_native_read_end(&read);
    if (profiling) {
        const uint64_t end = obs_dispatch_clock_ns();
        obs_dispatch_metrics_record(OBS_DISPATCH_CAMERA,
                                    guest_start - started,
                                    guest_end - guest_start,
                                    end - started, valid);
    }
    return valid;
}

void obs_monad_camera_step(void) {
    (void)obs_monad_camera_step_checked();
}

bool obs_monad_middle_orbit_delta(double screen_delta_pixels, bool vertical,
                                  float *radians) {
    if (!radians || !s_middle_orbit_policy_available ||
        !obs_monad_on_owner_thread() || !isfinite(screen_delta_pixels) ||
        fabs(screen_delta_pixels) > 10000.0)
        return false;

    const double milli_pixels = screen_delta_pixels * 1000.0;
    if (!isfinite(milli_pixels) || milli_pixels < (double)INT64_MIN ||
        milli_pixels > (double)INT64_MAX)
        return false;
    const int64_t input = (int64_t)llround(milli_pixels);

    ObsNativeRead read = {0};
    monad_error_t error = {0};
    if (!obs_native_read_begin(&read, s_engine.runtime,
                               &s_engine.publication, &error))
        return false;

    monad_i64_function_t policy = NULL;
    const char *name = vertical ? "obsidian_middle_orbit_pitch_urad"
                                : "obsidian_middle_orbit_yaw_urad";
    const bool found = obs_native_read_find_i64(&read, name, &policy, &error);
    const int64_t result = found ? policy(input) : 0;
    obs_native_read_end(&read);

    if (!found || result < -100000000 || result > 100000000)
        return false;
    *radians = (float)((double)result / 1000000.0);
    return isfinite(*radians);
}


bool obs_monad_code_enter(ObsCodeLease *lease) {
    return obs_monad_on_owner_thread() &&
           obs_publication_begin_call(&s_engine.publication, lease);
}
bool obs_monad_code_leave(ObsCodeLease *lease) {
    return obs_monad_on_owner_thread() &&
           obs_publication_end_call(&s_engine.publication, lease);
}

static bool runtime_ready(const ObsMonadRuntime *host) {
    return host && host->active && host->runtime && host->space;
}

bool obs_monad_on_owner_thread(void) {
    return runtime_ready(&s_engine) && pthread_equal(s_owner, pthread_self());
}
bool obs_monad_effect_allowed(uint32_t effects, ObsLifetime lifetime,
                              bool frame_active) {
    const ObsEffectEnvironment env = {
        .publication = &s_engine.publication,
        .owner_thread = obs_monad_on_owner_thread(),
        .engine_open = runtime_ready(&s_engine),
        .frame_open = frame_active
    };
    return obs_effect_admit(&env, effects, lifetime) == OBS_EFFECT_ADMIT;
}
bool obs_monad_runtime_mutation_allowed(void) {
    return obs_monad_effect_allowed(OBS_EFFECT_WRITE,
                                     OBS_LIFETIME_ENGINE, false);
}

static bool require_open(ObsMonadRuntime *host, monad_error_t *error) {
    if (runtime_ready(host) && obs_monad_on_owner_thread() && host == &s_engine)
        return true;
    if (error) {
        error->status = MONAD_ERROR_INVALID_ARGUMENT;
        error->message = "Monad host is closed or called from a non-owner thread";
    }
    return false;
}

bool obs_monad_open(ObsMonadRuntime *host, monad_error_t *error) {
    if (!host || host != &s_engine || host->active) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "only one inactive engine Monad host is supported";
        }
        return false;
    }
    s_owner = pthread_self();
    s_camera_policy_available = false;
    memset(&s_report, 0, sizeof(s_report));
    memset(&s_reload_stats, 0, sizeof(s_reload_stats));
    /* Explicit opt-in CPU stall threshold in microseconds. Default ~= 60 Hz.
     * 0/negative/oversized strings are ignored; no external config parser. */
    uint64_t stall_budget = OBS_PUBLISH_BUDGET_DEFAULT_US;
    const char *budget_env = getenv("OBSIDIAN_MONAD_STALL_BUDGET_US");
    if (budget_env && *budget_env >= '1' && *budget_env <= '9') {
        char *end = NULL;
        unsigned long long parsed = strtoull(budget_env, &end, 10);
        if (end && !*end && parsed > 0 && parsed <= UINT64_C(10000000))
            stall_budget = (uint64_t)parsed;
    }
    obs_publish_metrics_reset(&s_publish_metrics, stall_budget);
    const char *profile_ffi = getenv("OBSIDIAN_MONAD_PROFILE_FFI");
    obs_dispatch_metrics_reset(profile_ffi && strcmp(profile_ffi, "1") == 0);
    obs_reload_reset(&s_reload_watch);
    s_source_stage_enabled = false;
    s_source_ticket = 0;
    s_last_preflight = (ObsPreflightReport){0};
    obs_publication_reset(&host->publication);
    obs_publish_queue_reset(&host->requests);
    obs_export_catalog_clear(&host->last_publication);
    host->last_transition = (ObsCatalogTransition){0};
    host->runtime = NULL;
    host->space = NULL;
    if (monad_open(&host->runtime, error) != MONAD_OK)
        return false;
    if (monad_object_space_create(monad_thread_handle(host->runtime),
                                  &host->space, error) != MONAD_OK) {
        (void)monad_close(host->runtime, error);
        host->runtime = NULL;
        return false;
    }
    /* Generation zero is never published.  This changes across reopens,
     * invalidating foreign values from earlier engine lifetimes. */
    if (++s_next_generation == 0)
        ++s_next_generation;
    host->generation = s_next_generation;
    host->active = true;
    /* Failure is non-fatal: the legacy synchronous file loader remains a
     * correct fallback when OS worker threads are unavailable. */
    /* Compiler sessions are thread-affine. The source worker creates its own
     * optional *advisory* session, never shares the live monad_t or JIT. */
    const char *preflight = getenv("OBSIDIAN_MONAD_PREFLIGHT");
    const bool enable_preflight = preflight && strcmp(preflight, "1") == 0;
    s_source_stage_enabled = obs_source_stage_open_with_preflight(
        &s_source_stage, enable_preflight ? obs_semantic_preflight_hooks()
                                          : (ObsStagePreflightHooks){0});
    if (!obs_monad_register_bindings(host, error)) {
        monad_error_t close_error = {0};
        (void)obs_monad_close(host, &close_error);
        return false;
    }
    return true;
}

bool obs_monad_close(ObsMonadRuntime *host, monad_error_t *error) {
    if (!host || host != &s_engine || !host->active)
        return true;
    if (!obs_monad_on_owner_thread()) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "Monad host must be closed on its owning thread";
        }
        return false;
    }
    obs_reload_reset(&s_reload_watch);
    /* The object-space must never be destroyed while a Monad call is active. */
    if (!obs_publication_can_retire(&host->publication)) {
        if (error) { error->status = MONAD_ERROR_INVALID_ARGUMENT;
                     error->message = "active script call prevents shutdown"; }
        return false;
    }
    if (s_source_stage_enabled) obs_source_stage_close(&s_source_stage);
    s_source_stage_enabled = false;
    s_source_ticket = 0;
    obs_monad_binding_abort_publication();
    obs_monad_live_editor_close(host);
    obs_monad_unregister_key_dispatch();
    s_camera_policy_available = false;
    /* Do not close the runtime if its object space could not be destroyed. */
    if (host->space) {
        if (monad_object_space_destroy(host->space, error) != MONAD_OK)
            return false;
        host->space = NULL;
    }
    if (monad_close(host->runtime, error) != MONAD_OK)
        return false;
    host->runtime = NULL;
    host->active = false;
    host->generation = 0;
    obs_publication_reset(&host->publication);
    obs_publish_queue_reset(&host->requests);
    obs_export_catalog_clear(&host->last_publication);
    host->last_transition = (ObsCatalogTransition){0};
    return true;
}

static monad_status_t benchmark_scalar(monad_t *runtime, const char *name,
                                       int64_t *value,
                                       monad_error_t *error) {
    monad_i64_function_t function = NULL;
    monad_status_t status = monad_find_i64_function(runtime, name, &function,
                                                    error);
    if (status == MONAD_OK && function) *value = function(0);
    else if (status == MONAD_OK) {
        if (error) {
            error->status = MONAD_ERROR_NOT_FOUND;
            error->message = "benchmark accessor has no native entry point";
        }
        return MONAD_ERROR_NOT_FOUND;
    }
    return status;
}

static bool run_benchmark_declaration(ObsMonadRuntime *host,
                                      monad_error_t *error) {
    static const char *const scene_paths[] = {
        NULL,
        "gltf/Sponza/glTF/Sponza.gltf",
        "gltf/trees.glb",
        "gltf/NodePerformanceTest.glb",
        "gltf/VertexColorTest.glb",
        "gltf/xmp.glb",
        "gltf/TransmissionTest.glb",
        "gltf/Unicode❤♻Test.glb",
        "gltf/UnlitTest.glb",
        "gltf/TextureCoordinateTest.glb",
        "tests/fixtures/async-triangle.gltf",
        "tests/fixtures/async-static-instancing.gltf",
        "tests/fixtures/async-meshlet-growth.gltf",
    };
    static const char *const fields[] = {
        "obsidian-benchmark-scene-id",
        "obsidian-benchmark-width",
        "obsidian-benchmark-height",
        "obsidian-benchmark-warmup",
        "obsidian-benchmark-frames",
        "obsidian-benchmark-cache-mode",
        "obsidian-benchmark-min-meshes",
    };
    int64_t values[sizeof(fields) / sizeof(fields[0])] = {0};
    monad_status_t status = benchmark_scalar(host->runtime, fields[0],
                                              &values[0], error);
    if (status == MONAD_ERROR_NOT_FOUND) return true;
    if (status != MONAD_OK) return false;
    for (size_t i = 1; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (benchmark_scalar(host->runtime, fields[i], &values[i], error) !=
            MONAD_OK)
            return false;
    if (values[0] < 1 ||
        values[0] >= (int64_t)(sizeof(scene_paths) / sizeof(scene_paths[0])) ||
        values[1] < 1 || values[1] > 8192 ||
        values[2] < 1 || values[2] > 8192 ||
        values[3] < 0 || values[3] > 100000 ||
        values[4] < 1 || values[4] > 100000 ||
        values[3] > 100000 - values[4] ||
        values[5] < 0 || values[5] > 1 ||
        values[6] < 1 || values[6] > UINT32_MAX) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "invalid Monad benchmark accessor values";
        }
        return false;
    }
    ObsBenchmarkManifest manifest = {
        .width = (uint32_t)values[1],
        .height = (uint32_t)values[2],
        .warmup_frames = (uint32_t)values[3],
        .measured_frames = (uint32_t)values[4],
        .min_mesh_count = (uint32_t)values[6],
        .cache_mode = values[5] == 0
            ? OBS_BENCHMARK_CACHE_COLD : OBS_BENCHMARK_CACHE_WARM,
    };
    const char *asset_root = getenv("OBSIDIAN_BENCH_ASSET_ROOT");
    if (!asset_root || !asset_root[0]) asset_root = "assets";
    snprintf(manifest.name, sizeof(manifest.name), "scene-%" PRId64 "-%s",
             values[0], values[5] == 0 ? "cold" : "warm");
    const char *scene_root = values[0] <= 9 ? asset_root : ".";
    int path_length = snprintf(manifest.scene, sizeof(manifest.scene), "%s/%s",
                               scene_root, scene_paths[values[0]]);
    if (path_length < 0 || (size_t)path_length >= sizeof(manifest.scene)) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "benchmark asset root exceeds the scene path limit";
        }
        return false;
    }
    if (!obs_benchmark_manifest_stage(&manifest)) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "Monad benchmark manifest was already staged";
        }
        return false;
    }
    return true;
}

bool obs_monad_load_source(ObsMonadRuntime *host, const char *name,
                           const char *source, monad_source_result_t **result,
                           monad_error_t *error) {
    if (result) *result = NULL;
    if (!require_open(host, error) || !name || !source || !result) {
        if (error && (!name || !source || !result)) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "source name and text are required";
        }
        return false;
    }
    /* Verified against embed/README.org Cycles 38-41, 48, 51-52:
     * rejected monad_load_source transactions do not replace the live native
     * image. The engine gate preserves old callbacks on compile failure.
     * Host-side effects outside the native image remain a separate contract. */
    if (!obs_publication_begin_atomic_load(&host->publication)) {
        if (error) { error->status = MONAD_ERROR_INVALID_ARGUMENT;
                     error->message = "active Monad call prevents source loading"; }
        return false;
    }
    /* Host-side effects must not precede native image publication. The
     * staged key/frame callbacks commit only when MonadC reports success. */
    if (!obs_monad_binding_begin_publication()) {
        obs_publication_finish_atomic_load(&host->publication, false);
        if (error) { error->status = MONAD_ERROR_INVALID_ARGUMENT;
                     error->message = "host callback publication already active"; }
        return false;
    }
    obs_benchmark_manifest_begin();
    monad_status_t status = monad_load_source(host->runtime, name, source,
                                             strlen(source), result, error);
    bool published = status == MONAD_OK && *result &&
                     monad_source_result_is_valid(*result) &&
                     monad_source_result_is_published(*result);
    if (published) published = run_benchmark_declaration(host, error);
    obs_publication_finish_atomic_load(&host->publication, published);
    obs_monad_binding_finish_publication(published);
    obs_benchmark_manifest_finish(published);
    if (published) refresh_camera_policy(host->runtime);
    if (published) {
        /* Evidence is copied while the source result is still alive. A failure
         * to capture observability never changes MonadC publication success. */
        ObsNativeCatalog next = {0};
        if (obs_export_catalog_capture(&next, *result,
                                       host->publication.epoch)) {
            size_t unit_length = strlen(name);
            if (unit_length < sizeof(next.source_unit)) {
                memcpy(next.source_unit, name, unit_length + 1);
                next.source_unit_complete = true;
            }
            host->last_transition = obs_catalog_compare(
                &host->last_publication, &next);
            host->last_publication = next;
        } else {
            /* Compiler published successfully, but the host cannot certify
             * that its bounded diagnostic view represents this generation.
             * Never display a stale snapshot as if it described live code. */
            obs_export_catalog_clear(&host->last_publication);
            host->last_transition = (ObsCatalogTransition){0};
        }
        /* Callback retirement/rebinding is performed by the publication
         * journal above, after compiler success and before returning. */
    }
    return published;
}

bool obs_monad_load_file(ObsMonadRuntime *host, const char *path,
                         monad_source_result_t **result, monad_error_t *error) {
    if (result) *result = NULL;
    if (!require_open(host, error) || !path || !result) {
        if (error && (!path || !result)) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "a module path is required";
        }
        return false;
    }
    /* Follow Glemax's existing load-string path rather than inventing a
     * different interpreter.  Long files and short reads are rejected. */
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error) { error->status = MONAD_ERROR_IO;
                     error->message = "cannot open Monad module"; }
        return false;
    }
    struct stat before, after;
    if (fstat(fileno(file), &before) != 0 || !S_ISREG(before.st_mode)) {
        fclose(file); goto io_fail;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); goto io_fail; }
    long length = ftell(file);
    if (length < 0 || (unsigned long)length > 16ul * 1024ul * 1024ul ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); goto io_fail; }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        if (error) { error->status = MONAD_ERROR_OUT_OF_MEMORY;
                     error->message = "cannot allocate module source"; }
        return false;
    }
    size_t read_bytes = fread(buffer, 1, (size_t)length, file);
    bool stable = fstat(fileno(file), &after) == 0 &&
                  before.st_dev == after.st_dev &&
                  before.st_ino == after.st_ino &&
                  before.st_size == after.st_size &&
                  before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                  before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    int close_result = fclose(file);
    if (!stable || read_bytes != (size_t)length || close_result != 0) {
        free(buffer);
        goto io_fail;
    }
    /* The source-loader API consumes a NUL-terminated string. Disallow
     * embedded NULs instead of quietly compiling an invisible prefix. */
    if (memchr(buffer, '\0', (size_t)length)) {
        free(buffer);
        if (error) { error->status = MONAD_ERROR_INVALID_ARGUMENT;
                     error->message = "Monad module contains embedded NUL"; }
        return false;
    }
    buffer[length] = '\0';
    bool loaded = obs_monad_load_source(host, path, buffer, result, error);
    free(buffer);
    return loaded;
io_fail:
    if (error) { error->status = MONAD_ERROR_IO;
                 error->message = "cannot read Monad module"; }
    return false;
}

bool obs_monad_engine_start(void) {
    if (s_engine.active) return obs_monad_on_owner_thread();
    monad_error_t error = {0};
    if (!obs_monad_open(&s_engine, &error)) {
        fprintf(stderr, "Obsidian Monad: %s\n",
                error.message ? error.message : "runtime initialization failed");
        return false;
    }
    return true;
}

void obs_monad_engine_stop(void) {
    monad_error_t error = {0};
    if (!obs_monad_close(&s_engine, &error))
        fprintf(stderr, "Obsidian Monad shutdown: %s\n",
                error.message ? error.message : "unknown error");
}

bool obs_monad_engine_ready(void) { return runtime_ready(&s_engine); }
ObsMonadRuntime *obs_monad_engine(void) { return &s_engine; }

/// Boot publication reporting: one compiler remains authoritative
static bool boot_record_result(const char *path, bool loaded,
                               monad_source_result_t *result,
                               const monad_error_t *error,
                               uint64_t start_ns) {
    const bool published = loaded && result &&
        monad_source_result_is_valid(result) &&
        monad_source_result_is_published(result);
    uint64_t end_ns = obs_reload_clock_ns();
    obs_reload_stats_record(&s_reload_stats, end_ns >= start_ns
        ? (end_ns - start_ns) / 1000u : 0, published);
    if (!published) {
        reload_report(OBS_MONAD_RELOAD_FAILED,
            error && error->message ? error->message : "module was not published");
        const size_t diagnostic_count = result
            ? monad_source_result_diagnostic_count(result) : 0;
        if (diagnostic_count) {
            for (size_t i = 0; i < diagnostic_count; ++i) {
                const char *code =
                    monad_source_result_diagnostic_code(result, i);
                const char *message =
                    monad_source_result_diagnostic_message(result, i);
                fprintf(stderr, "Obsidian Monad boot '%s' [%s]: %s\n", path,
                    code ? code : "diagnostic",
                    message ? message : "no diagnostic message");
            }
        } else {
            fprintf(stderr, "Obsidian Monad boot '%s': %s\n", path,
                error && error->message ? error->message
                                        : "source was not published");
        }
    } else {
        ++s_report.published;
        reload_report(OBS_MONAD_RELOAD_SUCCEEDED, "module published");
        refresh_camera_policy(s_engine.runtime);
    }
    if (result) monad_source_result_destroy(result);
    return published;
}

static bool ensure_live_editor_open(const char **error) {
    return s_engine.live_evaluator ||
           obs_monad_live_editor_open(&s_engine, error);
}

static bool boot_load_staged(const ObsSourceSnapshot *snapshot) {
    ++s_report.attempts;
    if (snapshot->status != OBS_STAGE_OK) {
        const char *message = "source snapshot I/O failed";
        switch (snapshot->status) {
        case OBS_STAGE_TOO_LARGE: message = "module exceeds 16 MiB source limit"; break;
        case OBS_STAGE_INVALID_SOURCE: message = "module contains invalid bytes or path"; break;
        case OBS_STAGE_CHANGED: message = "source changed while reading"; break;
        case OBS_STAGE_OUT_OF_MEMORY: message = "module read out of memory"; break;
        default: break;
        }
        reload_report(OBS_MONAD_RELOAD_FAILED, message);
        obs_reload_stats_record(&s_reload_stats, 0, false);
        return false;
    }
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    uint64_t start_ns = obs_reload_clock_ns();
    bool loaded = obs_monad_load_source(&s_engine, snapshot->path,
                                        snapshot->source, &result, &error);
    if (!boot_record_result(snapshot->path, loaded, result, &error, start_ns))
        return false;
    if (!obs_monad_live_editor_enabled()) return true;
    const char *editor_error = NULL;
    if (!ensure_live_editor_open(&editor_error)) {
        reload_report(OBS_MONAD_RELOAD_FAILED,
                      editor_error ? editor_error
                                   : "live editor image initialization failed");
        return false;
    }
    const char *editor_path = getenv("OBSIDIAN_MONAD_EDITOR_BOOT");
    if (!editor_path || !*editor_path)
        editor_path = "monad/obsidian/editor.mon";
    if (!obs_monad_live_editor_reload_file(&s_engine, editor_path,
                                           &editor_error)) {
        reload_report(OBS_MONAD_RELOAD_FAILED,
                      editor_error ? editor_error
                                   : "live editor module was not loaded");
        fprintf(stderr, "Obsidian live editor boot '%s': %s\n", editor_path,
                editor_error ? editor_error : "live module was not loaded");
        return false;
    }
    return true;
}

void obs_monad_engine_boot_from_environment(void) {
    const char *path = boot_module_path();
    if (!obs_monad_engine_ready() || !obs_monad_on_owner_thread()) return;
    if (watch_enabled() && !s_reload_watch.observed_valid)
        (void)obs_reload_poll(&s_reload_watch, path, obs_reload_clock_ns(), 0, 0);
    monad_error_t error = {0};
    monad_source_result_t *result = NULL;
    ++s_report.attempts;
    uint64_t start_ns = obs_reload_clock_ns();
    bool loaded = obs_monad_load_file(&s_engine, path, &result, &error);
    if (!boot_record_result(path, loaded, result, &error, start_ns))
        return;
    if (!obs_monad_live_editor_enabled()) return;
    const char *editor_error = NULL;
    if (!ensure_live_editor_open(&editor_error)) {
        reload_report(OBS_MONAD_RELOAD_FAILED,
                      editor_error ? editor_error
                                   : "live editor image initialization failed");
        fprintf(stderr, "Obsidian live editor: %s\n",
                editor_error ? editor_error
                             : "live editor image initialization failed");
        return;
    }
    const char *editor_path = getenv("OBSIDIAN_MONAD_EDITOR_BOOT");
    if (!editor_path || !*editor_path)
        editor_path = "monad/obsidian/editor.mon";
    if (!obs_monad_live_editor_reload_file(&s_engine, editor_path,
                                           &editor_error)) {
        reload_report(OBS_MONAD_RELOAD_FAILED,
                      editor_error ? editor_error
                                   : "live editor module was not loaded");
        fprintf(stderr, "Obsidian live editor boot '%s': %s\n", editor_path,
                editor_error ? editor_error : "live module was not loaded");
    }
}
