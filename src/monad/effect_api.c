/// effect_api.c — Language-visible engine effect readiness (not QTT proof)
#include "effect_api.h"
#include "foreign.h"
#include "effect_policy.h"

/// Read-only host state inspection
static monad_status_t mutation_ready(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)args;
    if (argc || !out || !obs_host_owner(context))
        return obs_host_invalid(error, "engine-mutable? needs the engine thread");
    *out = monad_value_bool(space,
        obs_monad_effect_allowed(OBS_EFFECT_WRITE, OBS_LIFETIME_ENGINE, false));
    return MONAD_OK;
}
static monad_status_t gpu_ready(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)args;
    if (argc || !out || !obs_host_owner(context))
        return obs_host_invalid(error, "engine-gpu-mutable? needs the engine thread");
    *out = monad_value_bool(space,
        obs_monad_effect_allowed(OBS_EFFECT_GPU | OBS_EFFECT_WRITE,
                                 OBS_LIFETIME_ENGINE, false));
    return MONAD_OK;
}

/// Procedure registration
bool obs_monad_register_effect_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"engine-mutable?", "Can a script mutate CPU engine state now? Host policy, not a QTT certificate.", 0, mutation_ready, NULL},
        {"engine-gpu-mutable?", "Can a script request GPU-visible changes now? No raw GPU access.", 0, gpu_ready, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
