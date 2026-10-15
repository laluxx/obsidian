#include "geometry_upload_batch.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Deterministic source fixture
static ObsGeometryBudget test_budget(void) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i] = (ObsGeometryPoolBudget){
            .used = 12, .requested = 11, .element_bytes = 4,
            .capacity_bytes = 4096,
        };
    }
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].element_bytes = 1;
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].used = 13;
    return b;
}

static void test_memory_vs_file(void) {
    ObsGeometryBudget b = test_budget();
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
    unsigned char storage[OBS_GEOM_POOL_COUNT][128] = {{0}};
    FILE *file = tmpfile();
    assert(file);
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        size_t length = b.pools[i].requested * b.pools[i].element_bytes;
        spans[i] = (ObsGeometrySourceSpan){storage[i], length};
        for (size_t k = 0; k < length; ++k)
            storage[i][k] = (unsigned char)(i * 31u + k);
        assert(fwrite(storage[i], 1, length, file) == length);
    }
    rewind(file);
    ObsGeometryUploadBatch mem = {0};
    ObsGeometryUploadBatch cached = {0};
    assert(obs_geometry_upload_prepare_memory(spans, &b, &mem));
    assert(obs_geometry_upload_prepare_file(file, &b, &cached));
    assert(mem.state == OBS_UPLOAD_PREPARED);
    assert(mem.plan.staging_bytes == cached.plan.staging_bytes);
    assert(!memcmp(mem.bytes, cached.bytes, mem.plan.staging_bytes));
    /* Preparation must own bytes independently of the cooker. */
    const unsigned char first = mem.bytes[0];
    storage[0][0] ^= 0xff;
    assert(mem.bytes[0] == first);
    assert(mem.plan.reservation.base[OBS_GEOM_MESHLET_TRIANGLE] == 16);
    assert(mem.plan.reservation.end[OBS_GEOM_MESHLET_TRIANGLE] == 28);
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryCopy *c = &mem.plan.copies[i];
        assert((c->source_offset % 4) == 0);
        assert((c->destination_offset % 4) == 0);
        assert((c->copy_bytes % 4) == 0);
        for (uint64_t j = c->source_bytes; j < c->copy_bytes; ++j)
            assert(mem.bytes[c->source_offset + j] == 0);
    }
    assert(!obs_geometry_upload_prepare_memory(spans, &b, &mem));
    assert(obs_geometry_upload_submit(&mem));
    assert(!obs_geometry_upload_discard(&mem));
    assert(obs_geometry_upload_retire(&mem));
    assert(obs_geometry_upload_discard(&mem));
    assert(obs_geometry_upload_discard(&cached));
    fclose(file);
}

/// Failure cases cannot mutate an existing output
static void test_rejection(void) {
    ObsGeometryBudget b = test_budget();
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
    unsigned char bytes[OBS_GEOM_POOL_COUNT][64] = {{0}};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        spans[i] = (ObsGeometrySourceSpan){
            .data = bytes[i],
            .bytes = b.pools[i].requested * b.pools[i].element_bytes,
        };
    ObsGeometryUploadBatch untouched = {0};
    assert(!obs_geometry_upload_prepare_memory(NULL, &b, &untouched));
    assert(!obs_geometry_upload_prepare_memory(spans, NULL, &untouched));
    assert(!obs_geometry_upload_prepare_memory(spans, &b, NULL));
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        ObsGeometrySourceSpan saved = spans[i];
        spans[i].bytes--;
        assert(!obs_geometry_upload_prepare_memory(spans, &b, &untouched));
        spans[i] = saved;
        spans[i].data = NULL;
        assert(!obs_geometry_upload_prepare_memory(spans, &b, &untouched));
        spans[i] = saved;
        assert(untouched.state == OBS_UPLOAD_EMPTY && !untouched.bytes);
    }
    b.pools[OBS_GEOM_MESHLET_TRIANGLE].capacity_bytes = 24;
    assert(!obs_geometry_upload_prepare_memory(spans, &b, &untouched));
    assert(untouched.state == OBS_UPLOAD_EMPTY && !untouched.bytes);
    /* Zero-data/zero-count streams are valid even without pointers. */
    b = test_budget();
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i].requested = 0;
        spans[i] = (ObsGeometrySourceSpan){0};
    }
    assert(obs_geometry_upload_prepare_memory(spans, &b, &untouched));
    assert(untouched.state == OBS_UPLOAD_PREPARED && untouched.bytes == NULL);
    assert(obs_geometry_upload_discard(&untouched));
}

/// Deterministic shape fuzzing
static void test_randomized(void) {
    unsigned char bytes[OBS_GEOM_POOL_COUNT][128];
    memset(bytes, 0x95, sizeof bytes);
    uint64_t x = UINT64_C(0x8ab776db167f3419);
    for (unsigned n = 0; n < 50000; ++n) {
        ObsGeometryBudget b = test_budget();
        ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
        for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            b.pools[i].used = (uint32_t)((x >> 5) % 32);
            b.pools[i].requested = (uint32_t)((x >> 13) % 32);
            if (i != OBS_GEOM_MESHLET_TRIANGLE)
                b.pools[i].element_bytes = 4;
            b.pools[i].capacity_bytes = ((x >> 30) & 1) ? 4096 : 16;
            spans[i].bytes = b.pools[i].requested * b.pools[i].element_bytes;
            spans[i].data = spans[i].bytes ? bytes[i] : NULL;
        }
        ObsGeometryUploadBatch prepared = {0};
        ObsGeometryUploadPlan plan = {0};
        bool admissible = obs_geometry_upload_plan(&b, &plan);
        bool succeeded = obs_geometry_upload_prepare_memory(spans, &b, &prepared);
        assert(admissible == succeeded);
        if (succeeded) {
            assert(prepared.plan.staging_bytes == plan.staging_bytes);
            for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
                const ObsGeometryCopy *c = &prepared.plan.copies[i];
                if (c->source_bytes)
                    assert(!memcmp(prepared.bytes + c->source_offset,
                                   spans[i].data, (size_t)c->source_bytes));
            }
            assert(obs_geometry_upload_discard(&prepared));
        } else assert(prepared.state == OBS_UPLOAD_EMPTY);
    }
}

int main(void) {
    test_memory_vs_file();
    test_rejection();
    test_randomized();
    puts("geometry-upload-memory: 50,000 prepared cooked-geometry cases passed");
    return 0;
}
