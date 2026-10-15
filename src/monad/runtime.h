/// runtime — monad subsystem
#ifndef OBSIDIAN_MONAD_RUNTIME_H
#define OBSIDIAN_MONAD_RUNTIME_H

/* One owned Monad runtime for the legacy engine.  No implicit global VM in
 * graphics/audio callbacks.  Only the engine thread may drive the image. */
#include <stdbool.h>
#include <stdint.h>
#include <monad/monad.h>
#include <monad/embed.h>
#include "hotreload_stats.h"
#include "publication.h"
#include "export_catalog.h"
#include "catalog_transition.h"
#include "publication_queue.h"
#include "publication_metrics.h"
#include "source_stage.h"

/// Public interface
typedef struct MonadLiveEvaluator MonadLiveEvaluator;

typedef enum ObsMonadReloadState {
    OBS_MONAD_RELOAD_NEVER = 0,
    OBS_MONAD_RELOAD_SUCCEEDED,
    OBS_MONAD_RELOAD_FAILED
} ObsMonadReloadState;

typedef struct ObsMonadReloadReport {
    ObsMonadReloadState state;
    uint64_t attempts;
    uint64_t published;
    /* Snapshot owned by the caller; never a dangling monad_error_t string. */
    char message[192];
} ObsMonadReloadReport;

typedef struct ObsMonadRuntime {
    monad_t *runtime;
    monad_object_space_t *space;
    MonadLiveEvaluator *live_evaluator;
    uint64_t generation;
    ObsPublication publication;
    ObsPublishQueue requests; /* bounded owner-thread publication requests */
    ObsNativeCatalog last_publication; /* owned compiler export/QTT snapshot */
    ObsCatalogTransition last_transition; /* observed ABI/proof delta; never a publication authorization */
    bool active;
} ObsMonadRuntime;

/* The application normally uses obs_monad_engine_* below.  These functions
 * are public so alternative embedders can manage an engine-specific runtime. */
bool obs_monad_open(ObsMonadRuntime *host, monad_error_t *error);
bool obs_monad_close(ObsMonadRuntime *host, monad_error_t *error);
bool obs_monad_load_source(ObsMonadRuntime *host, const char *name,
                           const char *source,
                           monad_source_result_t **result,
                           monad_error_t *error);
bool obs_monad_load_file(ObsMonadRuntime *host, const char *path,
                         monad_source_result_t **result,
                         monad_error_t *error);

/* The engine owns a singleton host while its scene/camera globals live.
 * All callbacks use stable, generation-checked host objects. */
bool obs_monad_engine_start(void);
void obs_monad_engine_stop(void);
bool obs_monad_engine_ready(void);
/* Last completed worker preflight, owned report; never a publication proof. */
void obs_monad_engine_preflight_report(ObsPreflightReport *out);
ObsMonadRuntime *obs_monad_engine(void);
/* Runtime owner-thread predicate shared by all foreign-object APIs. */
bool obs_monad_on_owner_thread(void);
/* Mutations during compilation are rejected, not rolled back: publication
 * of the native image does not undo arbitrary C scene/GPU side effects. */
/* Engine-side admission only; never represented as a compiler QTT proof. */
bool obs_monad_effect_allowed(uint32_t effects, ObsLifetime lifetime,
                              bool frame_active);
bool obs_monad_runtime_mutation_allowed(void);
/* Every native-to-Monad call holds a generation lease while executing. */
bool obs_monad_code_enter(ObsCodeLease *lease);
bool obs_monad_code_leave(ObsCodeLease *lease);

/* Try loading the OBSIDIAN_MONAD_BOOT module at initialization.  Leaving this
 * unset does not compile or interpret code on the frame path. */
void obs_monad_engine_boot_from_environment(void);
/* Key/event callbacks enqueue work. The caller's event-loop checkpoint
 * performs compilation after events have been dispatched, never reentrantly
 * from a GLFW callback. This is a synchronous (possibly blocking) safe point. */
void obs_monad_engine_request_reload(void);
void obs_monad_engine_process_pending(void);
void obs_monad_engine_reload_report(ObsMonadReloadReport *out);
/* Sampled CPU time spent loading a module. No claim of GPU-frame latency. */
void obs_monad_engine_reload_stats(ObsReloadStats *out);
/* Separate blocking checkpoint CPU latency from compiler-only load time. */
void obs_monad_engine_publish_metrics(ObsPublishMetrics *out);
/* Read-only host source staging state; never a native compilation candidate. */
bool obs_monad_source_worker_ready(void);
bool obs_monad_source_snapshot_ready(void);
/* Optional directly compiled native Monad camera-policy hook.
 * The checked form reports a rejected/missing/invalid policy invocation; it
 * is required by unattended headless jobs so a no-op cannot count as success.
 * The void compatibility form remains for interactive editor keybindings. */
bool obs_monad_camera_step_checked(void);
void obs_monad_camera_step(void);
/* Monad-owned middle-drag mapping, returned in radians for the C camera host. */
bool obs_monad_middle_orbit_delta(double screen_delta_pixels, bool vertical,
                                  float *radians);

#endif
