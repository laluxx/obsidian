/// input_api.c — Read-only frame input for embedded game logic
/// Implementation
#include "input_api.h"
#include "bindings.h"
#include "foreign.h"
#include "input.h"
#include "window.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

static monad_status_t reject(monad_error_t *e, const char *text) {
    if (e) { e->status = MONAD_ERROR_INVALID_ARGUMENT; e->message = text; }
    return MONAD_ERROR_INVALID_ARGUMENT;
}
static bool valid(ObsMonadRuntime *host) {
    return host && host->active && host->space && obs_monad_on_owner_thread();
}

static monad_status_t key_down(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **result, void *ctx,
    monad_error_t *e) {
    int64_t code = -1;
    if (!valid(ctx) || argc != 1 || !args || !result ||
        monad_value_as_i64(args[0], &code, e) != MONAD_OK ||
        code < GLFW_KEY_SPACE || code > GLFW_KEY_LAST)
        return reject(e, "expected a GLFW key code within the supported range");
    *result = monad_value_bool(sp, isKeyDown((int)code) != 0);
    return MONAD_OK;
}

static monad_status_t mouse_down(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **result, void *ctx,
    monad_error_t *e) {
    int64_t button = -1;
    if (!valid(ctx) || argc != 1 || !args || !result ||
        monad_value_as_i64(args[0], &button, e) != MONAD_OK ||
        button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return reject(e, "expected a valid GLFW mouse button");
    *result = monad_value_bool(sp, isMouseButtonDown((int)button) != 0);
    return MONAD_OK;
}

static monad_status_t cursor_axis(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **result, void *ctx,
    monad_error_t *e) {
    (void)args;
    if (!valid(ctx) || argc != 0 || !result)
        return reject(e, "cursor position requires the engine thread");
    double x = 0, y = 0;
    getCursorPosition(&x, &y);
    if (!isfinite(x) || !isfinite(y)) return reject(e, "cursor position is invalid");
    return monad_value_make_f64(sp, x, result, e);
}
static monad_status_t cursor_y(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **result, void *ctx,
    monad_error_t *e) {
    (void)args;
    if (!valid(ctx) || argc != 0 || !result)
        return reject(e, "cursor position requires the engine thread");
    double x = 0, y = 0;
    getCursorPosition(&x, &y);
    if (!isfinite(x) || !isfinite(y)) return reject(e, "cursor position is invalid");
    return monad_value_make_f64(sp, y, result, e);
}
static monad_status_t frame_delta_ms(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **result, void *ctx,
    monad_error_t *e) {
    (void)args;
    if (!valid(ctx) || argc != 0 || !result)
        return reject(e, "frame delta requires the engine thread");
    /* Read only, bounded: no implicit physics mutation or event polling. */
    double ms = (double)delta_time * 1000.0;
    if (!isfinite(ms) || ms < 0 || ms > 1000000)
        return reject(e, "frame delta unavailable");
    return monad_value_make_i64(sp, (int64_t)ms, result, e);
}

bool obs_monad_register_input_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-key-down?", "Read sampled key state.", 1, key_down, NULL},
        {"key-down?", "Read sampled key state.", 1, key_down, NULL},
        {"obs-mouse-down?", "Read sampled mouse state.", 1, mouse_down, NULL},
        {"mouse-down?", "Read sampled mouse state.", 1, mouse_down, NULL},
        {"obs-cursor-x", "Cursor x coordinate.", 0, cursor_axis, NULL},
        {"cursor-x", "Cursor x coordinate.", 0, cursor_axis, NULL},
        {"obs-cursor-y", "Cursor y coordinate.", 0, cursor_y, NULL},
        {"cursor-y", "Cursor y coordinate.", 0, cursor_y, NULL},
        {"obs-frame-delta-ms", "Last frame delta in milliseconds.", 0, frame_delta_ms, NULL},
        {"frame-ms", "Last frame delta in milliseconds.", 0, frame_delta_ms, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
