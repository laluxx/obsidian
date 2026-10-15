/// HotReloadAPI — Monad observability over its own embedded image
#include "foreign.h"
#include "runtime.h"
#include "dispatch_metrics.h"
#include <stdint.h>

/// Live compilation metrics
/* Distinct callbacks instead of stateful thread-local selector contexts:
 * object-space definitions may outlive a published Monad source image. */
#define OBS_METRIC_CALLBACK(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc != 0 || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "reload metrics require owner thread"); \
    ObsReloadStats stats = {0}; \
    obs_monad_engine_reload_stats(&stats); \
    return monad_value_make_i64(sp, (int64_t)(expression), out, error); \
}
OBS_METRIC_CALLBACK(reload_attempts, stats.attempts)
OBS_METRIC_CALLBACK(reload_published, stats.published)
OBS_METRIC_CALLBACK(reload_failed, stats.failed)
OBS_METRIC_CALLBACK(reload_last_us, stats.last_us)
OBS_METRIC_CALLBACK(reload_p95_us, obs_reload_stats_percentile(&stats, 95))
OBS_METRIC_CALLBACK(reload_p99_us, obs_reload_stats_percentile(&stats, 99))

/// Live-code publication observability
/* These values come from the actual legacy host gate, not a staged MonadC
 * transaction. A clean type contract needs a compiler-authored descriptor. */
#define OBS_PUBLICATION_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "publication status requires owner thread"); \
    const ObsPublication *state = &((ObsMonadRuntime *)ctx)->publication; \
    return monad_value_make_i64(sp, (int64_t)(expression), out, error); \
}
OBS_PUBLICATION_METRIC(publication_epoch, state->epoch)
OBS_PUBLICATION_METRIC(publication_phase, state->phase)
OBS_PUBLICATION_METRIC(publication_calls, state->active_calls)
OBS_PUBLICATION_METRIC(publication_attempts, state->attempts)

/// Compiler-owned native generation accounting
static monad_status_t retained_native_images(monad_object_space_t *sp,
    size_t argc, monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    (void)args;
    ObsMonadRuntime *host = ctx;
    if (argc || !out || !obs_host_owner(host))
        return obs_host_invalid(error, "native image inspection requires engine thread");
    size_t count = monad_live_native_image_count(host->runtime);
    if (count > INT64_MAX)
        return obs_host_invalid(error, "native image count exceeds Int64 range");
    return monad_value_make_i64(sp, (int64_t)count, out, error);
}

/// Last published source certificates (owned snapshot, not live JIT pointers)
#define OBS_CATALOG_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "publication catalog requires owner thread"); \
    const ObsNativeCatalog *catalog = &((ObsMonadRuntime *)ctx)->last_publication; \
    if ((expression) > INT64_MAX) \
        return obs_host_invalid(error, "publication metric overflows Int64"); \
    return monad_value_make_i64(sp, (int64_t)(expression), out, error); \
}
OBS_CATALOG_METRIC(native_export_count, catalog->export_count)
OBS_CATALOG_METRIC(native_proof_count, catalog->resource_verified)
OBS_CATALOG_METRIC(native_inferred_count, catalog->qtt_inferred_only)
OBS_CATALOG_METRIC(native_catalog_epoch, catalog->publication_epoch)

/// Structural native-export changes (observations, never certified migration)
#define OBS_TRANSITION_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "native ABI transition requires owner thread"); \
    const ObsCatalogTransition *diff = &((ObsMonadRuntime *)ctx)->last_transition; \
    const uint64_t value = (uint64_t)(expression); \
    if (value > INT64_MAX) \
        return obs_host_invalid(error, "native ABI count exceeds Int64"); \
    return monad_value_make_i64(sp, (int64_t)value, out, error); \
}
OBS_TRANSITION_METRIC(native_abi_changes, diff->abi_changed)
OBS_TRANSITION_METRIC(native_abi_unknown, diff->abi_unknown)
OBS_TRANSITION_METRIC(native_export_removals, diff->removed)
OBS_TRANSITION_METRIC(native_proof_drift, diff->proof_changed)
OBS_TRANSITION_METRIC(native_cert_fingerprint_removed, diff->proof_removed)
OBS_TRANSITION_METRIC(native_transition_complete, diff->complete && diff->has_previous)

/// Owner-thread request scheduler observability (not candidate compilation)
#define OBS_QUEUE_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "queue metrics require owner thread"); \
    const ObsPublishQueue *queue = &((ObsMonadRuntime *)ctx)->requests; \
    const uint64_t amount = (uint64_t)(expression); \
    if (amount > INT64_MAX) \
        return obs_host_invalid(error, "queue metric overflows Int64"); \
    return monad_value_make_i64(sp, (int64_t)amount, out, error); \
}
OBS_QUEUE_METRIC(publish_queued, obs_publish_queue_pending(queue))
OBS_QUEUE_METRIC(publish_completed, queue->completed)
OBS_QUEUE_METRIC(publish_coalesced, queue->coalesced)
OBS_QUEUE_METRIC(publish_failed, queue->failed)
OBS_QUEUE_METRIC(publish_cancelled, queue->cancelled)

/// Source I/O staging (not compiler publication or a QTT proof)
#define OBS_STAGE_BOOL(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "source staging requires owner thread"); \
    return monad_value_make_i64(sp, (expression) ? 1 : 0, out, error); \
}
OBS_STAGE_BOOL(source_worker_active, obs_monad_source_worker_ready())
OBS_STAGE_BOOL(source_snapshot_ready, obs_monad_source_snapshot_ready())

/// Advisory worker compiler diagnostics (not live QTT or native publication)
#define OBS_PREFLIGHT_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "preflight inspection requires owner thread"); \
    ObsPreflightReport preflight = {0}; \
    obs_monad_engine_preflight_report(&preflight); \
    const uint64_t value = (uint64_t)(expression); \
    if (value > INT64_MAX) \
        return obs_host_invalid(error, "preflight counter exceeds Int64"); \
    return monad_value_make_i64(sp, (int64_t)value, out, error); \
}
OBS_PREFLIGHT_METRIC(preflight_status, preflight.status)
OBS_PREFLIGHT_METRIC(preflight_duration, preflight.elapsed_us)
OBS_PREFLIGHT_METRIC(preflight_diagnostics, preflight.diagnostic_count)

/// Real CPU checkpoint latency, separate from GPU frame or compiler-only timing
#define OBS_CHECKPOINT_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "publication timings require owner thread"); \
    ObsPublishMetrics timings = {0}; \
    obs_monad_engine_publish_metrics(&timings); \
    const uint64_t value = (uint64_t)(expression); \
    if (value > INT64_MAX) \
        return obs_host_invalid(error, "publication timing exceeds Int64"); \
    return monad_value_make_i64(sp, (int64_t)value, out, error); \
}
OBS_CHECKPOINT_METRIC(publish_last_us, timings.all.last_duration_us)
OBS_CHECKPOINT_METRIC(publish_p95_us, obs_publish_latency_percentile(&timings.all, 95, false))
OBS_CHECKPOINT_METRIC(publish_p99_us, obs_publish_latency_percentile(&timings.all, 99, false))
OBS_CHECKPOINT_METRIC(publish_wait_p95_us, obs_publish_latency_percentile(&timings.all, 95, true))
OBS_CHECKPOINT_METRIC(publish_stall_count, timings.all.over_budget)
OBS_CHECKPOINT_METRIC(publish_budget_us, timings.budget_us)
OBS_CHECKPOINT_METRIC(publish_repl_p95_us, obs_publish_latency_percentile(&timings.lane[OBS_PUBLISH_REPL], 95, false))
OBS_CHECKPOINT_METRIC(publish_boot_p95_us, obs_publish_latency_percentile(&timings.lane[OBS_PUBLISH_BOOT], 95, false))

/// Allocation-free CPU foreign-dispatch diagnostics
/* Counters are read from owner-thread-only histogram snapshots. Percentiles
 * are upper bounds of log2-nanosecond buckets; never GPU/JIT timings. */
#define OBS_DISPATCH_METRIC(fn, expression) \
static monad_status_t fn(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "dispatch profiler requires engine thread"); \
    ObsDispatchMetrics snapshot = {0}; \
    obs_dispatch_metrics_snapshot(&snapshot); \
    const uint64_t amount = (uint64_t)(expression); \
    if (amount > INT64_MAX) \
        return obs_host_invalid(error, "dispatch metric exceeds Int64"); \
    return monad_value_make_i64(sp, (int64_t)amount, out, error); \
}
OBS_DISPATCH_METRIC(dispatch_profile_enabled, snapshot.enabled)
OBS_DISPATCH_METRIC(dispatch_frame_calls, snapshot.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_TOTAL].samples)
OBS_DISPATCH_METRIC(dispatch_frame_failures, snapshot.lanes[OBS_DISPATCH_FRAME].failures)
OBS_DISPATCH_METRIC(dispatch_frame_p95_ns, obs_dispatch_histogram_percentile_ns(
    &snapshot.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_TOTAL], 95))
OBS_DISPATCH_METRIC(dispatch_frame_guest_p95_ns, obs_dispatch_histogram_percentile_ns(
    &snapshot.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_EXECUTE], 95))
OBS_DISPATCH_METRIC(dispatch_camera_calls, snapshot.lanes[OBS_DISPATCH_CAMERA].phases[OBS_DISPATCH_TOTAL].samples)
OBS_DISPATCH_METRIC(dispatch_camera_failures, snapshot.lanes[OBS_DISPATCH_CAMERA].failures)
OBS_DISPATCH_METRIC(dispatch_camera_p95_ns, obs_dispatch_histogram_percentile_ns(
    &snapshot.lanes[OBS_DISPATCH_CAMERA].phases[OBS_DISPATCH_TOTAL], 95))
OBS_DISPATCH_METRIC(dispatch_camera_guest_p95_ns, obs_dispatch_histogram_percentile_ns(
    &snapshot.lanes[OBS_DISPATCH_CAMERA].phases[OBS_DISPATCH_EXECUTE], 95))

/// Foreign registration
bool obs_monad_register_hotreload_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"dispatch-profile?", "Whether opt-in CPU foreign dispatch profiling is active.", 0, dispatch_profile_enabled, NULL},
        {"dispatch-frame-calls", "Recorded frame callbacks attempted, including errors.", 0, dispatch_frame_calls, NULL},
        {"dispatch-frame-failures", "Frame callback failures and declined borrows/leases.", 0, dispatch_frame_failures, NULL},
        {"dispatch-frame-p95-ns", "Upper bound of frame dispatch CPU total p95 in nanoseconds.", 0, dispatch_frame_p95_ns, NULL},
        {"dispatch-frame-guest-p95-ns", "Upper bound of Monad invocation CPU p95, excludes setup.", 0, dispatch_frame_guest_p95_ns, NULL},
        {"dispatch-camera-calls", "Pinned native camera calls attempted, including failures.", 0, dispatch_camera_calls, NULL},
        {"dispatch-camera-failures", "Failed native pin/lookup or invalid camera mutation.", 0, dispatch_camera_failures, NULL},
        {"dispatch-camera-p95-ns", "Upper bound of pinned camera policy CPU total p95.", 0, dispatch_camera_p95_ns, NULL},
        {"dispatch-camera-guest-p95-ns", "Upper bound of camera guest policy CPU p95.", 0, dispatch_camera_guest_p95_ns, NULL},
        {"preflight-status", "Advisory worker compiler status: 0 off, 1 accepted, 2 diagnostics, 3 unavailable.", 0, preflight_status, NULL},
        {"preflight-us", "Last isolated semantic preflight CPU wall duration in microseconds.", 0, preflight_duration, NULL},
        {"preflight-diagnostics", "Number of isolated compiler diagnostics; not a publication veto.", 0, preflight_diagnostics, NULL},
        {"publish-last-us", "Last owner-thread publication checkpoint CPU duration (microseconds).", 0, publish_last_us, NULL},
        {"publish-p95-us", "Rolling p95 synchronous checkpoint CPU cost, not GPU frame time.", 0, publish_p95_us, NULL},
        {"publish-p99-us", "Rolling p99 synchronous checkpoint CPU cost, not GPU frame time.", 0, publish_p99_us, NULL},
        {"publish-wait-p95-us", "Rolling p95 admission-to-dispatch wait time (microseconds).", 0, publish_wait_p95_us, NULL},
        {"publish-stalls", "Number of publication checkpoints exceeding the configured CPU budget.", 0, publish_stall_count, NULL},
        {"publish-budget-us", "Configured owner-thread checkpoint duration budget, microseconds.", 0, publish_budget_us, NULL},
        {"publish-repl-p95-us", "Rolling p95 REPL request checkpoint CPU duration (microseconds).", 0, publish_repl_p95_us, NULL},
        {"publish-boot-p95-us", "Rolling p95 module reload checkpoint CPU duration (microseconds).", 0, publish_boot_p95_us, NULL},
        {"source-worker?", "1 when source worker runs; optional isolated semantic preflight does not publish native code.", 0, source_worker_active, NULL},
        {"source-staged?", "1 when immutable changed-module bytes are waiting for an owner-thread load.", 0, source_snapshot_ready, NULL},
        {"publish-queued", "Queued host source requests; not compiler candidates.", 0, publish_queued, NULL},
        {"publish-completed", "Host publication requests attempted since engine open.", 0, publish_completed, NULL},
        {"publish-coalesced", "Redundant module reloads collapsed before compilation.", 0, publish_coalesced, NULL},
        {"publish-failed", "Host requests rejected or failed (not an image count).", 0, publish_failed, NULL},
        {"publish-cancelled", "Pending work explicitly cancelled before starting.", 0, publish_cancelled, NULL},
        {"native-abi-unknown", "Count of exports whose full C ABI was not available to compare.", 0, native_abi_unknown, NULL},
        {"native-abi-changes", "Observed structural ABI changes since prior publication; not a precommit proof.", 0, native_abi_changes, NULL},
        {"native-export-removals", "Names absent from latest SOURCE RESULT (not proved removed from live image).", 0, native_export_removals, NULL},
        {"native-proof-drift", "Changed compiler-issued QTT fingerprints.", 0, native_proof_drift, NULL},
        {"native-cert-fingerprint-removed", "Previous nonzero QTT fingerprint now absent.", 0, native_cert_fingerprint_removed, NULL},
        {"native-transition-complete?", "1 when same-source result ABI samples are complete; not full-image proof.", 0, native_transition_complete, NULL},
        {"native-export-count", "Exports in most recently published Monad source, not all retained images.", 0, native_export_count, NULL},
        {"native-resource-proof-count", "Resource-verified QTT definitions from most recent publication.", 0, native_proof_count, NULL},
        {"native-inferred-count", "Definitions with only inferred or incomplete resource QTT evidence.", 0, native_inferred_count, NULL},
        {"native-catalog-epoch", "Engine epoch at which the latest source-result snapshot was copied.", 0, native_catalog_epoch, NULL},
        {"native-image-count", "Compiler-retained native generations (includes pinned readers).", 0, retained_native_images, NULL},
        {"reload-generation", "Published legacy image generation (not foreign-object age).", 0, publication_epoch, NULL},
        {"reload-phase", "0=empty,1=ready,2=loading,3=poisoned.", 0, publication_phase, NULL},
        {"reload-active-calls", "Number of live owner-thread Monad callback leases.", 0, publication_calls, NULL},
        {"reload-load-count", "Legacy image load attempts since host opened.", 0, publication_attempts, NULL},
        {"obs-reload-attempts", "Attempts to publish a Monad boot module.", 0, reload_attempts, NULL},
        {"obs-reload-published", "Successful Monad image publications.", 0, reload_published, NULL},
        {"obs-reload-failed", "Failed loads; previous code is not assumed safe.", 0, reload_failed, NULL},
        {"obs-reload-last-us", "Last synchronous module load CPU duration, microseconds.", 0, reload_last_us, NULL},
        {"obs-reload-p95-us", "Rolling p95 module load CPU duration, microseconds.", 0, reload_p95_us, NULL},
        {"obs-reload-p99-us", "Rolling p99 module load CPU duration, microseconds.", 0, reload_p99_us, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
