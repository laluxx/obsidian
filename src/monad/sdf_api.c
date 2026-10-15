/// sdf_api.c — Borrowed signed-distance-field material capabilities
/// Implementation
#include "sdf_api.h"
#include "foreign.h"
#include "slot_index.h"
#include "scene.h"
#include "sdf.h"
#include <math.h>
#include <stdint.h>
#define SCENE_REF "obsidian.scene"
#define SDF_REF "obsidian.sdf"

static bool sdf_value(monad_value_t *value, ObsMonadRuntime *host,
                      SdfPrimitive **out, monad_error_t *error) {
    void *ptr = NULL;
    if (!obs_host_ref_value(value, host, SDF_REF,
            obs_host_epoch(host, scene->sdf_revision), &ptr, error)) return false;
    if (!scene->sdfs || scene->sdf_count > scene->sdf_capacity)
        return obs_host_invalid(error, "SDF storage unavailable"), false;
    size_t slot = 0;
    if (obs_slot_index(scene->sdfs, sizeof(*scene->sdfs),
                       scene->sdf_count, ptr, &slot)) {
        if (out) *out = &scene->sdfs[slot];
        return true;
    }
    return obs_host_invalid(error, "SDF reference no longer live"), false;
}
/// Borrowing and identity
static monad_status_t borrow_by_index(monad_object_space_t *sp,
    ObsMonadRuntime *host, int64_t index, monad_value_t **out, monad_error_t *e) {
    if (!out || !obs_host_owner(host) || index < 0 ||
        (uint64_t)index >= scene->sdf_count || !scene->sdfs ||
        scene->sdf_count > scene->sdf_capacity)
        return obs_host_invalid(e, "sdf: invalid or unavailable index");
    if (!obs_host_borrow(sp, host, SDF_REF, &scene->sdfs[index],
                         obs_host_epoch(host, scene->sdf_revision), out, e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

static monad_status_t sdf_at(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 2 || !args || !out || !obs_host_owner(host) ||
        !obs_host_require(args[0], host, SCENE_REF, scene, host->generation, e) ||
        monad_value_as_i64(args[1], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "expected a scene and valid sdf index");
    return borrow_by_index(sp, host, index, out, e);
}

/// Monad language surface
static monad_status_t sdf_short(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    ObsMonadRuntime *host = ctx;
    int64_t index = -1;
    if (argc != 1 || !args || !out || !obs_host_owner(host) ||
        monad_value_as_i64(args[0], &index, e) != MONAD_OK)
        return obs_host_invalid(e, "sdf expects an index");
    return borrow_by_index(sp, host, index, out, e);
}
static monad_status_t sdf_index(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    void *candidate = NULL;
    if (argc != 1 || !args || !out ||
        !obs_host_ref_value(args[0], ctx, SDF_REF,
             obs_host_epoch(ctx, scene->sdf_revision), &candidate, e) ||
        !scene->sdfs || scene->sdf_count > scene->sdf_capacity)
        return obs_host_invalid(e, "expected a live SDF");
    size_t slot = 0;
    if (!obs_slot_index(scene->sdfs, sizeof(*scene->sdfs),
                        scene->sdf_count, candidate, &slot))
        return obs_host_invalid(e, "SDF reference no longer live");
    return monad_value_make_i64(sp, (int64_t)slot, out, e);
}
static monad_status_t sdf_type(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    SdfPrimitive *sdf = NULL;
    if (argc != 1 || !args || !out || !sdf_value(args[0], ctx, &sdf, e))
        return obs_host_invalid(e, "expected a live SDF");
    return monad_value_make_i64(sp, sdf->type, out, e);
}
static monad_status_t sdf_roughness(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    SdfPrimitive *sdf = NULL;
    if (argc != 1 || !args || !out || !sdf_value(args[0], ctx, &sdf, e))
        return obs_host_invalid(e, "expected a live SDF");
    return monad_value_make_f64(sp, sdf->roughness, out, e);
}
static monad_status_t sdf_metallic(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    SdfPrimitive *sdf = NULL;
    if (argc != 1 || !args || !out || !sdf_value(args[0], ctx, &sdf, e))
        return obs_host_invalid(e, "expected a live SDF");
    return monad_value_make_f64(sp, sdf->metallic, out, e);
}
static monad_status_t sdf_set_material(monad_object_space_t *sp, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx, monad_error_t *e) {
    if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                  OBS_LIFETIME_ENGINE, false))
        return obs_host_invalid(e, "engine mutations are forbidden during Monad publication");
    SdfPrimitive *sdf = NULL;
    double roughness, metallic;
    if (argc != 3 || !args || !out || !sdf_value(args[0], ctx, &sdf, e) ||
        monad_value_as_f64(args[1], &roughness, e) != MONAD_OK ||
        monad_value_as_f64(args[2], &metallic, e) != MONAD_OK ||
        !isfinite(roughness) || roughness < 0 || roughness > 1 ||
        !isfinite(metallic) || metallic < 0 || metallic > 1)
        return obs_host_invalid(e, "roughness and metallic must be finite in [0,1]");
    /* Validate first and update as a unit before flushing the existing SSBO. */
    sdf->roughness = (float)roughness;
    sdf->metallic = (float)metallic;
    flushSdfSSBO();
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
bool obs_monad_register_sdf_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"obs-scene-sdf", "Borrow a scene-owned SDF primitive.", 2, sdf_at, NULL},
        {"sdf", "Borrow SDF primitive by index.", 1, sdf_short, NULL},
        {"sdf-index", "Index of a live borrowed SDF.", 1, sdf_index, NULL},
        {"obs-sdf-type", "SDF primitive kind.", 1, sdf_type, NULL},
        {"sdf-type", "SDF primitive kind.", 1, sdf_type, NULL},
        {"obs-sdf-roughness", "Read SDF material roughness.", 1, sdf_roughness, NULL},
        {"sdf-roughness", "Read SDF material roughness.", 1, sdf_roughness, NULL},
        {"obs-sdf-metallic", "Read SDF material metallic factor.", 1, sdf_metallic, NULL},
        {"sdf-metallic", "Read SDF material metallic factor.", 1, sdf_metallic, NULL},
        {"obs-sdf-set-material", "Atomically set validated PBR factors and flush the SSBO.", 3, sdf_set_material, NULL},
        {"sdf-material!", "Atomically set validated PBR factors and flush the SSBO.", 3, sdf_set_material, NULL},
    };
    return obs_host_register(host, entries, sizeof(entries)/sizeof(entries[0]), error);
}
