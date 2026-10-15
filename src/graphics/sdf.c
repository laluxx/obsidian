#define _POSIX_C_SOURCE 200809L
/// sdf — graphics subsystem
#ifdef OBS_SDF_TEST
#include "agent_engine_contract.h"
#else
#include "vulkan_setup.h"
#include "scene.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/// Implementation

extern SdfPrimitive* sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
extern uint32_t active_sdf_count; // Declared in renderer.c/vulkan_setup

/* Write authority is acquired per frame slot only after the caller waited
 * that slot's in-flight fence. Never write to every mapped GPU slot from a
 * game/editor/Monad callback: another slot might still be executing. */
static bool s_sdf_dirty[MAX_FRAMES_IN_FLIGHT] = {true};
void flushSdfSSBO(void) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        s_sdf_dirty[i] = true;
}
bool obs_sdf_sync_frame(uint32_t frame) {
    if (frame >= MAX_FRAMES_IN_FLIGHT ||
        scene->sdf_count > MAX_SDF_PRIMITIVES ||
        scene->sdf_count > scene->sdf_capacity ||
        (scene->sdf_count && !scene->sdfs)) return false;
    if (!sdfSSBOMapped[frame]) return false;
    if (s_sdf_dirty[frame]) {
        bool visible[MAX_SDF_PRIMITIVES];
        if (!scene_tree_collect_sdf_visibility(&scene->tree, scene->sdf_count,
                                               visible)) return false;
        uint32_t gpu_count = 0;
        for (uint32_t i = 0; i < scene->sdf_count; ++i) {
            if (visible[i])
                sdfSSBOMapped[frame][gpu_count++] = scene->sdfs[i];
        }
        active_sdf_count = gpu_count;
        s_sdf_dirty[frame] = false;
    }
    return true;
}

int32_t obs_sdf_add_checked(Scene *s, const char *name, int type,
                            vec3 pos, vec4 size, vec4 color) {
    /* Other scene instances cannot be flushed through the global GPU state. */
    if (s != scene || !name || !*name ||
        s->sdf_count >= MAX_SDF_PRIMITIVES ||
        s->sdf_count > s->sdf_capacity ||
        s->tree.count < 1 ||
        s->tree.count > SCENE_TREE_MAX_NODES - 2 ||
        (s->sdf_count && !s->sdfs)) return -1;
    for (unsigned k = 0; k < 3; ++k)
        if (!isfinite(pos[k])) return -1;
    for (unsigned k = 0; k < 4; ++k)
        if (!isfinite(size[k]) || !isfinite(color[k])) return -1;

    if (s->sdf_count == s->sdf_capacity) {
        uint32_t capacity = s->sdf_capacity ? s->sdf_capacity * 2u : 16u;
        if (capacity > MAX_SDF_PRIMITIVES) capacity = MAX_SDF_PRIMITIVES;
        SdfPrimitive *new_buf = NULL;
        if (posix_memalign((void **)&new_buf, 32,
                           (size_t)capacity * sizeof(*new_buf)) != 0 || !new_buf)
            return -1;
        memset(new_buf, 0, (size_t)capacity * sizeof(*new_buf));
        if (s->sdf_count)
            memcpy(new_buf, s->sdfs, (size_t)s->sdf_count * sizeof(*new_buf));
        free(s->sdfs);
        s->sdfs = new_buf;
        s->sdf_capacity = capacity;
    }
    int32_t index = (int32_t)s->sdf_count;
    SdfPrimitive *prim = &s->sdfs[index];
    memset(prim, 0, sizeof(*prim));
    mat4 transform;
    glm_mat4_identity(transform);
    glm_translate(transform, pos);
    glm_mat4_inv(transform, prim->inverseTransform);
    glm_vec4_copy(size, prim->size);
    glm_vec4_copy(color, prim->color);
    prim->metallic = 0.0f;
    prim->roughness = 0.5f;
    prim->emissiveStrength = 1.0f;
    prim->type = type;
    prim->operation = index == 0 ? SDF_OP_UNION : SDF_OP_SMOOTH_UNION;
    prim->smoothness = 0.5f;

    /* Both tree slots were preflighted: no partial group/material leaf. */
    int32_t node = scene_tree_add_node(&s->tree, name, 0, -1);
    int32_t leaf = scene_tree_add_node(&s->tree, "Material", node, -1);
    if (node < 0 || leaf < 0) return -1; /* unreachable with valid tree */
    s->tree.nodes[node].sdf_index = index;
    s->tree.nodes[leaf].sdf_index = -1;
    ++s->sdf_count;
    scene_bump_sdf_revision(s);
    flushSdfSSBO();
    return index;
}

void sdfs_add(Scene *s, const char *name, int type,
              vec3 pos, vec4 size, vec4 color) {
    (void)obs_sdf_add_checked(s, name, type, pos, size, color);
}
