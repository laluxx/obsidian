/// input_api — monad subsystem
#ifndef OBSIDIAN_MONAD_INPUT_API_H
#define OBSIDIAN_MONAD_INPUT_API_H
#include "runtime.h"

/// Public interface
bool obs_monad_register_input_api(ObsMonadRuntime *host, monad_error_t *error);
#endif
