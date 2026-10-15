/// material_assets.c — independent material value ownership
#include "material_assets.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/// Validation
bool obs_material_values_valid(const ObsMaterialValues *v) {
    if (!v || !isfinite(v->roughness) || !isfinite(v->metallic) ||
        v->roughness < 0 || v->roughness > 1 ||
        v->metallic < 0 || v->metallic > 1) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(v->rgba[i]) || v->rgba[i] < 0 || v->rgba[i] > 1)
            return false;
    return true;
}

/// Atomic application (GPU dirty notification belongs to the caller)
ObsPropertyResult obs_material_values_apply(float *roughness, float *metallic,
                                            float rgba[4], const ObsMaterialValues *v) {
    if (!roughness || !metallic || !rgba || !obs_material_values_valid(v))
        return OBS_PROPERTY_INVALID;
    bool changed = *roughness != v->roughness || *metallic != v->metallic;
    for (unsigned i = 0; i < 4; ++i) changed |= rgba[i] != v->rgba[i];
    if (!changed) return OBS_PROPERTY_UNCHANGED;
    *roughness = v->roughness;
    *metallic = v->metallic;
    for (unsigned i = 0; i < 4; ++i) rgba[i] = v->rgba[i];
    return OBS_PROPERTY_CHANGED;
}

/// Scene ownership
/* Revisions are process-monotonic, so a reset never revives old snapshots. */
static uint64_t s_material_revision;
static uint64_t next_revision(void) {
    if (++s_material_revision == 0) ++s_material_revision;
    return s_material_revision;
}
bool obs_material_library_append(ObsMaterialLibrary *l,
                                 const ObsMaterialValues *v, size_t *new_index) {
    if (!l || !obs_material_values_valid(v) ||
        l->count > l->capacity || (l->count && !l->items)) return false;
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 8;
        if (capacity < l->capacity || capacity > SIZE_MAX / sizeof(*l->items)) return false;
        ObsMaterialAsset *next = realloc(l->items, capacity * sizeof(*next));
        if (!next) return false;
        l->items = next;
        l->capacity = capacity;
    }
    if (new_index) *new_index = l->count;
    l->items[l->count++] = (ObsMaterialAsset){.values = *v};
    l->revision = next_revision();
    return true;
}
void obs_material_library_destroy(ObsMaterialLibrary *l) {
    if (!l) return;
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
    l->revision = next_revision();
}
