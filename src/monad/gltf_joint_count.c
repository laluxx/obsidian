/// gltf_joint_count — glTF skin metadata query shared by Monad and native tests
#include "asset_api.h"
#include "scene.h"
#include <limits.h>

bool obs_monad_gltf_joint_count(const OmdlSceneGraph *graph, int64_t *out) {
    if (!out) return false;
    if (!graph) {
        *out = 0;
        return true;
    }
    if (graph->skin_count && !graph->skins) return false;

    int64_t total = 0;
    for (uint32_t i = 0; i < graph->skin_count; ++i) {
        const uint32_t count = graph->skins[i].joints_count;
        if ((uint64_t)count > (uint64_t)INT64_MAX - (uint64_t)total)
            return false;
        total += (int64_t)count;
    }
    *out = total;
    return true;
}
