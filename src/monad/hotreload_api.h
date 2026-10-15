/// HotReloadAPI — dynamically registered compilation diagnostics
#ifndef OBSIDIAN_MONAD_HOTRELOAD_API_H
#define OBSIDIAN_MONAD_HOTRELOAD_API_H
#include "runtime.h"
/// Public interface
bool obs_monad_register_hotreload_api(ObsMonadRuntime *host, monad_error_t *error);
#endif
