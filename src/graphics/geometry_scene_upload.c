#include "geometry_scene_upload.h"
#include <string.h>

/// Slot and reservation invariants
static int find_active(const ObsGeometrySceneUploads *queue, uint64_t ticket) {
    if (!queue || !ticket) return -1;
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i)
        if (queue->scene_tickets[i] == ticket) return (int)i;
    return -1;
}

static int find_free(const ObsGeometrySceneUploads *queue) {
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i)
        if (!queue->scene_tickets[i] &&
            queue->flights[i].state == OBS_FLIGHT_EMPTY) return (int)i;
    return -1;
}

static bool same_reservation(const ObsGeometryReservation *a,
                              const ObsGeometryReservation *b) {
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        if (a->base[i] != b->base[i] || a->end[i] != b->end[i]) return false;
    return true;
}

bool obs_scene_uploads_init(ObsGeometrySceneUploads *queue,
                            const ObsGeometryBudget *initial) {
    /* Never reset a live image (or its generation tickets) in place. */
    if (!queue || !initial || queue->scenes.initialized) return false;
    ObsPendingScenes scenes = {0};
    if (!obs_pending_scenes_init(&scenes, initial)) return false;
    ObsGeometrySceneUploads next = {0};
    next.scenes = scenes;
    *queue = next;
    return true;
}

/// Prepare all CPU bytes before claiming the GPU destination
bool obs_scene_upload_prepare(ObsGeometrySceneUploads *queue, uint64_t scene_id,
                              const uint32_t requested[OBS_GEOM_POOL_COUNT],
                              const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                              uint64_t *scene_ticket) {
    if (!queue || !queue->scenes.initialized || !scene_id || !requested ||
        !spans || !scene_ticket) return false;
    int slot = find_free(queue);
    if (slot < 0) return false;
    ObsGeometryBudget b = queue->scenes.pool_layout;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i].used = queue->scenes.highwater[i];
        b.pools[i].requested = requested[i];
    }
    ObsGeometryUploadBatch prepared = {0};
    if (!obs_geometry_upload_prepare_memory(spans, &b, &prepared)) return false;
    uint64_t flight_ticket = 0;
    if (!obs_geometry_flight_take(&queue->flights[slot], &prepared, &flight_ticket)) {
        (void)obs_geometry_upload_discard(&prepared);
        return false;
    }
    uint64_t ticket = 0;
    if (!obs_pending_scene_reserve(&queue->scenes, scene_id, requested, &ticket)) {
        (void)obs_geometry_flight_cancel(&queue->flights[slot], flight_ticket);
        return false;
    }
    ObsPendingScene snapshot;
    if (!obs_pending_scene_snapshot(&queue->scenes, ticket, &snapshot) ||
        !same_reservation(&snapshot.geometry, &queue->flights[slot].batch.plan.reservation)) {
        /* Impossible when both layers consume the same budget. Keep a hard
         * rejection before any GPU submission if future APIs diverge. */
        (void)obs_pending_scene_cancel(&queue->scenes, ticket);
        (void)obs_geometry_flight_cancel(&queue->flights[slot], flight_ticket);
        return false;
    }
    queue->scene_tickets[slot] = ticket;
    queue->flight_tickets[slot] = flight_ticket;
    *scene_ticket = ticket;
    return true;
}

/// Backend-accepted submission and fence observations
bool obs_scene_upload_submitted(ObsGeometrySceneUploads *queue, uint64_t ticket) {
    int slot = find_active(queue, ticket);
    if (slot < 0 || queue->flights[slot].state != OBS_FLIGHT_PREPARED) return false;
    if (!obs_geometry_flight_submitted(&queue->flights[slot], queue->flight_tickets[slot]))
        return false;
    return obs_pending_scene_submitted(&queue->scenes, ticket);
}

bool obs_scene_upload_observe(ObsGeometrySceneUploads *queue, uint64_t ticket,
                              ObsPendingSceneFence fence) {
    int slot = find_active(queue, ticket);
    if (slot < 0 || queue->flights[slot].state != OBS_FLIGHT_IN_FLIGHT ||
        (fence != OBS_SCENE_FENCE_PENDING && fence != OBS_SCENE_FENCE_SIGNALED &&
         fence != OBS_SCENE_FENCE_UNKNOWN)) return false;
    ObsGeometryFenceObservation converted =
        fence == OBS_SCENE_FENCE_SIGNALED ? OBS_FENCE_SIGNALED :
        fence == OBS_SCENE_FENCE_UNKNOWN ? OBS_FENCE_UNKNOWN : OBS_FENCE_PENDING;
    if (!obs_geometry_flight_observe(&queue->flights[slot],
                                     queue->flight_tickets[slot], converted)) return false;
    return obs_pending_scene_observe(&queue->scenes, ticket, fence);
}

/// Publication and failure retirement
bool obs_scene_upload_publish(ObsGeometrySceneUploads *queue, uint64_t ticket,
                              ObsGeometryReservation *out) {
    int slot = find_active(queue, ticket);
    if (slot < 0 || !out || queue->flights[slot].state != OBS_FLIGHT_SIGNALED)
        return false;
    ObsPendingScene scene;
    if (!obs_pending_scene_snapshot(&queue->scenes, ticket, &scene) ||
        scene.state != OBS_SCENE_READY ||
        !same_reservation(&scene.geometry, &queue->flights[slot].batch.plan.reservation))
        return false;
    ObsGeometryReservation confirmed;
    if (!obs_geometry_flight_publish(&queue->flights[slot],
                                     queue->flight_tickets[slot], &confirmed)) return false;
    if (!obs_pending_scene_publish(&queue->scenes, ticket, out)) return false;
    queue->scene_tickets[slot] = 0;
    queue->flight_tickets[slot] = 0;
    return true;
}

bool obs_scene_upload_cancel(ObsGeometrySceneUploads *queue, uint64_t ticket) {
    int slot = find_active(queue, ticket);
    ObsPendingScene scene;
    if (slot < 0 || queue->flights[slot].state != OBS_FLIGHT_PREPARED ||
        !obs_pending_scene_snapshot(&queue->scenes, ticket, &scene) ||
        scene.state != OBS_SCENE_PREPARED) return false;
    if (!obs_geometry_flight_cancel(&queue->flights[slot],
                                    queue->flight_tickets[slot])) return false;
    if (!obs_pending_scene_cancel(&queue->scenes, ticket)) return false;
    queue->scene_tickets[slot] = 0;
    queue->flight_tickets[slot] = 0;
    return true;
}

bool obs_scene_upload_retire_quiesced(ObsGeometrySceneUploads *queue,
                                     uint64_t ticket, bool gpu_quiesced) {
    int slot = find_active(queue, ticket);
    if (slot < 0 || !gpu_quiesced) return false;
    if (!obs_geometry_flight_recover_quiesced(&queue->flights[slot],
            queue->flight_tickets[slot], true)) return false;
    if (!obs_pending_scene_retire_quiesced(&queue->scenes, ticket, true)) return false;
    queue->scene_tickets[slot] = 0;
    queue->flight_tickets[slot] = 0;
    return true;
}
