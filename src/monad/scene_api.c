/// scene_api.c — Borrowed scene-node capabilities with structural invalidation
/// Implementation
#include "scene_api.h"
#include "bindings.h"
#include "frame_state.h"
#include "foreign.h"
#include "scene.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OBS_SCENE_REF "obsidian.scene"
#define OBS_NODE_REF "obsidian.node"
#define OBS_FRAME_REF "obsidian.frame"
#define OBS_FRAME_SCENE_REF "obsidian.frame-scene"

/* Scene nodes compact by swapping the last slot into a deleted slot.
 * The registry tracks each node's logical identity independently of its address. */
static ObsIdentityView node_view(void) {
    return (ObsIdentityView){
        .items = scene->tree.nodes, .stride = sizeof(SceneNode),
        .count = scene->tree.count >= 0 && scene->tree.count <= SCENE_TREE_MAX_NODES
            ? (size_t)scene->tree.count : 0,
        .identity_offset = offsetof(SceneNode, foreign_identity),
        .revision = scene->tree.revision, .kind = OBS_ID_NODE,
    };
}

static monad_status_t invalid(monad_error_t *e, const char *message) {
    if (e) { e->status = MONAD_ERROR_INVALID_ARGUMENT; e->message = message; }
    return MONAD_ERROR_INVALID_ARGUMENT;
}
static bool owner(ObsMonadRuntime *host) {
    return host && host->active && host->space && obs_monad_on_owner_thread();
}
static bool scene_arg(monad_value_t *v, ObsMonadRuntime *host,
                      monad_error_t *e) {
    return obs_host_require(v, host, OBS_SCENE_REF, scene,
                            host ? host->generation : 0, e);
}
static bool node_arg(monad_value_t *v, ObsMonadRuntime *host,
                     SceneNode **out, int64_t *idx, monad_error_t *e) {
    size_t slot = 0;
    if (scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES ||
        !obs_host_resolve_identity(v, host, OBS_NODE_REF,
                                   &scene->foreign_objects, node_view(), &slot, e))
        return false;
    if (out) *out = &scene->tree.nodes[slot];
    if (idx) *idx = (int64_t)slot;
    return true;
}

/// Borrowing and identity
static monad_status_t borrow_node(monad_object_space_t *sp,
    ObsMonadRuntime *host, int64_t index, monad_value_t **out, monad_error_t *e) {
    if (!out || !owner(host) || scene->tree.count < 0 ||
        scene->tree.count > SCENE_TREE_MAX_NODES || index < 0 ||
        index >= scene->tree.count)
        return invalid(e, "node: invalid or unavailable index");
    if (!obs_host_borrow_identity(sp, host, OBS_NODE_REF, &scene->foreign_objects,
                                  node_view(), (size_t)index, out, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

static monad_status_t node_at(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **ret, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 2 || !args || !ret || !scene_arg(args[0], host, e) ||
        monad_value_as_i64(args[1], &index, e) != MONAD_OK)
        return invalid(e, "expected a scene and node index");
    return borrow_node(sp, host, index, ret, e);
}

/// Monad language surface
static monad_status_t node_short(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **ret, void *ctx, monad_error_t *e) {
    int64_t index = -1;
    if (argc != 1 || !args || !ret || !owner(ctx) ||
        monad_value_as_i64(args[0], &index, e) != MONAD_OK)
        return invalid(e, "node expects an index");
    return borrow_node(sp, ctx, index, ret, e);
}
static monad_status_t node_index(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    int64_t index = -1;
    if (argc != 1 || !argv || !ret || !node_arg(argv[0], ctx, NULL, &index, e))
        return invalid(e, "expected a live scene-node handle");
    return monad_value_make_i64(sp, index, ret, e);
}
static monad_status_t node_name(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    SceneNode *node = NULL;
    if (argc != 1 || !argv || !ret || !node_arg(argv[0], ctx, &node, NULL, e))
        return invalid(e, "expected a live scene-node handle");
    size_t n = 0;
    while (n < sizeof(node->name) && node->name[n]) ++n;
    return monad_value_make_string(sp, node->name, n, ret, e);
}
static monad_status_t node_parent(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    SceneNode *node = NULL;
    if (argc != 1 || !argv || !ret || !node_arg(argv[0], ctx, &node, NULL, e))
        return invalid(e, "expected a live scene-node handle");
    return monad_value_make_i64(sp, node->parent, ret, e);
}
static monad_status_t node_first_child(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    SceneNode *node = NULL;
    if (argc != 1 || !argv || !ret || !node_arg(argv[0], ctx, &node, NULL, e))
        return invalid(e, "expected a live scene-node handle");
    return monad_value_make_i64(sp, node->first_child, ret, e);
}
static monad_status_t node_visible(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    SceneNode *node = NULL;
    if (argc != 1 || !argv || !ret || !node_arg(argv[0], ctx, &node, NULL, e))
        return invalid(e, "expected a live scene-node handle");
    *ret = monad_value_bool(sp, node->visible);
    return MONAD_OK;
}
static monad_status_t node_set_visible(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    /* Staged Monad publication must not mutate engine-owned scene state. */
    if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE,
                                  OBS_LIFETIME_ENGINE, false))
        return invalid(e, "scene writes are forbidden during Monad publication");
    SceneNode *node = NULL;
    if (argc != 2 || !argv || !ret || !node_arg(argv[0], ctx, &node, NULL, e) ||
        (monad_value_kind(argv[1]) != MONAD_VALUE_BOOL))
        return invalid(e, "expected a live node and Boolean visibility");
    /* Scene-tree UI visibility only: no GPU culling contract implied. */
    node->visible = monad_value_is_true(argv[1]);
    *ret = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t scene_mesh_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    if (argc != 1 || !argv || !ret || !scene_arg(argv[0], ctx, e))
        return invalid(e, "expected a live scene handle");
    if (scene->meshes.count > INT64_MAX) return invalid(e, "mesh count overflow");
    return monad_value_make_i64(sp, (int64_t)scene->meshes.count, ret, e);
}
static monad_status_t scene_sdf_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx, monad_error_t *e) {
    if (argc != 1 || !argv || !ret || !scene_arg(argv[0], ctx, e))
        return invalid(e, "expected a live scene handle");
    return monad_value_make_i64(sp, scene->sdf_count, ret, e);
}

/* This view is tied to one active CPU frame. It contains no copied mesh list:
 * each lookup resolves against the current scene arrays and stable identity
 * registry, so additions, removals and renderer reordering are observed live. */
static bool frame_scene_arg(monad_value_t *value, ObsMonadRuntime *host,
                            monad_error_t *error) {
    const ObsFrameState *current = obs_frame_state_current();
    if (!current || !owner(host))
        return invalid(error, "scene view exists only on the active engine frame"), false;
    return obs_host_require(value, host, OBS_FRAME_SCENE_REF,
        obs_frame_state_identity(),
        obs_host_epoch(host, obs_frame_state_epoch()), error);
}

static bool frame_arg(monad_value_t *value, ObsMonadRuntime *host,
                      monad_error_t *error) {
    if (!obs_frame_state_current() || !owner(host))
        return invalid(error, "frame scene requires an active engine frame"), false;
    return obs_host_require(value, host, OBS_FRAME_REF,
        obs_frame_state_identity(),
        obs_host_epoch(host, obs_frame_state_epoch()), error);
}

static monad_status_t frame_scene(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx,
    monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    if (argc != 1 || !argv || !ret || !frame_arg(argv[0], host, e))
        return invalid(e, "frame-scene expects the active frame capability");
    return obs_host_borrow(sp, host, OBS_FRAME_SCENE_REF,
        (void *)obs_frame_state_identity(),
        obs_host_epoch(host, obs_frame_state_epoch()), ret, e)
        ? MONAD_OK : (e ? e->status : MONAD_ERROR_INVALID_ARGUMENT);
}

static monad_status_t frame_scene_mesh_count(monad_object_space_t *sp,
    size_t argc, monad_value_t *const *argv, monad_value_t **ret, void *ctx,
    monad_error_t *e) {
    if (argc != 1 || !argv || !ret ||
        !frame_scene_arg(argv[0], ctx, e) ||
        scene->meshes.count > scene->meshes.capacity ||
        (scene->meshes.count && !scene->meshes.items) ||
        scene->meshes.count > INT64_MAX)
        return invalid(e, "frame-scene-mesh-count expects a valid live scene view");
    return monad_value_make_i64(sp, (int64_t)scene->meshes.count, ret, e);
}

static monad_status_t frame_scene_mesh(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx,
    monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 2 || !argv || !ret ||
        !frame_scene_arg(argv[0], host, e) ||
        monad_value_as_i64(argv[1], &index, e) != MONAD_OK ||
        index < 0 || (uint64_t)index >= scene->meshes.count ||
        scene->meshes.count > scene->meshes.capacity || !scene->meshes.items)
        return invalid(e, "frame-scene-mesh expects a live scene view and mesh index");
    ObsIdentityView view = {
        .items = scene->meshes.items,
        .stride = sizeof(Mesh),
        .count = scene->meshes.count,
        .identity_offset = offsetof(Mesh, foreign_identity),
        .revision = scene->mesh_revision,
        .kind = OBS_ID_MESH,
    };
    if (!obs_host_borrow_identity(sp, host, "obsidian.mesh",
            &scene->foreign_objects, view, (size_t)index, ret, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

static monad_status_t frame_scene_node_count(monad_object_space_t *sp,
    size_t argc, monad_value_t *const *argv, monad_value_t **ret, void *ctx,
    monad_error_t *e) {
    if (argc != 1 || !argv || !ret ||
        !frame_scene_arg(argv[0], ctx, e) ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES)
        return invalid(e, "frame-scene-node-count expects a valid live scene view");
    return monad_value_make_i64(sp, scene->tree.count, ret, e);
}

static monad_status_t frame_scene_node(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *argv, monad_value_t **ret, void *ctx,
    monad_error_t *e) {
    int64_t index = -1;
    if (argc != 2 || !argv || !ret ||
        !frame_scene_arg(argv[0], ctx, e) ||
        monad_value_as_i64(argv[1], &index, e) != MONAD_OK ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES ||
        index < 0 || index >= scene->tree.count)
        return invalid(e, "frame-scene-node expects a live scene view and node index");
    return borrow_node(sp, ctx, index, ret, e);
}

bool obs_monad_register_scene_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-scene-node", "Borrow a stable, generation-checked node.", 2, node_at, NULL},
        {"node", "Borrow node by index from current scene.", 1, node_short, NULL},
        {"obs-node-index", "Index of a borrowed node.", 1, node_index, NULL},
        {"node-index", "Index of a borrowed node.", 1, node_index, NULL},
        {"obs-node-name", "Name of a borrowed node.", 1, node_name, NULL},
        {"node-name", "Name of a borrowed node.", 1, node_name, NULL},
        {"obs-node-parent", "Index of parent node, -1 for root.", 1, node_parent, NULL},
        {"node-parent", "Index of parent node, -1 for root.", 1, node_parent, NULL},
        {"obs-node-first-child", "Index of first child, -1 if none.", 1, node_first_child, NULL},
        {"node-first-child", "Index of first child, -1 if none.", 1, node_first_child, NULL},
        {"obs-node-visible?", "Read UI scene-tree visibility.", 1, node_visible, NULL},
        {"node-visible?", "Read UI scene-tree visibility.", 1, node_visible, NULL},
        {"obs-node-set-visible", "Set UI scene-tree visibility.", 2, node_set_visible, NULL},
        {"node-visible!", "Set UI scene-tree visibility.", 2, node_set_visible, NULL},
        {"obs-scene-mesh-count", "Number of mesh records in the scene.", 1, scene_mesh_count, NULL},
        {"mesh-count", "Number of mesh records in the scene.", 1, scene_mesh_count, NULL},
        {"obs-scene-sdf-count", "Number of SDF objects in the scene.", 1, scene_sdf_count, NULL},
        {"sdf-count", "Number of SDF objects in the scene.", 1, scene_sdf_count, NULL},
        {"frame-scene", "Borrow the current scene view; it expires at frame end.", 1, frame_scene, NULL},
        {"frame-scene-mesh-count", "Read the current live mesh count through a frame-scoped scene view.", 1, frame_scene_mesh_count, NULL},
        {"frame-scene-mesh", "Borrow a stable mesh handle from the live scene view.", 2, frame_scene_mesh, NULL},
        {"frame-scene-node-count", "Read the current live node count through a frame-scoped scene view.", 1, frame_scene_node_count, NULL},
        {"frame-scene-node", "Borrow a stable node handle from the live scene view.", 2, frame_scene_node, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]), error);
}
