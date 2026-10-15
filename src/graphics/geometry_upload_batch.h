/// geometry_upload_batch — prepared geometry transfers
#pragma once

#include "geometry_budget.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/// Source and destination copy regions
/* All offsets and copy sizes are four-byte aligned for vkCmdCopyBuffer.
 * The meshlet triangle stream may acquire trailing zero padding; its GPU
 * base/end indices therefore describe reserved bytes, not source bytes. */
typedef struct {
    uint64_t source_bytes;
    uint64_t copy_bytes;
    uint64_t source_offset;
    uint64_t destination_offset;
} ObsGeometryCopy;

typedef struct {
    ObsGeometryReservation reservation;
    ObsGeometryCopy copies[OBS_GEOM_POOL_COUNT];
    size_t staging_bytes;
} ObsGeometryUploadPlan;

typedef enum {
    OBS_UPLOAD_EMPTY = 0,
    OBS_UPLOAD_PREPARED,
    OBS_UPLOAD_SUBMITTED,
    OBS_UPLOAD_RETIRED
} ObsGeometryUploadState;

/* Each stream is a borrowed CPU span. Preparation copies all eight spans into
 * batch-owned storage, so the caller may release cooked arrays afterwards.
 * Byte sizes must match the planned geometry counts exactly. */
typedef struct {
    const void *data;
    size_t bytes;
} ObsGeometrySourceSpan;

typedef struct {
    ObsGeometryUploadPlan plan;
    unsigned char *bytes;
    ObsGeometryUploadState state;
} ObsGeometryUploadBatch;

/// Plan, prepare, publish
/* On failure, outputs are unchanged. The only allocation is in prepare;
 * source data must be read completely before GPU submission is attempted. */
bool obs_geometry_upload_plan(const ObsGeometryBudget *budget,
                              ObsGeometryUploadPlan *out);
bool obs_geometry_upload_prepare_file(FILE *file, const ObsGeometryBudget *budget,
                                      ObsGeometryUploadBatch *out);
bool obs_geometry_upload_prepare_memory(
    const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
    const ObsGeometryBudget *budget, ObsGeometryUploadBatch *out);
bool obs_geometry_upload_submit(ObsGeometryUploadBatch *batch);
bool obs_geometry_upload_retire(ObsGeometryUploadBatch *batch);
/* A submitted batch cannot be discarded until its GPU fence is signaled. */
bool obs_geometry_upload_discard(ObsGeometryUploadBatch *batch);
