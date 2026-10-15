#ifndef OBSIDIAN_MONAD_EDITOR_API_H
#define OBSIDIAN_MONAD_EDITOR_API_H

#include "runtime.h"

bool obs_monad_register_editor_api(ObsMonadRuntime *host,
                                   monad_error_t *error);

#endif
