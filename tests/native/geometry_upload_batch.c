#include "geometry_upload_batch.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Deterministic test data
static ObsGeometryBudget budget(void) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
        b.pools[i] = (ObsGeometryPoolBudget){
            .used = 4, .requested = 7, .element_bytes = 4,
            .capacity_bytes = 4096
        };
    }
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].element_bytes = 1;
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].used = 5;
    return b;
}
static FILE *make_source(const ObsGeometryBudget *b, bool truncate) {
    FILE *f = tmpfile(); assert(f);
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
        const ObsGeometryPoolBudget *p = &b->pools[i];
        size_t n = (size_t)p->element_bytes * p->requested;
        if (truncate && i == OBS_GEOM_POOL_COUNT - 1 && n) n--;
        for (size_t j = 0; j < n; j++)
            assert(fputc((int)((i * 37u + j) & 255u), f) != EOF);
    }
    assert(fseek(f, 0, SEEK_SET) == 0);
    return f;
}

/// Failure atomicity and four-byte Vulkan copies
int main(void) {
    ObsGeometryBudget b = budget();
    ObsGeometryUploadPlan p = {0};
    assert(obs_geometry_upload_plan(&b, &p));
    assert(p.reservation.base[OBS_GEOM_MESHLET_TRIANGLE] == 8);
    assert(p.reservation.end[OBS_GEOM_MESHLET_TRIANGLE] == 16);
    assert(p.copies[OBS_GEOM_MESHLET_TRIANGLE].source_bytes == 7);
    assert(p.copies[OBS_GEOM_MESHLET_TRIANGLE].copy_bytes == 8);
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
        assert(p.copies[i].source_offset % 4 == 0);
        assert(p.copies[i].destination_offset % 4 == 0);
        assert(p.copies[i].copy_bytes % 4 == 0);
    }
    FILE *f = make_source(&b, false);
    ObsGeometryUploadBatch batch = {0};
    assert(obs_geometry_upload_prepare_file(f, &b, &batch));
    assert(batch.state == OBS_UPLOAD_PREPARED);
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
        const ObsGeometryCopy *c = &batch.plan.copies[i];
        for (size_t j = 0; j < c->source_bytes; j++)
            assert(batch.bytes[c->source_offset + j] == (unsigned char)((i * 37u + j) & 255u));
        for (size_t j = c->source_bytes; j < c->copy_bytes; j++)
            assert(batch.bytes[c->source_offset + j] == 0);
    }
    assert(obs_geometry_upload_submit(&batch));
    assert(!obs_geometry_upload_submit(&batch));
    assert(!obs_geometry_upload_discard(&batch));
    assert(obs_geometry_upload_retire(&batch));
    assert(!obs_geometry_upload_retire(&batch));
    assert(obs_geometry_upload_discard(&batch));
    fclose(f);

    f = make_source(&b, true);
    ObsGeometryUploadBatch old = {0};
    assert(!obs_geometry_upload_prepare_file(f, &b, &old));
    assert(old.bytes == NULL && old.state == OBS_UPLOAD_EMPTY);
    fclose(f);
    ObsGeometryUploadPlan sentinel;
    memset(&sentinel, 0xA5, sizeof sentinel);
    ObsGeometryUploadPlan expected = sentinel;
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].capacity_bytes = 15;
    assert(!obs_geometry_upload_plan(&b, &sentinel));
    assert(memcmp(&expected, &sentinel, sizeof sentinel) == 0);

    b = budget(); b.pools[OBS_GEOM_VERTEX].element_bytes = 5;
    assert(!obs_geometry_upload_plan(&b, &sentinel));
    b = budget(); b.pools[OBS_GEOM_VERTEX].element_bytes = UINT64_MAX;
    assert(!obs_geometry_upload_plan(&b, &sentinel));

    /* Randomly mutate stream sizes, alignment, destinations and capacities;
     * successful layouts must meet the Vulkan copy-alignment contract. */
    uint64_t seed = UINT64_C(0x8abc51a541bc76c1);
    for (unsigned pass = 0; pass < 50000; ++pass) {
        b = budget();
        for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            b.pools[i].requested = (uint32_t)(seed % 113);
            b.pools[i].used = (uint32_t)((seed >> 8) % 80);
            b.pools[i].element_bytes = i == OBS_GEOM_MESHLET_TRIANGLE ? 1 : 4;
            b.pools[i].capacity_bytes = (seed >> 16) % 1024;
        }
        if (obs_geometry_upload_plan(&b, &p)) {
            for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
                const ObsGeometryCopy *c = &p.copies[i];
                if (c->copy_bytes) assert((c->source_offset | c->destination_offset | c->copy_bytes) % 4 == 0);
                assert(c->copy_bytes >= c->source_bytes);
                assert(c->destination_offset + c->copy_bytes <= b.pools[i].capacity_bytes);
                assert(p.reservation.end[i] >= p.reservation.base[i]);
            }
        }
    }
    puts("geometry-upload-batch: 50,000 layout and failure-atomicity cases passed");
    return 0;
}
