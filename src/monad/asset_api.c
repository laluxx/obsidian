/// asset_api.c — Borrowed scene-owned glTF instance metadata
/// Implementation
#include "asset_api.h"
#include "foreign.h"
#include "slot_index.h"
#include "scene.h"
#include <stdint.h>
#include <limits.h>
#define SCENE_REF "obsidian.scene"
#define GLTF_REF "obsidian.gltf-instance"

static bool instance_value(monad_value_t *value, ObsMonadRuntime *host,
                           GLTFInstance **out, monad_error_t *error) {
    void *candidate = NULL;
    if (!obs_host_ref_value(value, host, GLTF_REF,
            obs_host_epoch(host, scene->gltf_revision), &candidate, error)) return false;
    if (!scene->gltf_instances || scene->gltf_instance_count > scene->gltf_instance_capacity)
        return obs_host_invalid(error, "glTF scene storage unavailable"), false;
    size_t slot = 0;
    if (obs_slot_index(scene->gltf_instances, sizeof(*scene->gltf_instances),
                       scene->gltf_instance_count, candidate, &slot)) {
        if (out) *out = &scene->gltf_instances[slot];
        return true;
    }
    return obs_host_invalid(error, "glTF instance reference no longer live"), false;
}
/// Borrowing and identity
static monad_status_t borrow_by_index(monad_object_space_t *sp,
    ObsMonadRuntime *host, int64_t index, monad_value_t **out, monad_error_t *e) {
    if (!out || !obs_host_owner(host) || index < 0 ||
        (uint64_t)index >= scene->gltf_instance_count || !scene->gltf_instances ||
        scene->gltf_instance_count > scene->gltf_instance_capacity)
        return obs_host_invalid(e, "gltf: invalid or unavailable index");
    if (!obs_host_borrow(sp, host, GLTF_REF, &scene->gltf_instances[index],
                         obs_host_epoch(host, scene->gltf_revision), out, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

static monad_status_t instance_at(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 2 || !args || !out || !obs_host_owner(host) ||
        !obs_host_require(args[0], host, SCENE_REF, scene, host->generation, e) ||
        monad_value_as_i64(args[1], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "expected a scene and valid gltf index");
    return borrow_by_index(sp, host, index, out, e);
}

/// Monad language surface
static monad_status_t instance_short(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 1 || !args || !out || !obs_host_owner(host) ||
        monad_value_as_i64(args[0], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "gltf expects an index");
    return borrow_by_index(sp, host, index, out, e);
}
static monad_status_t instance_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    if (argc != 1 || !args || !out || !obs_host_owner(host) ||
        !obs_host_require(args[0], host, SCENE_REF, scene, host->generation, e) ||
        scene->gltf_instance_count > INT64_MAX)
        return obs_host_invalid(e, "expected a live scene");
    return monad_value_make_i64(sp, (int64_t)scene->gltf_instance_count, out, e);
}
static monad_status_t instance_index(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    void *candidate = NULL;
    if (argc != 1 || !args || !out ||
        !obs_host_ref_value(args[0], ctx, GLTF_REF,
             obs_host_epoch(ctx, scene->gltf_revision), &candidate, e) ||
        !scene->gltf_instances ||
        scene->gltf_instance_count > scene->gltf_instance_capacity)
        return obs_host_invalid(e, "expected a live glTF instance");
    size_t slot = 0;
    if (!obs_slot_index(scene->gltf_instances, sizeof(*scene->gltf_instances),
                        scene->gltf_instance_count, candidate, &slot) || slot > INT64_MAX)
        return obs_host_invalid(e, "glTF instance reference no longer live");
    return monad_value_make_i64(sp, (int64_t)slot, out, e);
}
static monad_status_t mesh_start(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    GLTFInstance *inst = NULL;
    if (argc != 1 || !args || !out || !instance_value(args[0], ctx, &inst, e) ||
        inst->mesh_start_index > INT64_MAX)
        return obs_host_invalid(e, "expected a live glTF instance");
    return monad_value_make_i64(sp, (int64_t)inst->mesh_start_index, out, e);
}
static monad_status_t mesh_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    GLTFInstance *inst = NULL;
    if (argc != 1 || !args || !out || !instance_value(args[0], ctx, &inst, e) ||
        inst->mesh_count > INT64_MAX)
        return obs_host_invalid(e, "expected a live glTF instance");
    return monad_value_make_i64(sp, (int64_t)inst->mesh_count, out, e);
}
static monad_status_t animation_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    GLTFInstance *inst = NULL;
    if (argc != 1 || !args || !out || !instance_value(args[0], ctx, &inst, e) ||
        inst->animation_count > INT64_MAX)
        return obs_host_invalid(e, "expected a live glTF instance");
    return monad_value_make_i64(sp, (int64_t)inst->animation_count, out, e);
}
static monad_status_t joint_count(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    GLTFInstance *inst = NULL;
    if (argc != 1 || !args || !out || !instance_value(args[0], ctx, &inst, e))
        return obs_host_invalid(e, "expected a live glTF instance");

    int64_t total = 0;
    if (!obs_monad_gltf_joint_count(inst->gltf_data, &total))
        return obs_host_invalid(e, "glTF skin metadata unavailable or joint count exceeds Monad Int");
    return monad_value_make_i64(sp, total, out, e);
}
bool obs_monad_register_asset_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-scene-gltf-count", "Number of scene-owned glTF instances.", 1, instance_count, NULL},
        {"gltf-count", "Number of scene-owned glTF instances.", 1, instance_count, NULL},
        {"obs-scene-gltf", "Borrow a glTF instance with structural epoch checking.", 2, instance_at, NULL},
        {"gltf", "Borrow a glTF instance by index.", 1, instance_short, NULL},
        {"gltf-index", "Index of a live borrowed glTF instance.", 1, instance_index, NULL},
        {"obs-gltf-mesh-start", "First mesh index associated with the instance.", 1, mesh_start, NULL},
        {"gltf-mesh-start", "First mesh index associated with the instance.", 1, mesh_start, NULL},
        {"obs-gltf-mesh-count", "Number of meshes associated with the instance.", 1, mesh_count, NULL},
        {"gltf-mesh-count", "Number of meshes associated with the instance.", 1, mesh_count, NULL},
        {"obs-gltf-animation-count", "Number of animations in the instance.", 1, animation_count, NULL},
        {"gltf-animation-count", "Number of animations in the instance.", 1, animation_count, NULL},
        {"obs-gltf-joint-count", "Sum of joint entries across the instance's glTF skins; zero when no skin metadata exists.", 1, joint_count, NULL},
        {"gltf-joint-count", "Sum of joint entries across the instance's glTF skins; zero when no skin metadata exists.", 1, joint_count, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries)/sizeof(entries[0]), error);
}
