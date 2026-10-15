/// Async glTF production-controller contract test declarations
#pragma once
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <cglm/cglm.h>
#include "pending_load.h"
#include "cache_textures.h"
#include "gltf_source_plan.h"
#define SCENE_TREE_MAX_NODES 4096

typedef struct { float *weights; size_t target_count; } MorphData;
typedef struct {
    char *name; MorphData *morph_data; void *node; uint64_t foreign_identity;
    size_t vertexCount,indexCount,meshletCount,morphCount;
    uint32_t megaBaseVertex,megaBaseIndex,megaBaseMeshlet,
             megaBaseMeshletVertex,megaBaseMeshletTriangle,dynamicBaseVertex,morphDeltaOffset;
    int morphWeightOffset,jointCount,jointOffset;
    int32_t textureIndex,normalMapIndex,metallicRoughIndex,aoIndex,emissiveIndex,
            transmissionIndex,thicknessIndex;
} Mesh;
typedef struct { Mesh *items; uint32_t *draw_indices; size_t count,capacity; } Meshes;
typedef struct { int32_t count;uint64_t revision; } SceneTree;
typedef struct {uint32_t joints_count;} OmdlSkin;
typedef struct {int32_t skin_idx,mesh_idx;} OmdlNode;
typedef struct {uint32_t node_count,skin_count;OmdlSkin *skins;OmdlNode *nodes;} OmdlSceneGraph;
typedef struct cgltf_data {int n;} cgltf_data;
typedef struct { uint32_t vertexOffset,indexOffset,meshletOffset,meshletVertexOffset,
    meshletTriangleOffset,morphOffset,indexCount,morphCount; } OmdlMeshMeta;
typedef struct {
    void *gltf_data; size_t mesh_start_index,mesh_count; mat4 instance_transform;
    ObsCacheTextureLease texture_lease;
} GLTFInstance;
typedef struct {
    Meshes meshes; GLTFInstance *gltf_instances;
    size_t gltf_instance_count,gltf_instance_capacity;
    SceneTree tree; uint64_t mesh_revision,gltf_revision;
} Scene;
typedef struct {int n;} VulkanContext;
typedef struct { VulkanContext *context; uint64_t active_ticket; } ObsVkSceneLoadBridge;
typedef enum { TEXTURE_STATUS_EMPTY, TEXTURE_STATUS_READY, TEXTURE_STATUS_FAILED } TextureStatus;
typedef struct { TextureStatus status; bool loaded; } Texture2D;
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
typedef enum {
    OBS_GLTF_ASYNC_IDLE,OBS_GLTF_ASYNC_PREPARING,OBS_GLTF_ASYNC_UPLOADING,
    OBS_GLTF_ASYNC_TEXTURES,OBS_GLTF_ASYNC_PUBLISHED,OBS_GLTF_ASYNC_FAILED,
    OBS_GLTF_ASYNC_CANCELLED,OBS_GLTF_ASYNC_QUARANTINED,OBS_GLTF_ASYNC_NEEDS_COOK
} ObsGltfAsyncStatus;
typedef struct ObsAsyncGltf {
    pthread_mutex_t mutex;
    pthread_t worker;
    ObsVkSceneLoadBridge bridge;
    ObsPendingSceneLoad pending;
    Scene *target;
    ObsGltfCacheCandidate *prepared;
    void *owned_payload;
    uint64_t serial,ticket,gpu_ticket,last_ticket;
    ObsGltfAsyncStatus last_status;
    char *source_path,*next_path;
    uint64_t next_ticket;
    ObsGltfAsyncStatus status;
    bool worker_running,worker_done,worker_ok,worker_needs_cook,cancellation_requested,initialized;
    ObsGltfSourcePlan source_plan;
    ObsGltfPlanError source_plan_error;
    uint64_t requests,published,cancelled,failed,cache_misses;
} ObsAsyncGltf;
extern VulkanContext context;
extern bool fake_gpu_ready;
extern unsigned fake_acquires,fake_releases,fake_dirty;

bool obs_gltf_cache_prepare_cpu(const char *,ObsGltfCacheCandidate **);
bool obs_gltf_cook_static_cpu(const char *,ObsGltfCacheCandidate **);
ObsGltfPlanError obs_gltf_source_plan(const char *, ObsGltfSourcePlan *);
void obs_gltf_cache_destroy(ObsGltfCacheCandidate *);
bool obs_vk_scene_load_bridge_init(ObsVkSceneLoadBridge *,VulkanContext *);
ObsSceneLoadBackend obs_vk_scene_load_backend(ObsVkSceneLoadBridge *);
bool meshes_reserve(Meshes *,size_t);
bool meshes_append_prepared(Meshes *,Mesh *,size_t);
void scene_bump_gltf_revision(Scene *);
void scene_tree_register_gltf(Scene *,GLTFInstance *,const char *);
void markMeshesSSBODirty(VulkanContext *);
bool texture_pool_lease_acquire(void *,const char *,int32_t *);
void texture_pool_lease_release(void *,int32_t);
Texture2D *texture_pool_get(int32_t);

bool obs_gltf_async_init(ObsAsyncGltf *, Scene *,VulkanContext *);
uint64_t obs_gltf_async_request(ObsAsyncGltf *,const char *);
ObsGltfAsyncStatus obs_gltf_async_poll(ObsAsyncGltf *,uint64_t);
bool obs_gltf_async_cancel(ObsAsyncGltf *,uint64_t);
bool obs_gltf_async_close(ObsAsyncGltf *);
bool obs_gltf_async_shutdown_quiesced(ObsAsyncGltf *);
