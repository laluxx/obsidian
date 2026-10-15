#include "scene.h"
#include "sdf.h"
#include <assert.h>
#include <stdio.h>

Scene scene;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;

int main(void) {
    SdfPrimitive primitives[2] = {0};
    SdfPrimitive slots[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES] = {0};
    primitives[0].color[0] = 0.25f;
    primitives[1].color[0] = 0.75f;
    scene.sdfs = primitives;
    scene.sdf_count = scene.sdf_capacity = 2;
    scene.tree.count = 4;
    scene.tree.nodes[0] = (SceneNode){
        .first_child = 1, .sdf_index = -1, .visible = true
    };
    scene.tree.nodes[1] = (SceneNode){
        .parent = 0, .first_child = 2, .sdf_index = -1,
        .next_sibling = -1, .visible = true
    };
    scene.tree.nodes[2] = (SceneNode){
        .parent = 1, .first_child = -1, .sdf_index = 0,
        .next_sibling = 3, .visible = true
    };
    scene.tree.nodes[3] = (SceneNode){
        .parent = 1, .first_child = -1, .sdf_index = 1,
        .next_sibling = -1, .visible = true
    };
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        sdfSSBOMapped[slot] = slots[slot];

    flushSdfSSBO();
    assert(obs_sdf_sync_frame(0) && active_sdf_count == 2);
    assert(slots[0][0].color[0] == 0.25f);
    assert(slots[0][1].color[0] == 0.75f);
    assert(obs_sdf_sync_frame(1) && active_sdf_count == 2);

    scene.tree.nodes[1].visible = false;
    flushSdfSSBO();
    assert(obs_sdf_sync_frame(0) && active_sdf_count == 0);
    assert(obs_sdf_sync_frame(1) && active_sdf_count == 0);

    scene.tree.nodes[1].visible = true;
    scene.tree.nodes[2].visible = false;
    flushSdfSSBO();
    assert(obs_sdf_sync_frame(1) && active_sdf_count == 1);
    assert(slots[1][0].color[0] == 0.75f);

    puts("SDF visibility: fenced-slot compaction and ancestor propagation PASS");
    return 0;
}
