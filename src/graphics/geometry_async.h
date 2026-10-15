/// geometry_async — checked nonblocking scene-upload host handoff
#pragma once

#include "geometry_scene_upload.h"

/// Backend admission and fence results
/* REJECTED means the backend guarantees that *no GPU work was accepted*.
 * UNKNOWN means work may have been submitted, so the CPU/GPU resources must
 * remain owned until the backend can prove quiescence. */
typedef enum {
    OBS_ASYNC_REJECTED = 0,
    OBS_ASYNC_ACCEPTED,
    OBS_ASYNC_UNKNOWN
} ObsAsyncSubmitResult;

typedef enum {
    OBS_ASYNC_PENDING = 0,
    OBS_ASYNC_SIGNALED,
    OBS_ASYNC_UNCERTAIN
} ObsAsyncPollResult;

typedef struct {
    /* start is allowed to allocate Vulkan handles; it owns them once accepted.
     * It may inspect batch until it returns but must retain GPU staging itself
     * until retire returns success. Never store borrowed CPU span addresses. */
    ObsAsyncSubmitResult (*start)(void *user, uint64_t ticket,
                                  const ObsGeometryUploadBatch *batch);
    /* Nonblocking, e.g. vkGetFenceStatus. No waiting on the render thread. */
    ObsAsyncPollResult (*poll)(void *user, uint64_t ticket);
    /* Called after signaled fence OR an independently verified quiescence.
     * Must release fence, command buffers and GPU staging; false means keep
     * ownership and do not publish/recycle the reservation. */
    bool (*retire)(void *user, uint64_t ticket, bool quiesced);
    void *user;
} ObsGeometryAsyncBackend;

typedef enum {
    OBS_ASYNC_IDLE = 0,
    OBS_ASYNC_PREPARED,
    OBS_ASYNC_IN_FLIGHT,
    OBS_ASYNC_READY,
    OBS_ASYNC_QUARANTINED
} ObsAsyncState;

typedef struct {
    ObsGeometrySceneUploads uploads;
    ObsGeometryAsyncBackend backend;
    uint64_t ticket;
    ObsAsyncState state;
    bool initialized;
} ObsGeometryAsync;

/// Owner-thread entry points
/* This bridge intentionally permits ONE outstanding upload. While active,
 * the renderer must exclude ALL legacy append allocators and conventional
 * geometry batch uploads; otherwise those writers can alias reserved ranges.
 * The caller publishes its scene only AFTER obs_geometry_async_commit succeeds. */
bool obs_geometry_async_init(ObsGeometryAsync *a, const ObsGeometryBudget *initial,
                             const ObsGeometryAsyncBackend *backend);
/* Keep ticket serials across successive Vulkan uploads while synchronizing
 * committed legacy cursors between flights. Rejected while busy. */
bool obs_geometry_async_sync_idle(ObsGeometryAsync *a, const ObsGeometryBudget *current);
bool obs_geometry_async_prepare(ObsGeometryAsync *a, uint64_t scene_id,
                               const uint32_t counts[OBS_GEOM_POOL_COUNT],
                               const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                               uint64_t *ticket);
/* An UNKNOWN submission must not be cancelled as though no GPU work occurred. */
bool obs_geometry_async_submit(ObsGeometryAsync *a, uint64_t ticket);
ObsAsyncState obs_geometry_async_poll(ObsGeometryAsync *a, uint64_t ticket);
bool obs_geometry_async_commit(ObsGeometryAsync *a, uint64_t ticket,
                               ObsGeometryReservation *out);
bool obs_geometry_async_cancel_prepared(ObsGeometryAsync *a, uint64_t ticket);
/* After successful fence observation, a cancelled scene may abandon its GPU
 * ranges without ever publishing them. No device-idle wait is necessary. */
bool obs_geometry_async_discard_ready(ObsGeometryAsync *a, uint64_t ticket);
/* Retire a pending, ready, or quarantined flight only after independent
 * queue/device-idle proof. A PREPARED flight is safely cancelled without
 * calling the backend. Never treat an elapsed timeout as proof. */
bool obs_geometry_async_retire_quiesced(ObsGeometryAsync *a, uint64_t ticket,
                                       bool gpu_quiesced);
bool obs_geometry_async_busy(const ObsGeometryAsync *a);
