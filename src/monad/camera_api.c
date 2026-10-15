/// camera_api.c — Atomic, bounded camera policies for embedded Monad
/// Implementation
#include "camera_api.h"
#include "bindings.h"
#include "foreign.h"
#include "camera.h"
#include <stddef.h>
#include <math.h>
#include <stdint.h>

#define CAMERA_TYPE "obsidian.camera"

static monad_status_t reject(monad_error_t *e, const char *message) {
    if (e) { e->status = MONAD_ERROR_INVALID_ARGUMENT; e->message = message; }
    return MONAD_ERROR_INVALID_ARGUMENT;
}

static bool borrowed_camera(monad_value_t *v, ObsMonadRuntime *host,
                            monad_error_t *e) {
    return obs_host_require(v, host, CAMERA_TYPE, &camera,
                            host ? host->generation : 0, e);
}

static monad_status_t camera_yaw(monad_object_space_t *space, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *e) {
    if (argc != 1 || !args || !out || !borrowed_camera(args[0], ctx, e))
        return reject(e, "expected a live camera handle");
    return monad_value_make_f64(space, camera.yaw, out, e);
}
static monad_status_t camera_pitch(monad_object_space_t *space, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *e) {
    if (argc != 1 || !args || !out || !borrowed_camera(args[0], ctx, e))
        return reject(e, "expected a live camera handle");
    return monad_value_make_f64(space, camera.pitch, out, e);
}
static monad_status_t camera_set_angles(monad_object_space_t *space, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *e) {
    if (!obs_monad_runtime_mutation_allowed())
        return reject(e, "engine mutations are forbidden during Monad publication");
    double yaw, pitch;
    if (argc != 3 || !args || !out || !borrowed_camera(args[0], ctx, e) ||
        monad_value_as_f64(args[1], &yaw, e) != MONAD_OK ||
        monad_value_as_f64(args[2], &pitch, e) != MONAD_OK ||
        !isfinite(yaw) || !isfinite(pitch) || fabs(yaw) > 3600.0 ||
        pitch < -89.0 || pitch > 89.0)
        return reject(e, "camera yaw/pitch outside finite safe bounds");
    /* Both checks precede either mutation: no partially applied command. */
    camera.yaw = (float)yaw;
    camera.pitch = (float)pitch;
    camera.use_look_at = false;
    camera_update(&camera);
    *out = monad_value_nil(space);
    return MONAD_OK;
}
static monad_status_t camera_translate(monad_object_space_t *space, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *e) {
    if (!obs_monad_runtime_mutation_allowed())
        return reject(e, "engine mutations are forbidden during Monad publication");
    if (argc != 4 || !args || !out || !borrowed_camera(args[0], ctx, e))
        return reject(e, "expected live camera handle and three Float offsets");
    double delta[3], next[3];
    for (size_t i = 0; i < 3; ++i) {
        if (monad_value_as_f64(args[i+1], &delta[i], e) != MONAD_OK ||
            !isfinite(delta[i]) || fabs(delta[i]) > 1.0e6)
            return reject(e, "camera translation must be finite and bounded");
        next[i] = (double)camera.position[i] + delta[i];
        if (!isfinite(next[i]) || fabs(next[i]) > 1.0e8)
            return reject(e, "translated camera position exceeds world limits");
    }
    for (size_t i = 0; i < 3; ++i) camera.position[i] = (float)next[i];
    camera_update(&camera);
    *out = monad_value_nil(space);
    return MONAD_OK;
}

bool obs_monad_register_camera_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-camera-yaw", "Camera horizontal angle, in degrees.", 1, camera_yaw, NULL},
        {"camera-yaw", "Camera horizontal angle, in degrees.", 1, camera_yaw, NULL},
        {"obs-camera-pitch", "Camera vertical angle, in degrees.", 1, camera_pitch, NULL},
        {"camera-pitch", "Camera vertical angle, in degrees.", 1, camera_pitch, NULL},
        {"obs-camera-set-angles", "Set yaw and pitch as one validated operation.", 3, camera_set_angles, NULL},
        {"camera-angles!", "Set yaw/pitch atomically and validate bounds.", 3, camera_set_angles, NULL},
        {"obs-camera-translate", "Move camera atomically by bounded offsets.", 4, camera_translate, NULL},
        {"camera-move!", "Move camera by three bounded offsets.", 4, camera_translate, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
