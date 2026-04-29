#include "vulkan_setup.h"
#include "scene.h"
#include <string.h>

extern SdfPrimitive* sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
extern uint32_t active_sdf_count; // Declared in renderer.c/vulkan_setup

void flushSdfSSBO(void) {
    // Sync the CPU SDF array to the GPU SSBO across all frames
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (sdfSSBOMapped[i] && scene.sdfs) {
            memcpy(sdfSSBOMapped[i], scene.sdfs, scene.sdf_count * sizeof(SdfPrimitive));
        }
    }
    active_sdf_count = scene.sdf_count;
}

void sdfs_add(Scene* s, const char* name, int type, vec3 pos, vec4 size, vec4 color) {
    if (s->sdf_count == s->sdf_capacity) {
        s->sdf_capacity = s->sdf_capacity == 0 ? 16 : s->sdf_capacity * 2;
        s->sdfs = realloc(s->sdfs, s->sdf_capacity * sizeof(SdfPrimitive));
    }

    SdfPrimitive* prim = &s->sdfs[s->sdf_count];
    memset(prim, 0, sizeof(SdfPrimitive));

    // Inverse transform is what the raymarcher needs to put a point into local space!
    mat4 transform;
    glm_mat4_identity(transform);
    glm_translate(transform, pos);
    glm_mat4_inv(transform, prim->inverseTransform);

    glm_vec4_copy(size, prim->size);
    glm_vec4_copy(color, prim->color);
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, prim->emissive);

    prim->metallic = 0.0f;
    prim->roughness = 0.5f;
    prim->emissiveStrength = 1.0f;
    prim->type = type;
    prim->operation = s->sdf_count == 0 ? SDF_OP_UNION : SDF_OP_SMOOTH_UNION;
    prim->smoothness = 0.5f; // IQ's 'k' factor for buttery blending!

    int32_t new_idx = s->sdf_count;
    s->sdf_count++;

    // Add to UI Scene Tree
    int node_idx = scene_tree_add_node(&s->tree, name, 0, -1);
    s->tree.nodes[node_idx].sdf_index = new_idx;
    int mat_node_idx = scene_tree_add_node(&s->tree, "Material", node_idx, -1);
    s->tree.nodes[mat_node_idx].sdf_index = -1; // Explicitly detach from SDF system so it parses as a standard material leaf!

    flushSdfSSBO();
}
