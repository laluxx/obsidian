/// frame_api.c — Frame-scoped foreign values and live per-frame Monad hook
#include "frame_api.h"
#include "bindings.h"
#include "foreign.h"
#include "frame_state.h"
#include "dispatch_metrics.h"
#include "window.h"
#include <stdint.h>
#include <stdio.h>

/// Stable ownership and checked borrowing
#define FRAME_TYPE "obsidian.frame"
static monad_value_t *s_frame_callback;
static unsigned s_callback_depth;
static uint64_t s_callback_successes;

static bool frame_valid(monad_value_t *value, ObsMonadRuntime *host,
                        monad_error_t *error) {
    const ObsFrameState *current = obs_frame_state_current();
    if (!current) return obs_host_invalid(error, "frame exists only between beginFrame and endFrame"), false;
    /* The current state pointer is not dereferenced from the foreign value. */
    return obs_host_require(value, host, FRAME_TYPE,
        obs_frame_state_identity(),
        obs_host_epoch(host, obs_frame_state_epoch()), error);
}

static monad_status_t borrow_frame(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)args;
    ObsMonadRuntime *host = context;
    if (argc || !out || !obs_host_owner(host) || !obs_frame_state_current())
        return obs_host_invalid(error, "frame is only available during an active engine frame");
    return obs_host_borrow(space, host, FRAME_TYPE,
        (void *)obs_frame_state_identity(),
        obs_host_epoch(host, obs_frame_state_epoch()), out, error)
        ? MONAD_OK : error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
}

/// Typed scalar accessors
#define FRAME_I64(name, member) \
static monad_status_t name(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    if (argc != 1 || !args || !out || !frame_valid(args[0], ctx, error)) \
        return obs_host_invalid(error, "expected an active frame capability"); \
    return monad_value_make_i64(sp, (int64_t)obs_frame_state_current()->member, out, error); \
}
FRAME_I64(number, number)
FRAME_I64(width, width)
FRAME_I64(height, height)
FRAME_I64(slot, slot)
static monad_status_t delta(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    if (argc != 1 || !args || !out || !frame_valid(args[0], ctx, error))
        return obs_host_invalid(error, "expected an active frame capability");
    return monad_value_make_f64(sp, obs_frame_state_current()->delta_seconds, out, error);
}
static monad_status_t active(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    (void)args;
    if (argc || !out || !obs_host_owner(ctx))
        return obs_host_invalid(error, "frame-active? requires the engine thread");
    *out = monad_value_bool(sp, obs_frame_state_current() != NULL);
    return MONAD_OK;
}

/// Optional instrumentation, never from foreign GPU pointers
static monad_status_t profile_enable(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    if (!obs_monad_runtime_mutation_allowed())
        return obs_host_invalid(error, "engine mutations are forbidden during Monad publication");
    if (argc != 1 || !args || !out || !obs_host_owner(ctx) ||
        monad_value_kind(args[0]) != MONAD_VALUE_BOOL)
        return obs_host_invalid(error, "frame-profile! expects Boolean");
    obs_frame_profile_enable(monad_value_is_true(args[0]));
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
#define FRAME_PROFILE_I64(name, member) \
static monad_status_t name(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, \
    monad_error_t *error) { \
    (void)args; \
    if (argc || !out || !obs_host_owner(ctx)) \
        return obs_host_invalid(error, "frame profile requires the engine thread"); \
    ObsFrameProfile snapshot = {0}; \
    obs_frame_profile_get(&snapshot); \
    return monad_value_make_i64(sp, (int64_t)snapshot.member, out, error); \
}
FRAME_PROFILE_I64(profile_vertices, vertex2d_count)
FRAME_PROFILE_I64(profile_bytes, vertex2d_data_bytes)
FRAME_PROFILE_I64(profile_fence, begin_fence_wait_ns)
FRAME_PROFILE_I64(profile_upload, geometry_upload_ns)
FRAME_PROFILE_I64(profile_record, command_record_ns)
FRAME_PROFILE_I64(profile_submit, queue_submit_ns)
FRAME_PROFILE_I64(profile_present, queue_present_ns)

/// Live game callback: compiled Monad receives the current frame capability
static monad_status_t register_callback(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (argc != 1 || !args || !out || !obs_host_owner(context) ||
        monad_value_kind(args[0]) != MONAD_VALUE_PROCEDURE)
        return obs_host_invalid(error, "on-frame! expects a Monad procedure of one argument");
    if (obs_monad_binding_staging()) {
        if (!obs_monad_binding_stage_frame(args[0]))
            return obs_host_invalid(error, "publication callback journal exhausted");
    } else {
        obs_monad_frame_set_callback(args[0]);
    }
    *out = monad_value_bool(space, true);
    return MONAD_OK;
}
void obs_monad_frame_set_callback(monad_value_t *procedure) {
    if (procedure) monad_value_retain(procedure);
    monad_value_t *previous = s_frame_callback;
    s_frame_callback = procedure;
    s_callback_successes = 0;
    if (previous) monad_value_release(previous);
}
void obs_monad_frame_discard_callback(void) {
    /* Called on the owner thread before image retirement or host teardown. */
    monad_value_t *old = s_frame_callback;
    s_frame_callback = NULL;
    if (old) monad_value_release(old);
}
void obs_monad_frame_tick(void) {
    ObsMonadRuntime *host = obs_monad_engine();
    if (!s_frame_callback || s_callback_depth || !obs_host_owner(host) ||
        !obs_frame_state_current()) return;
    /* Zero sampling clocks and no heap allocation when instrumentation is off.
     * A failed borrow/lease is still counted as a failed dispatch attempt. */
    const bool profiling = obs_dispatch_metrics_enabled();
    const uint64_t started = profiling ? obs_dispatch_clock_ns() : 0;
    monad_value_t *frame_ref = NULL;
    monad_error_t error = {0};
    if (!obs_host_borrow(host->space, host, FRAME_TYPE,
             (void *)obs_frame_state_identity(),
             obs_host_epoch(host, obs_frame_state_epoch()), &frame_ref, &error)) {
        if (profiling) {
            uint64_t end = obs_dispatch_clock_ns();
            obs_dispatch_metrics_record(OBS_DISPATCH_FRAME,
                                        end - started, 0, end - started, false);
        }
        return;
    }
    ObsCodeLease lease = {0};
    if (!obs_monad_code_enter(&lease)) {
        monad_value_release(frame_ref);
        if (profiling) {
            uint64_t end = obs_dispatch_clock_ns();
            obs_dispatch_metrics_record(OBS_DISPATCH_FRAME,
                                        end - started, 0, end - started, false);
        }
        return;
    }
    /* A live callback may replace itself while executing; pin until return. */
    monad_value_t *callback = s_frame_callback;
    monad_value_retain(callback);
    ++s_callback_depth;
    const uint64_t guest_start = profiling ? obs_dispatch_clock_ns() : 0;
    monad_value_t *result = NULL;
    monad_status_t status = monad_value_call(host->space, callback,
        1, &frame_ref, &result, &error);
    const uint64_t guest_end = profiling ? obs_dispatch_clock_ns() : 0;
    --s_callback_depth;
    monad_value_release(result);
    monad_value_release(frame_ref);
    /* Check identity before releasing the pin; never inspect a retired value. */
    const bool still_current = s_frame_callback == callback;
    monad_value_release(callback);
    (void)obs_monad_code_leave(&lease);
    if (profiling) {
        uint64_t end = obs_dispatch_clock_ns();
        obs_dispatch_metrics_record(OBS_DISPATCH_FRAME,
                                    guest_start - started,
                                    guest_end - guest_start,
                                    end - started, status == MONAD_OK);
    }
    if (status == MONAD_OK) ++s_callback_successes;
    if (status != MONAD_OK) {
        fprintf(stderr, "Obsidian Monad frame: %s\n",
                error.message ? error.message : "callback failed");
        /* Fail closed; a continuously failing callback must not spam/freeze frames. */
        /* A failing old callback must not discard its freshly rebound successor. */
        if (still_current) obs_monad_frame_discard_callback();
    }
}

/// Observable live callback execution for integration harnesses
static monad_status_t callback_calls(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    (void)args;
    if (argc || !out || !obs_host_owner(ctx))
        return obs_host_invalid(error, "frame callback statistics require owner thread");
    return monad_value_make_i64(sp, (int64_t)s_callback_successes, out, error);
}

/// Host procedure catalog
bool obs_monad_register_frame_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"frame", "Borrow the active CPU frame, expiring at endFrame.", 0, borrow_frame, NULL},
        {"obs-current-frame", "Compatibility spelling for active frame.", 0, borrow_frame, NULL},
        {"frame-active?", "True between beginFrame and endFrame.", 0, active, NULL},
        {"frame-number", "Monotone engine CPU frame ordinal.", 1, number, NULL},
        {"frame-delta", "Frame time in seconds, sanitized and nonnegative.", 1, delta, NULL},
        {"frame-width", "Framebuffer width at frame start.", 1, width, NULL},
        {"frame-height", "Framebuffer height at frame start.", 1, height, NULL},
        {"frame-slot", "Renderer CPU frame slot (not a Vulkan handle).", 1, slot, NULL},
        {"on-frame!", "Register a compiled Monad function receiving a frame.", 1, register_callback, NULL},
        {"frame-callback-calls", "Successful callbacks since last registration.", 0, callback_calls, NULL},
        {"frame-profile!", "Enable or disable optional CPU frame instrumentation.", 1, profile_enable, NULL},
        {"frame-last-vertices", "Last profiled vertex2d count (when enabled).", 0, profile_vertices, NULL},
        {"frame-last-bytes", "Last profiled vertex data bytes (when enabled).", 0, profile_bytes, NULL},
        {"frame-last-fence-ns", "Last frame's CPU fence wait in ns.", 0, profile_fence, NULL},
        {"frame-last-upload-ns", "Last frame's CPU geometry upload in ns.", 0, profile_upload, NULL},
        {"frame-last-record-ns", "Last frame's CPU command-record time in ns.", 0, profile_record, NULL},
        {"frame-last-submit-ns", "Last frame's CPU queue-submit time in ns.", 0, profile_submit, NULL},
        {"frame-last-present-ns", "Last frame's CPU queue-present call time in ns.", 0, profile_present, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
