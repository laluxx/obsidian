/// mesh_api.c — Borrowed CPU mesh references, no raw GPU handles
/// Implementation
#include "mesh_api.h"
#include "foreign.h"
#include "scene.h"
#include "vulkan_setup.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>
#define MESH_REF "obsidian.mesh"
#define SCENE_REF "obsidian.scene"

static bool valid_scene(monad_value_t *value, ObsMonadRuntime *host,
                        monad_error_t *error) {
    return obs_host_require(value, host, SCENE_REF, scene,
                            host->generation, error);
}
/* A stable registry slot replaces a moving Mesh* as the foreign token. */
static ObsIdentityView mesh_view(void) {
    return (ObsIdentityView){
        .items = scene->meshes.items, .stride = sizeof(Mesh),
        .count = scene->meshes.count, .identity_offset = offsetof(Mesh, foreign_identity),
        .revision = scene->mesh_revision, .kind = OBS_ID_MESH,
    };
}
static bool mesh_value(monad_value_t *value, ObsMonadRuntime *host,
                       Mesh **mesh, size_t *index, monad_error_t *error) {
    size_t slot = 0;
    if (scene->meshes.count > scene->meshes.capacity ||
        !obs_host_resolve_identity(value, host, MESH_REF, &scene->foreign_objects,
                                   mesh_view(), &slot, error)) return false;
    if (mesh) *mesh = &scene->meshes.items[slot];
    if (index) *index = slot;
    return true;
}
/// Borrowing and identity
static monad_status_t borrow_by_index(monad_object_space_t *sp,
    ObsMonadRuntime *host, int64_t index, monad_value_t **out, monad_error_t *e) {
    if (!out || !obs_host_owner(host) || index < 0 ||
        (uint64_t)index >= scene->meshes.count || !scene->meshes.items ||
        scene->meshes.count > scene->meshes.capacity)
        return obs_host_invalid(e, "mesh: invalid or unavailable index");
    if (!obs_host_borrow_identity(sp, host, MESH_REF, &scene->foreign_objects,
                                  mesh_view(), (size_t)index, out, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

static monad_status_t mesh_at(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 2 || !args || !out || !obs_host_owner(host) ||
        !valid_scene(args[0], host, e) ||
        monad_value_as_i64(args[1], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "expected a scene and valid mesh index");
    return borrow_by_index(sp, host, index, out, e);
}

/// Monad language surface
static monad_status_t mesh_short(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 1 || !args || !out || !obs_host_owner(host) ||
        monad_value_as_i64(args[0], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "mesh expects an index");
    return borrow_by_index(sp, host, index, out, e);
}
static monad_status_t mesh_index(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    size_t idx = 0;
    if (argc != 1 || !args || !out ||
        !mesh_value(args[0], ctx, NULL, &idx, e) || idx > INT64_MAX)
        return obs_host_invalid(e, "expected a live mesh");
    return monad_value_make_i64(sp, (int64_t)idx, out, e);
}
static monad_status_t mesh_name(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *mesh = NULL;
    if (argc != 1 || !args || !out || !mesh_value(args[0], ctx, &mesh, NULL, e))
        return obs_host_invalid(e, "expected a live mesh");
    /* A bound prevents an unterminated engine string from reading arbitrary
     * memory. Legacy engine names are not yet represented as sized strings. */
    const char *name = mesh->name ? mesh->name : "";
    size_t len = 0;
    while (len < 4096 && name[len]) ++len;
    if (len == 4096) return obs_host_invalid(e, "mesh name exceeds host limit");
    return monad_value_make_string(sp, name, len, out, e);
}
static monad_status_t mesh_visible(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *mesh = NULL;
    if (argc != 1 || !args || !out || !mesh_value(args[0], ctx, &mesh, NULL, e))
        return obs_host_invalid(e, "expected a live mesh");
    *out = monad_value_bool(sp, mesh->visible);
    return MONAD_OK;
}
static monad_status_t mesh_set_visible(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                  OBS_LIFETIME_ENGINE, false))
        return obs_host_invalid(e, "engine mutations are forbidden during Monad publication");
    Mesh *mesh = NULL;
    size_t index = 0;
    if (argc != 2 || !args || !out || !mesh_value(args[0], ctx, &mesh, &index, e) ||
        monad_value_kind(args[1]) != MONAD_VALUE_BOOL)
        return obs_host_invalid(e, "expected a live mesh and Boolean visibility");
    bool visible = monad_value_is_true(args[1]);
    if (mesh->visible != visible) {
        mesh->visible = visible;
        markMeshDirty(&context, (uint32_t)index);
    }
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t mesh_material(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *mesh = NULL;
    if (argc != 1 || !args || !out || !mesh_value(args[0], ctx, &mesh, NULL, e))
        return obs_host_invalid(e, "expected a live mesh");
    return monad_value_make_i64(sp, mesh->materialIndex, out, e);
}
bool obs_monad_register_mesh_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-scene-mesh", "Borrow a stable generation-checked mesh.", 2, mesh_at, NULL},
        {"mesh", "Borrow mesh at index from the current scene.", 1, mesh_short, NULL},
        {"obs-mesh-index", "Index of a currently live mesh.", 1, mesh_index, NULL},
        {"mesh-index", "Live mesh index.", 1, mesh_index, NULL},
        {"obs-mesh-name", "Name of a borrowed mesh.", 1, mesh_name, NULL},
        {"mesh-name", "Mesh display name.", 1, mesh_name, NULL},
        {"obs-mesh-visible?", "Read mesh render visibility.", 1, mesh_visible, NULL},
        {"mesh-visible?", "True if the mesh is visible.", 1, mesh_visible, NULL},
        {"obs-mesh-set-visible", "Set visibility and mark GPU mesh data dirty.", 2, mesh_set_visible, NULL},
        {"mesh-visible!", "Update visibility and invalidate the GPU mesh data.", 2, mesh_set_visible, NULL},
        {"obs-mesh-material-index", "Read the mesh material-table index.", 1, mesh_material, NULL},
        {"mesh-material-index", "Index of the mesh's assigned material.", 1, mesh_material, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries)/sizeof(entries[0]), error);
}
