/// mesh_views_api.c — typed material/transform views of stable scene meshes
#include "mesh_views_api.h"
#include "foreign.h"
#include "scene.h"
#include "mesh_properties.h"
#include "physics.h"
#include "vulkan_setup.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/// Identity and ownership
/* Material in the current renderer is an embedded set of PBR fields on Mesh,
 * NOT a separately allocated instance of renderer.h's immediate Material.
 * Both MaterialView and TransformView share the mesh's stable registry slot
 * but use DISTINCT Monad nominal foreign types. The underlying pointer is
 * never exposed to scripts, retained or dereferenced without validation. */
#define MESH_REF "obsidian.mesh"
#define MATERIAL_REF "obsidian.material"
#define TRANSFORM_REF "obsidian.transform"
static ObsIdentityView live_meshes(void) {
    return (ObsIdentityView){
        .items = scene->meshes.items, .stride = sizeof(Mesh),
        .count = scene->meshes.count,
        .identity_offset = offsetof(Mesh, foreign_identity),
        .revision = scene->mesh_revision, .kind = OBS_ID_MESH,
    };
}
static bool current_mesh(monad_value_t *value, ObsMonadRuntime *host,
                         const char *type, Mesh **mesh, size_t *index,
                         monad_error_t *e) {
    size_t slot = 0;
    if (scene->meshes.count > scene->meshes.capacity ||
        !obs_host_resolve_identity(value, host, type, &scene->foreign_objects,
                                   live_meshes(), &slot, e)) return false;
    if (mesh) *mesh = &scene->meshes.items[slot];
    if (index) *index = slot;
    return true;
}
static monad_status_t borrowed_view(monad_object_space_t *sp, size_t argc,
     monad_value_t *const *args, monad_value_t **out, void *ctx,
     monad_error_t *e, const char *type) {
    size_t index = 0;
    ObsMonadRuntime *host = ctx;
    if (!out || argc != 1 || !args ||
        !current_mesh(args[0], host, MESH_REF, NULL, &index, e) ||
        !obs_host_borrow_identity(sp, host, type, &scene->foreign_objects,
                                  live_meshes(), index, out, e))
        return obs_host_invalid(e, "expected a live mesh for a typed view");
    return MONAD_OK;
}
static monad_status_t material_view(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    return borrowed_view(sp, argc, args, out, ctx, e, MATERIAL_REF);
}
static monad_status_t transform_view(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    return borrowed_view(sp, argc, args, out, ctx, e, TRANSFORM_REF);
}

/// Read-only material and transform queries
static monad_status_t material_metallic(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    if (n != 1 || !a || !out ||
        !current_mesh(a[0], ctx, MATERIAL_REF, &m, NULL, e))
        return obs_host_invalid(e, "expected a live material");
    return monad_value_make_f64(sp, m->metallicFactor, out, e);
}
static monad_status_t material_roughness(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    if (n != 1 || !a || !out ||
        !current_mesh(a[0], ctx, MATERIAL_REF, &m, NULL, e))
        return obs_host_invalid(e, "expected a live material");
    return monad_value_make_f64(sp, m->roughnessFactor, out, e);
}
#define COLOR_READER(FNAME, CHANNEL) \
static monad_status_t FNAME(monad_object_space_t *sp, size_t n, \
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) { \
    Mesh *m = NULL; \
    if (n != 1 || !a || !out || \
        !current_mesh(a[0], ctx, MATERIAL_REF, &m, NULL, e)) \
        return obs_host_invalid(e, "expected a live material"); \
    return monad_value_make_f64(sp, m->baseColorFactor[CHANNEL], out, e); \
}
COLOR_READER(material_red, 0)
COLOR_READER(material_green, 1)
COLOR_READER(material_blue, 2)
COLOR_READER(material_alpha, 3)
#undef COLOR_READER
#define POSITION_READER(FNAME, AXIS) \
static monad_status_t FNAME(monad_object_space_t *sp, size_t n, \
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) { \
    Mesh *m = NULL; \
    if (n != 1 || !a || !out || \
        !current_mesh(a[0], ctx, TRANSFORM_REF, &m, NULL, e)) \
        return obs_host_invalid(e, "expected a live transform"); \
    return monad_value_make_f64(sp, m->model[3][AXIS], out, e); \
}
POSITION_READER(transform_x, 0)
POSITION_READER(transform_y, 1)
POSITION_READER(transform_z, 2)
#undef POSITION_READER

/// Guarded GPU-visible mutations
static bool gpu_write_allowed(void) {
    return obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                     OBS_LIFETIME_ENGINE, false);
}
static monad_status_t set_material_factors(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    size_t index = 0;
    double r, metal;
    if (!gpu_write_allowed() || n != 3 || !a || !out ||
        !current_mesh(a[0], ctx, MATERIAL_REF, &m, &index, e) ||
        monad_value_as_f64(a[1], &r, e) != MONAD_OK ||
        monad_value_as_f64(a[2], &metal, e) != MONAD_OK ||
        index > UINT32_MAX)
        return obs_host_invalid(e, "material-factors!: invalid values, capability or effect");
    ObsPropertyResult state = obs_property_pbr(&m->roughnessFactor, &m->metallicFactor, r, metal);
    if (state == OBS_PROPERTY_INVALID)
        return obs_host_invalid(e, "material factors must be finite within [0,1]");
    if (state == OBS_PROPERTY_CHANGED) markMeshDirty(&context, (uint32_t)index);
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t set_material_color(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    size_t index = 0;
    double rgba[4];
    if (!gpu_write_allowed() || n != 5 || !a || !out ||
        !current_mesh(a[0], ctx, MATERIAL_REF, &m, &index, e) || index > UINT32_MAX)
        return obs_host_invalid(e, "material-color!: invalid material or effect");
    for (size_t k = 0; k < 4; ++k)
        if (monad_value_as_f64(a[k + 1], &rgba[k], e) != MONAD_OK)
            return obs_host_invalid(e, "material-color!: expected RGBA numbers");
    ObsPropertyResult state = obs_property_rgba(m->baseColorFactor, rgba);
    if (state == OBS_PROPERTY_INVALID)
        return obs_host_invalid(e, "material-color!: finite RGBA components within [0,1] required");
    if (state == OBS_PROPERTY_CHANGED) markMeshDirty(&context, (uint32_t)index);
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t set_world_position(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    size_t index = 0;
    double xyz[3];
    if (!gpu_write_allowed() || n != 4 || !a || !out ||
        !current_mesh(a[0], ctx, TRANSFORM_REF, &m, &index, e) || index > UINT32_MAX)
        return obs_host_invalid(e, "transform-position!: invalid transform or effect");
    for (size_t k = 0; k < 3; ++k)
        if (monad_value_as_f64(a[k + 1], &xyz[k], e) != MONAD_OK)
            return obs_host_invalid(e, "transform-position!: expected XYZ numbers");
    ObsPropertyResult state = obs_property_world_position(m->model, xyz);
    if (state == OBS_PROPERTY_INVALID)
        return obs_host_invalid(e, "transform-position!: expected finite, representable XYZ");
    if (state == OBS_PROPERTY_CHANGED) {
        physics_set_transform(m);
        markMeshDirty(&context, (uint32_t)index);
    }
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t rotate_world_axis(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    Mesh *m = NULL;
    size_t index = 0;
    double axis[3], radians;
    if (!gpu_write_allowed() || n != 5 || !a || !out ||
        !current_mesh(a[0], ctx, TRANSFORM_REF, &m, &index, e) ||
        index > UINT32_MAX)
        return obs_host_invalid(e, "transform-rotate!: invalid transform or effect");
    for (size_t k = 0; k < 3; ++k)
        if (monad_value_as_f64(a[k + 1], &axis[k], e) != MONAD_OK)
            return obs_host_invalid(e, "transform-rotate!: expected axis XYZ");
    if (monad_value_as_f64(a[4], &radians, e) != MONAD_OK)
        return obs_host_invalid(e, "transform-rotate!: expected radians");
    ObsPropertyResult state =
        obs_property_world_rotate_axis(m->model, axis, radians);
    if (state == OBS_PROPERTY_INVALID)
        return obs_host_invalid(e,
            "transform-rotate!: expected a finite angle and nonzero finite axis");
    if (state == OBS_PROPERTY_CHANGED) {
        physics_set_transform(m);
        markMeshDirty(&context, (uint32_t)index);
    }
    *out = monad_value_nil(sp);
    return MONAD_OK;
}

/// Monad host procedure catalog
bool obs_monad_register_mesh_views_api(ObsMonadRuntime *host, monad_error_t *e) {
    static const ObsHostBinding entries[] = {
        {"material", "Borrow PBR factors belonging to an existing mesh.", 1, material_view, NULL},
        {"mesh-material", "Borrow the material view of a mesh.", 1, material_view, NULL},
        {"transform", "Borrow world-model transform of a mesh.", 1, transform_view, NULL},
        {"mesh-transform", "Borrow the transform view of a mesh.", 1, transform_view, NULL},
        {"material-metallic", "PBR metallic factor.", 1, material_metallic, NULL},
        {"material-roughness", "PBR roughness factor.", 1, material_roughness, NULL},
        {"material-red", "PBR base color red.", 1, material_red, NULL},
        {"material-green", "PBR base color green.", 1, material_green, NULL},
        {"material-blue", "PBR base color blue.", 1, material_blue, NULL},
        {"material-alpha", "PBR base color alpha.", 1, material_alpha, NULL},
        {"material-factors!", "Set roughness and metallic atomically; dirties GPU slot on change.", 3, set_material_factors, NULL},
        {"material-color!", "Set RGBA atomically; dirties GPU slot on change.", 5, set_material_color, NULL},
        {"transform-x", "World model translation X.", 1, transform_x, NULL},
        {"transform-y", "World model translation Y.", 1, transform_y, NULL},
        {"transform-z", "World model translation Z.", 1, transform_z, NULL},
        {"transform-position!", "Set world position; teleports attached physics bodies and dirties GPU slot on change.", 4, set_world_position, NULL},
        {"transform-rotate!", "Rotate a mesh about a world-space axis in radians; preserves translation and basis scale.", 5, rotate_world_axis, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries)/sizeof(entries[0]), e);
}
