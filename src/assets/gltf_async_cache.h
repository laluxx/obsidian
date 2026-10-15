/// gltf_async_cache — independently owned CPU scene candidate
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "geometry_upload_batch.h"
#include "scene.h"
#include "omdl_mesh_meta.h"

/// Privately owned source candidate
/* Worker-safe payload: cache hits and the supported static cold glTF subset
 * produce the SAME eight-stream ownership and scene publication contract.
 * Neither producer accesses Vulkan, global Scene, or renderer pools. */
typedef struct {
    char *source_path;
    OmdlSceneGraph *graph;
    OmdlMeshMeta *metadata;
    Mesh *meshes;
    size_t mesh_count;
    ObsCacheTexturePlan textures;
    void *streams[OBS_GEOM_POOL_COUNT];
    uint32_t counts[OBS_GEOM_POOL_COUNT];
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
} ObsGltfCacheCandidate;

/* Cold static-triangle subset; returns the same owned eight-stream candidate.
 * Unsupported features are reported by returning false (source inventory
 * determines whether that is NEEDS_COOK or malformed). */
bool obs_gltf_cook_static_cpu(const char *source, ObsGltfCacheCandidate **out);

bool obs_gltf_cache_prepare_cpu(const char *source, ObsGltfCacheCandidate **out);
void obs_gltf_cache_destroy(ObsGltfCacheCandidate *candidate);
