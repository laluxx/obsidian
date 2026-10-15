/// material_assets.h — scene-owned independent PBR material presets
#ifndef OBSIDIAN_SCENE_MATERIAL_ASSETS_H
#define OBSIDIAN_SCENE_MATERIAL_ASSETS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mesh_properties.h"

/// Value semantics
/* Material presets are independent from mesh-embedded properties. Applying
 * copies values to a mesh; later preset edits do not implicitly update it. */
typedef struct ObsMaterialValues {
    float roughness, metallic;
    float rgba[4];
} ObsMaterialValues;
typedef struct ObsMaterialAsset {
    uint64_t foreign_identity;
    ObsMaterialValues values;
} ObsMaterialAsset;
typedef struct ObsMaterialLibrary {
    ObsMaterialAsset *items;
    size_t count, capacity;
    uint64_t revision;
} ObsMaterialLibrary;

/// Public interface
bool obs_material_values_valid(const ObsMaterialValues *values);
ObsPropertyResult obs_material_values_apply(float *roughness, float *metallic,
                                            float rgba[4], const ObsMaterialValues *values);
bool obs_material_library_append(ObsMaterialLibrary *library,
    const ObsMaterialValues *values, size_t *new_index);
void obs_material_library_destroy(ObsMaterialLibrary *library);
#endif
