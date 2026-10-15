#include "geometry_budget.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/// Fixtures
static ObsGeometryBudget budget(void) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i] = (ObsGeometryPoolBudget){
            .used = 3, .requested = 5, .element_bytes = 32,
            .capacity_bytes = 1024
        };
    }
    return b;
}
static uint64_t rnd = UINT64_C(0x1234febabef00d99);
static uint32_t next(void) {
    rnd ^= rnd >> 12; rnd ^= rnd << 25; rnd ^= rnd >> 27;
    return (uint32_t)((rnd * UINT64_C(2685821657736338717)) >> 32);
}

/// Capacity and failure atomicity
int main(void) {
    ObsGeometryBudget b = budget();
    ObsGeometryReservation result = {0};
    assert(obs_geometry_reserve_preview(&b, &result));
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
        assert(result.base[i] == 3 && result.end[i] == 8);
    }
    assert(!obs_geometry_reserve_preview(NULL, &result));
    assert(!obs_geometry_reserve_preview(&b, NULL));

    /* The *last* pool failing must never publish partial offsets. */
    ObsGeometryReservation sentinel;
    memset(&sentinel, 0xA5, sizeof sentinel);
    ObsGeometryReservation untouched = sentinel;
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].capacity_bytes = 7 * 32;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));
    assert(memcmp(&sentinel, &untouched, sizeof sentinel) == 0);

    /* A bound not divisible by the element size must round down. */
    b = budget(); b.pools[OBS_GEOM_SKIN].capacity_bytes = 8 * 32 - 1;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));

    b = budget(); b.pools[OBS_GEOM_VERTEX].used = UINT32_MAX;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));
    b = budget(); b.pools[OBS_GEOM_VERTEX].used = UINT32_MAX - 4;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));
    b = budget(); b.pools[OBS_GEOM_VERTEX].element_bytes = 0;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));
    b = budget(); b.pools[OBS_GEOM_VERTEX].element_bytes = UINT64_MAX;
    assert(!obs_geometry_reserve_preview(&b, &sentinel));
    b = budget();
    assert(obs_geometry_staging_preflight(&b, 1024, true));
    assert(!obs_geometry_staging_preflight(&b, 400, true)); // 3 meshlet streams
    assert(!obs_geometry_staging_preflight(&b, 1024, false)); // no meshlet fallback
    b.pools[OBS_GEOM_MESHLET].requested = 0;
    b.pools[OBS_GEOM_BOUNDS].requested = 0;
    b.pools[OBS_GEOM_SKIN].requested = 0;
    b.pools[OBS_GEOM_MESHLET_VERTEX].requested = 0;
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].requested = 0;
    assert(obs_geometry_staging_preflight(&b, 0, false));
    b = budget(); b.pools[OBS_GEOM_VERTEX].requested = 0;
    assert(obs_geometry_reserve_preview(&b, &result));
    assert(result.end[OBS_GEOM_VERTEX] == result.base[OBS_GEOM_VERTEX]);

    for (unsigned n=0; n<100000; ++n) {
        b = budget();
        for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
            b.pools[i].used = next() % 1024;
            b.pools[i].requested = next() % 1024;
            b.pools[i].element_bytes = (next() % 128) + 1;
            b.pools[i].capacity_bytes = next() % 65536;
        }
        bool expected = true;
        for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
            ObsGeometryPoolBudget *p = &b.pools[i];
            if ((uint64_t)p->used + p->requested >= UINT32_MAX ||
                (uint64_t)p->used + p->requested > p->capacity_bytes/p->element_bytes) {
                expected = false;
            }
        }
        memset(&sentinel, 0xA5, sizeof sentinel);
        untouched = sentinel;
        const bool ok = obs_geometry_reserve_preview(&b, &sentinel);
        assert(ok == expected);
        if (!ok) assert(memcmp(&sentinel, &untouched, sizeof sentinel) == 0);
        else for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
            assert(sentinel.base[i] == b.pools[i].used);
            assert(sentinel.end[i] == b.pools[i].used + b.pools[i].requested);
        }
    }
    puts("geometry-budget: 100,000 capacity/failure atomicity cases passed");
}
