/// gltf_node_api.c — scoped, type-checked OMDL node operations
#include "gltf_node_api.h"
#include "foreign.h"
#include "slot_index.h"
#include "effect_policy.h"
#include "scene.h"
#include "node_overrides.h"
#include <stdint.h>
#include <limits.h>
#include <string.h>

#define GLTF_REF "obsidian.gltf-instance"
#define NODE_REF "obsidian.gltf-node"

/// Resolve without trusting a script pointer
static bool gltf_node_value(monad_value_t *v, ObsMonadRuntime *host,
                            OmdlSceneGraph **graph, size_t *node_index,
                            monad_error_t *e) {
    void *candidate = NULL;
    if (!obs_host_ref_value(v, host, NODE_REF,
            obs_host_epoch(host, scene->gltf_revision), &candidate, e) ||
        scene->gltf_instance_count > scene->gltf_instance_capacity ||
        !scene->gltf_instances)
        return false;
    /* Compare candidate addresses only; don't dereference untrusted memory. */
    for (size_t i = 0; i < scene->gltf_instance_count; ++i) {
        OmdlSceneGraph *g = (OmdlSceneGraph *)scene->gltf_instances[i].gltf_data;
        size_t n = 0;
        if (g && g->nodes && g->node_count &&
            obs_slot_index(g->nodes, sizeof(*g->nodes), g->node_count,
                           candidate, &n)) {
            if (graph) *graph = g;
            if (node_index) *node_index = n;
            return true;
        }
    }
    obs_host_invalid(e, "glTF node is no longer part of the live scene");
    return false;
}
static monad_status_t node_borrow(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    void *raw = NULL;
    int64_t idx = -1;
    if (argc != 2 || !args || !out ||
        !obs_host_ref_value(args[0], host, GLTF_REF,
            obs_host_epoch(host, scene->gltf_revision), &raw, e) ||
        monad_value_as_i64(args[1], &idx, e) != MONAD_OK || idx < 0 ||
        scene->gltf_instance_count > scene->gltf_instance_capacity ||
        !scene->gltf_instances)
        return obs_host_invalid(e, "gltf-node requires a live instance and nonnegative index");
    size_t inst_idx = 0;
    if (!obs_slot_index(scene->gltf_instances, sizeof(GLTFInstance),
                        scene->gltf_instance_count, raw, &inst_idx))
        return obs_host_invalid(e, "glTF instance has expired");
    OmdlSceneGraph *g = (OmdlSceneGraph *)scene->gltf_instances[inst_idx].gltf_data;
    if (!g || !g->nodes || (uint64_t)idx >= g->node_count)
        return obs_host_invalid(e, "glTF node index out of range");
    if (!obs_host_borrow(sp, host, NODE_REF, &g->nodes[idx],
                         obs_host_epoch(host, scene->gltf_revision), out, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

/// Node metadata and script translation
static monad_status_t node_index(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    size_t n = 0;
    if (argc != 1 || !args || !out || !gltf_node_value(args[0], ctx, NULL, &n, e) || n > INT64_MAX)
        return obs_host_invalid(e, "expected a live glTF node");
    return monad_value_make_i64(sp, (int64_t)n, out, e);
}
static monad_status_t node_name(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    OmdlSceneGraph *g = NULL;
    size_t n = 0;
    if (argc != 1 || !args || !out || !gltf_node_value(args[0], ctx, &g, &n, e))
        return obs_host_invalid(e, "expected a live glTF node");
    size_t length = 0;
    while (length < sizeof(g->nodes[n].name) && g->nodes[n].name[length]) ++length;
    return monad_value_make_string(sp, g->nodes[n].name, length, out, e);
}
#define OFFSET_READER(NAME, AXIS) \
static monad_status_t NAME(monad_object_space_t *sp, size_t argc, \
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) { \
    OmdlSceneGraph *g = NULL; size_t n = 0; \
    if (argc != 1 || !args || !out || \
        !gltf_node_value(args[0], ctx, &g, &n, e) || !g->script_translations) \
        return obs_host_invalid(e, "expected a live glTF node with script overlay"); \
    return monad_value_make_f64(sp, g->script_translations[n][AXIS], out, e); \
}
OFFSET_READER(offset_x, 0)
OFFSET_READER(offset_y, 1)
OFFSET_READER(offset_z, 2)
#undef OFFSET_READER
static monad_status_t node_offset_set(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    OmdlSceneGraph *g = NULL;
    size_t n = 0;
    double xyz[3];
    if (argc != 4 || !args || !out ||
        !obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU, OBS_LIFETIME_ENGINE, false) ||
        !gltf_node_value(args[0], ctx, &g, &n, e) || !g->script_translations)
        return obs_host_invalid(e, "gltf-node-offset!: invalid write capability or node");
    for (unsigned i = 0; i < 3; ++i)
        if (monad_value_as_f64(args[i+1], &xyz[i], e) != MONAD_OK)
            return obs_host_invalid(e, "gltf-node-offset!: expected XYZ numbers");
    if (obs_node_offset_set(g->script_translations[n], xyz) == OBS_PROPERTY_INVALID)
        return obs_host_invalid(e, "gltf-node-offset!: expected finite representable XYZ");
    /* animate_scene() recomposes local/world matrices and marks touched meshes
     * dirty on the next update; do not mutate sampled matrices here. */
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t node_offset_clear(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    OmdlSceneGraph *g = NULL;
    size_t n = 0;
    if (argc != 1 || !args || !out ||
        !obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU, OBS_LIFETIME_ENGINE, false) ||
        !gltf_node_value(args[0], ctx, &g, &n, e) || !g->script_translations)
        return obs_host_invalid(e, "gltf-node-offset-clear!: invalid write capability or node");
    const double zero[3] = {0, 0, 0};
    (void)obs_node_offset_set(g->script_translations[n], zero);
    *out = monad_value_nil(sp);
    return MONAD_OK;
}

/// Public host registration
bool obs_monad_register_gltf_node_api(ObsMonadRuntime *host, monad_error_t *e) {
    static const ObsHostBinding entries[] = {
        {"gltf-node", "Borrow a glTF node by instance and index.", 2, node_borrow, NULL},
        {"gltf-node-index", "Index of a currently live glTF node.", 1, node_index, NULL},
        {"gltf-node-name", "Name of an OMDL node.", 1, node_name, NULL},
        {"gltf-node-offset-x", "Script-local translation X.", 1, offset_x, NULL},
        {"gltf-node-offset-y", "Script-local translation Y.", 1, offset_y, NULL},
        {"gltf-node-offset-z", "Script-local translation Z.", 1, offset_z, NULL},
        {"gltf-node-offset!", "Replace persistent node-local XYZ translation offset.", 4, node_offset_set, NULL},
        {"gltf-node-offset-clear!", "Clear persistent translation offset.", 1, node_offset_clear, NULL},
    };
    return obs_host_register(host, entries, sizeof entries/sizeof entries[0], e);
}
