/// gltf_async — worker-owned, nonblocking glTF scene loading
#pragma once
#include "gltf_async_cache.h"
#include "gltf_source_plan.h"
#include "pending_load.h"
#include "scene_load_vk.h"
#include <pthread.h>

/// Public status
/* Ticket belongs to the request, not Vulkan. Stale tickets never mutate the
 * active candidate. The loader is deliberately bounded to one GPU flight. */
typedef enum {
    OBS_GLTF_ASYNC_IDLE = 0, OBS_GLTF_ASYNC_PREPARING,
    OBS_GLTF_ASYNC_UPLOADING, OBS_GLTF_ASYNC_TEXTURES,
    OBS_GLTF_ASYNC_PUBLISHED, OBS_GLTF_ASYNC_FAILED,
    OBS_GLTF_ASYNC_CANCELLED, OBS_GLTF_ASYNC_QUARANTINED,
    OBS_GLTF_ASYNC_NEEDS_COOK
} ObsGltfAsyncStatus;

typedef struct ObsAsyncGltf ObsAsyncGltf;
struct ObsAsyncGltf {
    pthread_mutex_t mutex;
    pthread_t worker;
    ObsVkSceneLoadBridge bridge;
    ObsPendingSceneLoad pending;
    Scene *target;
    ObsGltfCacheCandidate *prepared;
    void *owned_payload;
    uint64_t serial, ticket, gpu_ticket;
    uint64_t last_ticket;
    ObsGltfAsyncStatus last_status;
    char *source_path;
    char *next_path;
    uint64_t next_ticket;
    ObsGltfAsyncStatus status;
    bool worker_running, worker_done, worker_ok;
    bool worker_needs_cook, cancellation_requested, initialized;
    ObsGltfSourcePlan source_plan;
    ObsGltfPlanError source_plan_error;
    uint64_t requests, published, cancelled, failed, cache_misses;
};

/* The worker handles OMDL cache hits and a constrained static cold subset.
 * NEEDS_COOK means a valid uncached glTF requires an unsupported cold-cook
 * feature (e.g. skin, morph, animation, texture, multi-node scene). Never
 * invoke the global, synchronous legacy cooker on worker/render threads. */
bool obs_gltf_async_init(ObsAsyncGltf *owner, Scene *scene, VulkanContext *context);
/* Returns a nonzero request ticket promptly on admission (0 if the initial
 * worker cannot be launched); no source parsing or Vulkan call.
 * A newer request supersedes an older one without freeing its in-flight GPU
 * ownership. Results are added to the existing scene after completion. */
uint64_t obs_gltf_async_request(ObsAsyncGltf *owner, const char *path);
ObsGltfAsyncStatus obs_gltf_async_poll(ObsAsyncGltf *owner, uint64_t ticket);
/* Cancellation is logical while GPU is in flight; physical retirement waits
 * for a real fence. Never waits on the GPU or joins a running worker. */
bool obs_gltf_async_cancel(ObsAsyncGltf *owner, uint64_t ticket);
/* Succeeds only when no worker/GPU flight/owned candidate remains. */
bool obs_gltf_async_close(ObsAsyncGltf *owner);
/* Only at process shutdown AFTER successful vkDeviceWaitIdle. May join the
 * source worker; never call from a render frame. */
bool obs_gltf_async_shutdown_quiesced(ObsAsyncGltf *owner);
