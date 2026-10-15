/// Agent C ABI test fixture: only external ownership services are replaced
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { void *device; } VulkanContext;
extern VulkanContext context;
#define VK_SUCCESS 0
int vkDeviceWaitIdle(void *device);
void cleanup(VulkanContext *ctx);
bool obs_headless_init(uint32_t width, uint32_t height, bool readback_enabled);
bool obs_headless_render_frame(void (*draw)(void *), void *userdata);
bool obs_headless_render_raw(const char *path, void (*draw)(void *), void *userdata);
#ifndef OBS_MONAD_SCULPT_TEST
bool obs_monad_engine_start(void);
void obs_monad_engine_stop(void);
bool obs_monad_engine_ready(void);
bool obs_monad_camera_step_checked(void);
typedef struct { const char *message; } monad_error_t;
typedef struct { int published; } monad_source_result_t;
typedef struct { int n; } ObsMonadRuntime;
ObsMonadRuntime *obs_monad_engine(void);
bool obs_monad_load_file(ObsMonadRuntime *host, const char *path,
                          monad_source_result_t **result, monad_error_t *error);
size_t monad_source_result_diagnostic_count(const monad_source_result_t *result);
const char *monad_source_result_diagnostic_message(const monad_source_result_t *result,
                                                    size_t index);
void monad_source_result_destroy(monad_source_result_t *result);
bool obs_monad_load_source(ObsMonadRuntime *host, const char *name,
                            const char *source, monad_source_result_t **result,
                            monad_error_t *error);
#endif /* !OBS_MONAD_SCULPT_TEST */

/* Cycle 49: compile the actual SDF append and agent editing sources against
 * their external engine/scene owners, without fabricating Vulkan rendering. */
#define MAX_SDF_PRIMITIVES 1024u
#define MAX_FRAMES_IN_FLIGHT 2u
#define SCENE_TREE_MAX_NODES 4096
#define SDF_TYPE_SPHERE 0
#define SDF_TYPE_BOX 1
#define SDF_TYPE_TERRAIN 2
#define SDF_OP_UNION 0
#define SDF_OP_SMOOTH_UNION 1
typedef float vec3[3];
typedef float vec4[4];
typedef float mat4[4][4];
typedef struct {
    mat4 inverseTransform;
    vec4 size, color;
    vec3 emissive;
    float metallic, roughness, emissiveStrength, smoothness;
    int type, operation, _pad1;
} SdfPrimitive;
typedef struct { int32_t sdf_index; } SceneNode;
typedef struct {
    int32_t count;
    SceneNode nodes[SCENE_TREE_MAX_NODES];
} SceneTree;
typedef struct {
    uint64_t sdf_lifetime_epoch, sdf_revision;
    SdfPrimitive *sdfs;
    uint32_t sdf_count, sdf_capacity;
    SceneTree tree;
} Scene;
extern Scene *scene;
void scene_bump_sdf_revision(Scene *s);
int32_t scene_tree_add_node(SceneTree *tree, const char *name,
                            int32_t parent, int32_t mesh_index);
static inline bool scene_tree_collect_sdf_visibility(
    const SceneTree *tree, uint32_t sdf_count,
    bool visible[MAX_SDF_PRIMITIVES]) {
    if (!tree || !visible || sdf_count > MAX_SDF_PRIMITIVES) return false;
    for (uint32_t i = 0; i < sdf_count; ++i) visible[i] = true;
    return true;
}
int32_t obs_sdf_add_checked(Scene *s, const char *name, int type,
                            vec3 pos, vec4 size, vec4 color);
void sdfs_add(Scene *s, const char *name, int type,
              vec3 pos, vec4 size, vec4 color);
void flushSdfSSBO(void);
bool obs_sdf_sync_frame(uint32_t slot);
#define OBS_EFFECT_WRITE 2u
#define OBS_EFFECT_GPU 4u
#define OBS_LIFETIME_ENGINE 2u
bool obs_monad_effect_allowed(unsigned effects, unsigned lifetime, bool force);
static inline void glm_mat4_identity(mat4 m) {
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) m[i][j] = i == j ? 1.0f : 0.0f;
}
static inline void glm_translate(mat4 m, const vec3 p) {
    for (unsigned i = 0; i < 3; ++i) m[3][i] += p[i];
}
static inline void glm_mat4_inv(const mat4 m, mat4 dst) {
    glm_mat4_identity(dst);
    for (unsigned i = 0; i < 3; ++i) dst[3][i] = -m[3][i];
}
static inline void glm_vec4_copy(const vec4 v, vec4 dst) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = v[i];
}
static inline void glm_vec3_copy(const vec3 v, vec3 dst) {
    for (unsigned i = 0; i < 3; ++i) dst[i] = v[i];
}
