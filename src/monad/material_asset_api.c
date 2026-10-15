/// material_asset_api.c — typed scene material presets, distinct from mesh views
#include "material_asset_api.h"
#include "foreign.h"
#include "scene.h"
#include "material_assets.h"
#include "mesh_properties.h"
#include "vulkan_setup.h"
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#define MATERIAL_TYPE "obsidian.material-asset"
#define MESH_TYPE "obsidian.mesh"

/// Stable material and mesh identity
static ObsIdentityView live_materials(void) {
    return (ObsIdentityView){
        .items=scene->materials.items, .stride=sizeof(ObsMaterialAsset),
        .count=scene->materials.count,
        .identity_offset=offsetof(ObsMaterialAsset,foreign_identity),
        .revision=scene->materials.revision, .kind=OBS_ID_MATERIAL,
    };
}
static ObsIdentityView live_meshes(void) {
    return (ObsIdentityView){
        .items=scene->meshes.items,.stride=sizeof(Mesh),
        .count=scene->meshes.count,
        .identity_offset=offsetof(Mesh,foreign_identity),
        .revision=scene->mesh_revision,.kind=OBS_ID_MESH,
    };
}
static bool read_mesh(monad_value_t *v, ObsMonadRuntime *host,
    Mesh **mesh, size_t *index, monad_error_t *e) {
    size_t n=0;
    if (scene->meshes.count > scene->meshes.capacity ||
        !obs_host_resolve_identity(v,host,MESH_TYPE,&scene->foreign_objects,
                                   live_meshes(),&n,e)) return false;
    if (mesh) *mesh=&scene->meshes.items[n];
    if (index) *index=n;
    return true;
}
static bool read_material(monad_value_t *v, ObsMonadRuntime *host,
    ObsMaterialAsset **asset, size_t *index, monad_error_t *e) {
    size_t n=0;
    if (scene->materials.count > scene->materials.capacity ||
        !obs_host_resolve_identity(v,host,MATERIAL_TYPE,&scene->foreign_objects,
                                   live_materials(),&n,e)) return false;
    if (asset) *asset=&scene->materials.items[n];
    if (index) *index=n;
    return true;
}
static bool write_allowed(void) {
    return obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                    OBS_LIFETIME_ENGINE,false);
}
static monad_status_t borrow_asset(monad_object_space_t *sp, ObsMonadRuntime *host,
                                   size_t index, monad_value_t **out, monad_error_t *e) {
    if (!obs_host_borrow_identity(sp,host,MATERIAL_TYPE,&scene->foreign_objects,
                                  live_materials(),index,out,e))
        return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
    return MONAD_OK;
}

/// Material creation, capture and inspection
static monad_status_t asset_at(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    int64_t index=-1;
    if (n!=1 || !a || !out ||
        monad_value_as_i64(a[0],&index,e)!=MONAD_OK || index<0 ||
        (uint64_t)index >= scene->materials.count)
        return obs_host_invalid(e,"material-asset: invalid index");
    return borrow_asset(sp,ctx,(size_t)index,out,e);
}
static monad_status_t asset_count(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    (void)a;
    if (n!=0 || !out || !obs_host_owner(ctx) || scene->materials.count>INT64_MAX)
        return obs_host_invalid(e,"material-asset-count: requires owner thread");
    return monad_value_make_i64(sp,(int64_t)scene->materials.count,out,e);
}
static monad_status_t asset_store(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    Mesh *mesh=NULL;
    if (n!=1 || !a || !out || !write_allowed() ||
        !read_mesh(a[0],ctx,&mesh,NULL,e))
        return obs_host_invalid(e,"material-store: expected a writable live mesh");
    ObsMaterialValues values={.roughness=mesh->roughnessFactor,
                              .metallic=mesh->metallicFactor};
    for (unsigned i=0;i<4;++i) values.rgba[i]=mesh->baseColorFactor[i];
    size_t index=0;
    if (!obs_material_library_append(&scene->materials,&values,&index))
        return obs_host_invalid(e,"material-store: invalid values or out of memory");
    return borrow_asset(sp,ctx,index,out,e);
}
static monad_status_t asset_index(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    size_t index=0;
    if (n!=1 || !a || !out || !read_material(a[0],ctx,NULL,&index,e) || index>INT64_MAX)
        return obs_host_invalid(e,"material-asset-index: expired material");
    return monad_value_make_i64(sp,(int64_t)index,out,e);
}
#define ASSET_VALUE(NAME,FIELD) \
static monad_status_t NAME(monad_object_space_t *sp,size_t n, \
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) { \
    ObsMaterialAsset *asset=NULL; \
    if (n!=1 || !a || !out || !read_material(a[0],ctx,&asset,NULL,e)) \
        return obs_host_invalid(e,"material asset reference expired"); \
    return monad_value_make_f64(sp,asset->values.FIELD,out,e); \
}
ASSET_VALUE(asset_roughness,roughness)
ASSET_VALUE(asset_metallic,metallic)
#undef ASSET_VALUE

/// Atomic copying onto GPU-owned mesh properties
static monad_status_t asset_apply(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    ObsMaterialAsset *asset=NULL;
    Mesh *mesh=NULL;
    size_t index=0;
    if (n!=2 || !a || !out || !write_allowed() ||
        !read_material(a[0],ctx,&asset,NULL,e) ||
        !read_mesh(a[1],ctx,&mesh,&index,e) || index>UINT32_MAX ||
        !obs_material_values_valid(&asset->values))
        return obs_host_invalid(e,"material-apply!: invalid asset, mesh or effect");
    const ObsMaterialValues *v=&asset->values;
    ObsPropertyResult result=obs_material_values_apply(
        &mesh->roughnessFactor,&mesh->metallicFactor,mesh->baseColorFactor,v);
    if (result==OBS_PROPERTY_INVALID)
        return obs_host_invalid(e,"material-apply!: invalid PBR values");
    if (result==OBS_PROPERTY_CHANGED)
        markMeshDirty(&context,(uint32_t)index);
    *out=monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t asset_factors(monad_object_space_t *sp,size_t n,
    monad_value_t *const *a,monad_value_t **out,void *ctx,monad_error_t *e) {
    ObsMaterialAsset *asset=NULL;
    double r,m;
    if (n!=3 || !a || !out || !write_allowed() ||
        !read_material(a[0],ctx,&asset,NULL,e) ||
        monad_value_as_f64(a[1],&r,e)!=MONAD_OK ||
        monad_value_as_f64(a[2],&m,e)!=MONAD_OK ||
        obs_property_pbr(&asset->values.roughness,
                         &asset->values.metallic,r,m)==OBS_PROPERTY_INVALID)
        return obs_host_invalid(e,"material-asset-factors!: invalid input");
    *out=monad_value_nil(sp);
    return MONAD_OK;
}

/// Host registration
bool obs_monad_register_material_asset_api(ObsMonadRuntime *host,monad_error_t *e) {
    static const ObsHostBinding b[]={
        {"material-asset", "Borrow an independent scene material asset by index.",1,asset_at,NULL},
        {"material-asset-count","Count scene-owned reusable material presets.",0,asset_count,NULL},
        {"material-store","Capture a mesh's PBR values into an independent material asset.",1,asset_store,NULL},
        {"material-asset-index","Stable asset's current index.",1,asset_index,NULL},
        {"material-asset-roughness","Saved PBR roughness.",1,asset_roughness,NULL},
        {"material-asset-metallic","Saved PBR metallic.",1,asset_metallic,NULL},
        {"material-asset-factors!","Modify independent PBR preset factors.",3,asset_factors,NULL},
        {"material-apply!","Copy stored material factors and color to a live mesh.",2,asset_apply,NULL},
    };
    return obs_host_register(host,b,sizeof b/sizeof b[0],e);
}
