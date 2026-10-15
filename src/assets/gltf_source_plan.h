/// gltf_source_plan — private cache-miss scene inventory, no engine side effects
#ifndef OBSIDIAN_GLTF_SOURCE_PLAN_H
#define OBSIDIAN_GLTF_SOURCE_PLAN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Bounded inventory result
/* A source plan is NOT a cooked geometry candidate.  It does not own GPU
 * resources and must never be submitted to the Vulkan geometry adapter. */
typedef enum {
    OBS_GLTF_PLAN_OK = 0,
    OBS_GLTF_PLAN_IO,
    OBS_GLTF_PLAN_INVALID,
    OBS_GLTF_PLAN_UNSUPPORTED,
    OBS_GLTF_PLAN_LIMIT,
    OBS_GLTF_PLAN_NOMEM
} ObsGltfPlanError;

typedef struct {
    uint64_t source_fingerprint;
    uint64_t vertices, indices, triangles;
    uint32_t nodes, meshes, primitives, images, skins, animations;
    bool has_morph_targets;
    bool has_skinning;
} ObsGltfSourcePlan;

/* Thread safe. Loads glTF buffers with cgltf, validates references, records a
 * bounded inventory, hashes external image bytes and checks indices before
 * any renderer/global mutation. This is NOT an atomic filesystem snapshot.
 * No Vulkan, Scene, texture pool, or legacy cooker globals are accessed.
 * Not a replacement for meshlet cooking or upload preparation. */
ObsGltfPlanError obs_gltf_source_plan(const char *path,
                                      ObsGltfSourcePlan *out);
const char *obs_gltf_plan_error_name(ObsGltfPlanError error);
#endif
