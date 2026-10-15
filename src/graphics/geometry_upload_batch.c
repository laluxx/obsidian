#include "geometry_upload_batch.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// Checked arithmetic
static bool round_to_four(uint64_t value, uint64_t *out) {
    if (value > UINT64_MAX - 3) return false;
    *out = (value + 3) & ~UINT64_C(3);
    return true;
}

/// Prepared copy layout
bool obs_geometry_upload_plan(const ObsGeometryBudget *budget,
                              ObsGeometryUploadPlan *out) {
    if (!budget || !out) return false;
    ObsGeometryUploadPlan next = {0};
    if (!obs_geometry_reserve_preview(budget, &next.reservation)) return false;
    uint64_t cursor = 0;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryPoolBudget *p = &budget->pools[i];
        if (!p->element_bytes || p->requested > UINT64_MAX / p->element_bytes ||
            p->used > UINT64_MAX / p->element_bytes) return false;
        const uint64_t bytes = (uint64_t)p->requested * p->element_bytes;
        uint64_t copy_size = 0;
        if (!round_to_four(bytes, &copy_size)) return false;
        uint64_t dst = (uint64_t)p->used * p->element_bytes;
        if (i == OBS_GEOM_MESHLET_TRIANGLE && bytes) {
            if (!round_to_four(dst, &dst)) return false;
            /* The extra reserved padding must be accounted for in the global
             * triangle allocator, including the hole before a misaligned base. */
            if (dst > UINT32_MAX || copy_size > UINT32_MAX - dst ||
                dst + copy_size >= UINT32_MAX) return false;
            next.reservation.base[i] = (uint32_t)dst;
            next.reservation.end[i] = (uint32_t)(dst + copy_size);
        } else if (bytes && ((dst & 3) || (bytes & 3))) {
            return false;
        }
        if (dst > p->capacity_bytes || copy_size > p->capacity_bytes - dst) return false;
        if (cursor > UINT64_MAX - copy_size) return false;
        next.copies[i] = (ObsGeometryCopy){
            .source_bytes = bytes,
            .copy_bytes = copy_size,
            .source_offset = cursor,
            .destination_offset = dst,
        };
        cursor += copy_size;
    }
    if (cursor > SIZE_MAX) return false;
    next.staging_bytes = (size_t)cursor;
    *out = next;
    return true;
}

/// Failure-atomic CPU preparation
bool obs_geometry_upload_prepare_file(FILE *file, const ObsGeometryBudget *budget,
                                      ObsGeometryUploadBatch *out) {
    if (!file || !budget || !out || out->state != OBS_UPLOAD_EMPTY) return false;
    ObsGeometryUploadBatch next = {0};
    if (!obs_geometry_upload_plan(budget, &next.plan)) return false;
    /* A zero-byte scene is legal; keep the batch state explicit. */
    next.bytes = next.plan.staging_bytes ? malloc(next.plan.staging_bytes) : NULL;
    if (next.plan.staging_bytes && !next.bytes) return false;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryCopy *c = &next.plan.copies[i];
        if (!c->source_bytes) continue;
        unsigned char *ptr = next.bytes + (size_t)c->source_offset;
        if (fread(ptr, 1, (size_t)c->source_bytes, file) != (size_t)c->source_bytes) {
            free(next.bytes);
            return false;
        }
        memset(ptr + c->source_bytes, 0, (size_t)(c->copy_bytes - c->source_bytes));
    }
    next.state = OBS_UPLOAD_PREPARED;
    *out = next;
    return true;
}

/// Cooked in-memory source preparation
/* Validate *all* spans before allocation/copy. Never let a missing cooked
 * stream turn into a zero-filled or partially uploaded GPU allocation. */
bool obs_geometry_upload_prepare_memory(
    const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
    const ObsGeometryBudget *budget, ObsGeometryUploadBatch *out) {
    if (!spans || !budget || !out || out->state != OBS_UPLOAD_EMPTY) return false;
    ObsGeometryUploadBatch next = {0};
    if (!obs_geometry_upload_plan(budget, &next.plan)) return false;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryCopy *copy = &next.plan.copies[i];
        if (copy->source_bytes > SIZE_MAX ||
            spans[i].bytes != (size_t)copy->source_bytes ||
            (copy->source_bytes && !spans[i].data)) return false;
    }
    next.bytes = next.plan.staging_bytes ? malloc(next.plan.staging_bytes) : NULL;
    if (next.plan.staging_bytes && !next.bytes) return false;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const ObsGeometryCopy *copy = &next.plan.copies[i];
        if (!copy->copy_bytes) continue;
        unsigned char *dest = next.bytes + (size_t)copy->source_offset;
        if (copy->source_bytes)
            memcpy(dest, spans[i].data, (size_t)copy->source_bytes);
        memset(dest + copy->source_bytes, 0,
               (size_t)(copy->copy_bytes - copy->source_bytes));
    }
    next.state = OBS_UPLOAD_PREPARED;
    *out = next;
    return true;
}

/// Explicit fence lifecycle
bool obs_geometry_upload_submit(ObsGeometryUploadBatch *batch) {
    if (!batch || batch->state != OBS_UPLOAD_PREPARED) return false;
    batch->state = OBS_UPLOAD_SUBMITTED;
    return true;
}

bool obs_geometry_upload_retire(ObsGeometryUploadBatch *batch) {
    if (!batch || batch->state != OBS_UPLOAD_SUBMITTED) return false;
    batch->state = OBS_UPLOAD_RETIRED;
    return true;
}

bool obs_geometry_upload_discard(ObsGeometryUploadBatch *batch) {
    if (!batch || batch->state == OBS_UPLOAD_SUBMITTED) return false;
    free(batch->bytes);
    memset(batch, 0, sizeof(*batch));
    return true;
}
