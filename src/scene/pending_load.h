/// pending_load — owner-thread scene publication across GPU completion
#pragma once

#include "geometry_async.h"
#include <stdbool.h>
#include <stdint.h>

/// Engine-owned upload backend
/* The backend must publish ALL of its GPU allocation cursors before commit
 * returns success. This is intentionally a callback interface: calling the
 * pure geometry journal directly would bypass Vulkan's committed offsets.
 * start() may reject only before accepting GPU work. UNKNOWN work must be
 * retained by the backend and recovered using a real GPU-quiescence proof. */
typedef struct {
    uint32_t vertices, indices, morphs, meshlets;
    uint32_t meshlet_vertices, meshlet_triangles;
} ObsSceneGpuBases;

typedef enum {
    OBS_SCENE_START_REJECTED = 0,
    OBS_SCENE_START_ACCEPTED,
    OBS_SCENE_START_UNKNOWN
} ObsSceneStartResult;

typedef struct {
    ObsSceneStartResult (*start)(void *user, uint64_t scene_id,
                  const uint32_t counts[OBS_GEOM_POOL_COUNT],
                  const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                  uint64_t *ticket);
    ObsAsyncState (*poll)(void *user, uint64_t ticket);
    bool (*commit)(void *user, uint64_t ticket, ObsSceneGpuBases *out);
    bool (*discard_ready)(void *user, uint64_t ticket);
    bool (*recover_quiesced)(void *user, uint64_t ticket, bool quiesced);
    void *user;
} ObsSceneLoadBackend;

/// Owned scene candidate
/* `object` is a fully prepared private scene payload, including any graph,
 * animation, material, texture or physics resources owned by the caller.
 * A successful prepare MOVES the payload and clears the caller's struct.
 * dispose() releases it after cancellation/recovery.
 * ready() is a non-mutating, allocation-free admission check.
 * activate() must be infallible and allocation-free: after backend commit,
 * it consumes the payload and makes the scene visible. The caller must not
 * invoke live engine mutation from the ready() callback. */
typedef struct {
    void *object;
    void *user;
    void (*dispose)(void *object, void *user);
    bool (*ready)(const void *object, void *user);
    void (*activate)(void *object, void *user, const ObsSceneGpuBases *bases);
} ObsSceneLoadPayload;

typedef enum {
    OBS_LOAD_EMPTY = 0,
    OBS_LOAD_IN_FLIGHT,
    OBS_LOAD_READY,
    OBS_LOAD_QUARANTINED
} ObsSceneLoadState;

typedef struct {
    ObsSceneLoadBackend backend;
    ObsSceneLoadPayload owned;
    uint64_t ticket;
    uint64_t scene_id;
    ObsSceneLoadState state;
    uint64_t accepted, activated, discarded, quarantined;
    bool initialized;
} ObsPendingSceneLoad;

/// Strict owner-thread lifecycle
/* Zero-initialize before init (for example: ObsPendingSceneLoad load = {0}). */
bool obs_scene_load_init(ObsPendingSceneLoad *load, const ObsSceneLoadBackend *backend);
/* On failure the candidate remains caller-owned. Once accepted, even an
 * unsuccessful GPU submission leaves it owned by load until cancellation or
 * quiescence recovery. Source spans are borrowed only during prepare/start;
 * the backend must take its own copy before returning. */
bool obs_scene_load_prepare(ObsPendingSceneLoad *load, uint64_t scene_id,
                            const uint32_t counts[OBS_GEOM_POOL_COUNT],
                            const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                            ObsSceneLoadPayload *candidate, uint64_t *ticket);
/* Prepare combines GPU admission because the present Vulkan BeginCooked does
 * so. Returns false on clean rejection, retaining caller ownership; UNKNOWN
 * requires a nonzero ticket and transfers ownership to quarantine. */
ObsSceneLoadState obs_scene_load_poll(ObsPendingSceneLoad *load, uint64_t ticket);
bool obs_scene_load_publish(ObsPendingSceneLoad *load, uint64_t ticket,
                             ObsSceneGpuBases *published);
/* No cancellation is allowed while GPU work might still be running. */
bool obs_scene_load_cancel(ObsPendingSceneLoad *load, uint64_t ticket);
bool obs_scene_load_recover_quiesced(ObsPendingSceneLoad *load, uint64_t ticket,
                                      bool gpu_quiesced);
/* Shutdown refuses to abandon in-flight or uncertain GPU resource owners. */
bool obs_scene_load_close(ObsPendingSceneLoad *load);
