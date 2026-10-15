/// material_assets.c — separate assets, stable IDs and bounded PBR values
#include "material_assets.h"
#include "object_identity.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/// Asset identity and reset
int main(void) {
    ObsIdentityRegistry registry = {0};
    ObsMaterialLibrary lib = {0};
    float rough=.5f,metal=.2f,color[4]={.1f,.2f,.3f,1};
    ObsMaterialValues value = {.roughness=.5f,.metallic=.2f,.rgba={.1f,.2f,.3f,1}};
    assert(obs_material_values_apply(&rough,&metal,color,&value)==OBS_PROPERTY_UNCHANGED);
    ObsMaterialValues changed=value; changed.metallic=.9f;
    assert(obs_material_values_apply(&rough,&metal,color,&changed)==OBS_PROPERTY_CHANGED);
    assert(metal==.9f);
    changed.rgba[2]=NAN;
    assert(obs_material_values_apply(&rough,&metal,color,&changed)==OBS_PROPERTY_INVALID);
    assert(metal==.9f && color[2]==.3f);
    size_t i=0;
    assert(obs_material_library_append(&lib, &value, &i) && i==0);
    ObsIdentityView view = {.items=lib.items,.stride=sizeof(*lib.items),
        .count=lib.count,.identity_offset=offsetof(ObsMaterialAsset,foreign_identity),
        .revision=lib.revision,.kind=OBS_ID_MATERIAL};
    const void *token=NULL; uint32_t generation=0;
    assert(obs_identity_borrow(&registry, view, i, &token, &generation));
    assert(token && generation);
    const size_t total = 50000;
    for (size_t n = 0; n < total; ++n) {
        value.roughness=(float)(n%100)/100;
        assert(obs_material_library_append(&lib, &value, NULL));
        view.items=lib.items; view.count=lib.count; view.revision=lib.revision;
        size_t found=SIZE_MAX;
        assert(obs_identity_resolve(&registry, view, token, generation, &found));
        assert(found==0);
    }
    value.rgba[1]=NAN;
    const size_t previous=lib.count;
    assert(!obs_material_library_append(&lib, &value, NULL));
    assert(lib.count==previous);
    obs_material_library_destroy(&lib);
    view.items=lib.items;view.count=lib.count;view.revision=lib.revision;
    size_t found=0;
    assert(!obs_identity_resolve(&registry,view,token,generation,&found));
    obs_identity_destroy(&registry);
    puts("material_assets: 50,000 appends, stable references, invalidation PASS");
}
