/// asset_api — monad subsystem
#ifndef OBSIDIAN_MONAD_ASSET_API_H
#define OBSIDIAN_MONAD_ASSET_API_H
#include "runtime.h"
#include <stdint.h>

typedef struct OmdlSceneGraph OmdlSceneGraph;

/// Public interface
bool obs_monad_register_asset_api(ObsMonadRuntime *host, monad_error_t *error);
/* Sum joint entries across an instance graph's skins; a missing graph is a
 * valid no-skin result. False means inconsistent metadata or Int overflow. */
bool obs_monad_gltf_joint_count(const OmdlSceneGraph *graph, int64_t *out);
#endif
