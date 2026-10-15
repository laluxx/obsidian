#include "geometry_budget.h"
#include <limits.h>

/// Reservation planning
bool obs_geometry_reserve_preview(const ObsGeometryBudget *budget,
                                  ObsGeometryReservation *out) {
    if (!budget || !out) return false;
    ObsGeometryReservation proposed = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryPoolBudget *b = &budget->pools[i];
        if (!b->element_bytes) return false;
        /* UINT32_MAX is the renderer's invalid base address. Reserve it. */
        if (b->used == UINT32_MAX ||
            (uint64_t)b->used + b->requested >= UINT32_MAX) return false;
        const uint64_t slots = b->capacity_bytes / b->element_bytes;
        if ((uint64_t)b->used + b->requested > slots) return false;
        proposed.base[i] = b->used;
        proposed.end[i] = b->used + b->requested;
    }
    *out = proposed;
    return true;
}

/// Upload staging preflight
bool obs_geometry_staging_preflight(const ObsGeometryBudget *budget,
                                    uint64_t staging_bytes, bool staging_mapped) {
    if (!budget) return false;
    uint64_t meshlet_total = 0;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryPoolBudget *b = &budget->pools[i];
        if (!b->element_bytes ||
            b->requested > UINT64_MAX / b->element_bytes) return false;
        const uint64_t bytes = (uint64_t)b->requested * b->element_bytes;
        const bool meshlet_stream = i >= OBS_GEOM_MESHLET;
        if (bytes && meshlet_stream && !staging_mapped) return false;
        /* Morph has an existing standalone staging fallback; the other
         * streams unconditionally write to persistent staging when present. */
        if (staging_mapped && i != OBS_GEOM_MORPH && bytes > staging_bytes)
            return false;
        if (i >= OBS_GEOM_MESHLET && i <= OBS_GEOM_SKIN) {
            if (bytes > UINT64_MAX - meshlet_total) return false;
            meshlet_total += bytes;
        }
    }
    return !staging_mapped || meshlet_total <= staging_bytes;
}
