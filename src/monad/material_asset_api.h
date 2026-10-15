/// material_asset_api.h — independent material assets exposed as foreign values
#ifndef OBSIDIAN_MONAD_MATERIAL_ASSET_API_H
#define OBSIDIAN_MONAD_MATERIAL_ASSET_API_H
#include "runtime.h"

/// Public interface
bool obs_monad_register_material_asset_api(ObsMonadRuntime *host, monad_error_t *error);
#endif
