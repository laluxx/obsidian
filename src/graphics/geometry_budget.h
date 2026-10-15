/// geometry_budget — graphics subsystem
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Checked host-side geometry reservation
/* A pure, allocation-free capacity test; no Vulkan commands, counters, or
 * pending-copy state are changed by this function. This is a preflight, NOT
 * a transaction over already-submitted GPU work. */
typedef enum {
    OBS_GEOM_VERTEX, OBS_GEOM_INDEX, OBS_GEOM_MORPH,
    OBS_GEOM_MESHLET, OBS_GEOM_BOUNDS, OBS_GEOM_SKIN,
    OBS_GEOM_MESHLET_VERTEX, OBS_GEOM_MESHLET_TRIANGLE,
    OBS_GEOM_POOL_COUNT
} ObsGeometryPoolKind;

typedef struct {
    uint32_t used;           /* element offset, before this reservation */
    uint32_t requested;      /* additional elements */
    uint64_t element_bytes;  /* bytes per element, must be nonzero */
    uint64_t capacity_bytes; /* real destination buffer capacity */
} ObsGeometryPoolBudget;

typedef struct {
    ObsGeometryPoolBudget pools[OBS_GEOM_POOL_COUNT];
} ObsGeometryBudget;

typedef struct {
    uint32_t base[OBS_GEOM_POOL_COUNT];
    uint32_t end[OBS_GEOM_POOL_COUNT];
} ObsGeometryReservation;

/* Fully writes *out only on success. Handles all-zero requests; rejects
 * arithmetic overflow, overflowing element offsets and exhausted buffers. */
bool obs_geometry_reserve_preview(const ObsGeometryBudget *budget,
                                  ObsGeometryReservation *out);

/* Prevent staging-buffer overflow BEFORE any upload is enqueued. When no
 * staging buffer exists, the legacy vertex/index/morph fallback is available,
 * but meshlet streams require persistent staging in this implementation. */
bool obs_geometry_staging_preflight(const ObsGeometryBudget *budget,
                                    uint64_t staging_bytes, bool staging_mapped);
