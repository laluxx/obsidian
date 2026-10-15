#include "geometry_async.h"
#include <string.h>

/// Lifecycle invariants
static bool active(const ObsGeometryAsync *a, uint64_t ticket,
                   ObsAsyncState state) {
    return a && a->initialized && ticket && a->ticket == ticket &&
           a->state == state;
}

bool obs_geometry_async_init(ObsGeometryAsync *a, const ObsGeometryBudget *initial,
                             const ObsGeometryAsyncBackend *backend) {
    if (!a || !initial || !backend || !backend->start || !backend->poll ||
        !backend->retire || a->initialized) return false;
    ObsGeometryAsync next = {0};
    if (!obs_scene_uploads_init(&next.uploads, initial)) return false;
    next.backend = *backend;
    next.initialized = true;
    *a = next;
    return true;
}

bool obs_geometry_async_sync_idle(ObsGeometryAsync *a, const ObsGeometryBudget *current) {
    if (!a || !a->initialized || !current || a->state != OBS_ASYNC_IDLE ||
        a->uploads.scenes.live) return false;
    ObsGeometryBudget validated = *current;
    ObsGeometryReservation preview;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        validated.pools[i].requested = 0;
        const ObsGeometryPoolBudget *old = &a->uploads.scenes.pool_layout.pools[i];
        if (current->pools[i].used < a->uploads.scenes.published[i] ||
            current->pools[i].element_bytes != old->element_bytes)
            return false; /* Keep stream layout and published offsets stable. */
    }
    if (validated.pools[OBS_GEOM_MESHLET].used != validated.pools[OBS_GEOM_BOUNDS].used ||
        validated.pools[OBS_GEOM_MESHLET].used != validated.pools[OBS_GEOM_SKIN].used ||
        !obs_geometry_reserve_preview(&validated, &preview)) return false;
    a->uploads.scenes.pool_layout = validated;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        a->uploads.scenes.published[i] = a->uploads.scenes.highwater[i] =
            validated.pools[i].used;
    return true;
}

bool obs_geometry_async_prepare(ObsGeometryAsync *a, uint64_t scene_id,
                               const uint32_t counts[OBS_GEOM_POOL_COUNT],
                               const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                               uint64_t *ticket) {
    if (!a || !a->initialized || a->state != OBS_ASYNC_IDLE || !ticket) return false;
    uint64_t issued = 0;
    if (!obs_scene_upload_prepare(&a->uploads, scene_id, counts, spans, &issued))
        return false;
    a->ticket = issued;
    a->state = OBS_ASYNC_PREPARED;
    *ticket = issued;
    return true;
}

/// Backend submission and polling
bool obs_geometry_async_submit(ObsGeometryAsync *a, uint64_t ticket) {
    if (!active(a, ticket, OBS_ASYNC_PREPARED)) return false;
    const ObsGeometryUploadBatch *batch = NULL;
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i) {
        if (a->uploads.scene_tickets[i] == ticket) {
            batch = &a->uploads.flights[i].batch;
            break;
        }
    }
    if (!batch || batch->state != OBS_UPLOAD_PREPARED) return false;
    ObsAsyncSubmitResult result = a->backend.start(a->backend.user, ticket, batch);
    if (result == OBS_ASYNC_REJECTED) return false;
    /* Even a failed host-side acknowledgement cannot permit reuse after a
     * driver has accepted work. Poison until GPU completion is proven. */
    if (!obs_scene_upload_submitted(&a->uploads, ticket)) {
        a->state = OBS_ASYNC_QUARANTINED;
        return false;
    }
    if (result == OBS_ASYNC_ACCEPTED) {
        a->state = OBS_ASYNC_IN_FLIGHT;
        return true;
    }
    a->state = OBS_ASYNC_QUARANTINED;
    (void)obs_scene_upload_observe(&a->uploads, ticket, OBS_SCENE_FENCE_UNKNOWN);
    return false;
}

ObsAsyncState obs_geometry_async_poll(ObsGeometryAsync *a, uint64_t ticket) {
    if (!active(a, ticket, OBS_ASYNC_IN_FLIGHT))
        return a && a->initialized && a->ticket == ticket ? a->state : OBS_ASYNC_IDLE;
    const ObsAsyncPollResult result = a->backend.poll(a->backend.user, ticket);
    if (result == OBS_ASYNC_PENDING) return a->state;
    if (result == OBS_ASYNC_SIGNALED &&
        obs_scene_upload_observe(&a->uploads, ticket, OBS_SCENE_FENCE_SIGNALED))
        return a->state = OBS_ASYNC_READY;
    (void)obs_scene_upload_observe(&a->uploads, ticket, OBS_SCENE_FENCE_UNKNOWN);
    return a->state = OBS_ASYNC_QUARANTINED;
}

/// Commit and retirement
bool obs_geometry_async_commit(ObsGeometryAsync *a, uint64_t ticket,
                               ObsGeometryReservation *out) {
    if (!active(a, ticket, OBS_ASYNC_READY) || !out) return false;
    ObsGeometryReservation staged;
    /* GPU handles must be retired only after proven fence completion. A
     * cleanup failure leaves the scene unpublished (fail closed). */
    if (!a->backend.retire(a->backend.user, ticket, true) ||
        !obs_scene_upload_publish(&a->uploads, ticket, &staged)) {
        a->state = OBS_ASYNC_QUARANTINED;
        return false;
    }
    *out = staged;
    a->ticket = 0;
    a->state = OBS_ASYNC_IDLE;
    return obs_pending_scenes_drain(&a->uploads.scenes);
}

bool obs_geometry_async_cancel_prepared(ObsGeometryAsync *a, uint64_t ticket) {
    if (!active(a, ticket, OBS_ASYNC_PREPARED) ||
        !obs_scene_upload_cancel(&a->uploads, ticket)) return false;
    a->ticket = 0;
    a->state = OBS_ASYNC_IDLE;
    return obs_pending_scenes_drain(&a->uploads.scenes);
}

bool obs_geometry_async_discard_ready(ObsGeometryAsync *a, uint64_t ticket) {
    if (!active(a, ticket, OBS_ASYNC_READY)) return false;
    if (!a->backend.retire(a->backend.user, ticket, true) ||
        !obs_scene_upload_retire_quiesced(&a->uploads, ticket, true)) {
        a->state = OBS_ASYNC_QUARANTINED;
        return false;
    }
    a->ticket = 0;
    a->state = OBS_ASYNC_IDLE;
    return obs_pending_scenes_drain(&a->uploads.scenes);
}

bool obs_geometry_async_retire_quiesced(ObsGeometryAsync *a, uint64_t ticket,
                                       bool gpu_quiesced) {
    if (!gpu_quiesced || !a || !a->initialized || !ticket || a->ticket != ticket)
        return false;
    if (a->state == OBS_ASYNC_PREPARED)
        return obs_geometry_async_cancel_prepared(a, ticket);
    if (a->state != OBS_ASYNC_IN_FLIGHT && a->state != OBS_ASYNC_READY &&
        a->state != OBS_ASYNC_QUARANTINED) return false;
    if (!a->backend.retire(a->backend.user, ticket, true) ||
        !obs_scene_upload_retire_quiesced(&a->uploads, ticket, true)) return false;
    a->ticket = 0;
    a->state = OBS_ASYNC_IDLE;
    return obs_pending_scenes_drain(&a->uploads.scenes);
}

bool obs_geometry_async_busy(const ObsGeometryAsync *a) {
    return a && a->initialized && a->state != OBS_ASYNC_IDLE;
}
