#include "scene.h"
#include "asset_api.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    int64_t count = -1;
    assert(obs_monad_gltf_joint_count(NULL, &count));
    assert(count == 0);

    OmdlSceneGraph graph = {0};
    assert(obs_monad_gltf_joint_count(&graph, &count));
    assert(count == 0);

    OmdlSkin skins[] = {
        {.joints_count = 3},
        {.joints_count = 0},
        {.joints_count = 5},
    };
    graph.skin_count = sizeof(skins) / sizeof(skins[0]);
    graph.skins = skins;
    assert(obs_monad_gltf_joint_count(&graph, &count));
    assert(count == 8);

    graph.skins = NULL;
    count = -7;
    assert(!obs_monad_gltf_joint_count(&graph, &count));
    assert(count == -7);
    assert(!obs_monad_gltf_joint_count(&graph, NULL));

    puts("gltf_joint_count: no-skin, aggregate, malformed and output-atomic cases PASS");
    return 0;
}
