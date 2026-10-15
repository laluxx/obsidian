#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Source refs are emitted in mesh order, then skin-joint order. The builder
 * keeps only the first occurrence of a node within each mesh, matching the
 * runtime traversal's per-mesh first-joint break. */
typedef struct {
    uint32_t node_index;
    uint32_t mesh_index;
    uint32_t joint_index;
} ObsSkinJointRef;

/* Compact per-node entries; node identity is implied by the offset range. */
typedef struct {
    uint32_t mesh_index;
    uint32_t joint_index;
} ObsSkinOverrideRef;

typedef struct {
    size_t node_count;
    size_t ref_count;
    size_t *offsets; /* ref range for node n: [offsets[n], offsets[n + 1]) */
    ObsSkinOverrideRef *refs;
} ObsSkinOverrideIndex;

/* Build stable node buckets while preserving source order within each node.
 * Initialize index to {0}; on failure its existing value remains unchanged. */
bool obs_skin_override_index_build(size_t node_count, size_t mesh_count,
                                   const ObsSkinJointRef *source,
                                   size_t source_count,
                                   ObsSkinOverrideIndex *index);
void obs_skin_override_index_destroy(ObsSkinOverrideIndex *index);
