/// SceneLoad Vulkan adapter — isolated interface-shaped test declaration
#pragma once
#include "pending_load.h"
#include <stdint.h>
#include <stddef.h>

typedef struct VulkanContext { int sentinel; } VulkanContext;
typedef struct { unsigned char bytes[32]; } Vertex;
typedef struct { unsigned char bytes[24]; } MorphDelta;
typedef struct { unsigned char bytes[16]; } Meshlet;
typedef struct { unsigned char bytes[20]; } MeshletBounds;
typedef struct { unsigned char bytes[24]; } MeshletSkinData;
typedef struct {
    uint32_t vertices, indices, morphs, meshlets;
    uint32_t meshlet_vertices, meshlet_triangles;
} MegaGeometryCounts;
typedef struct {
    uint32_t vertices, indices, morphs, meshlets;
    uint32_t meshlet_vertices, meshlet_triangles;
} MegaGeometryBases;
typedef struct {
    const Vertex *vertices;
    const uint32_t *indices;
    const MorphDelta *morphs;
    const Meshlet *meshlets;
    const MeshletBounds *bounds;
    const MeshletSkinData *skins;
    const uint32_t *meshlet_vertices;
    const uint8_t *meshlet_triangles;
} MegaCookedGeometry;

typedef struct {
    VulkanContext *context;
    MegaGeometryCounts counts;
    uint64_t active_ticket;
} ObsVkSceneLoadBridge;

bool megaGeometryAsyncBeginCooked(VulkanContext*, uint64_t,
                                  const MegaGeometryCounts*,
                                  const MegaCookedGeometry*, uint64_t*);
ObsAsyncState megaGeometryAsyncPoll(uint64_t);
bool megaGeometryAsyncCommit(VulkanContext*, uint64_t,
                             const MegaGeometryCounts*, MegaGeometryBases*);
bool megaGeometryAsyncDiscardReady(uint64_t);
bool megaGeometryAsyncRecoverDeviceIdle(VulkanContext*, uint64_t);
bool obs_vk_scene_load_bridge_init(ObsVkSceneLoadBridge*, VulkanContext*);
ObsSceneLoadBackend obs_vk_scene_load_backend(ObsVkSceneLoadBridge*);
