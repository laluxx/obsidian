/// effect_api.h — Inspection of the runtime effect admission boundary
#ifndef OBSIDIAN_MONAD_EFFECT_API_H
#define OBSIDIAN_MONAD_EFFECT_API_H
#include "runtime.h"
#include <stdbool.h>

/// Public interface
bool obs_monad_register_effect_api(ObsMonadRuntime *host, monad_error_t *error);
#endif
