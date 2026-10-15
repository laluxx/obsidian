/// geometry_scene_upload — retain prepared staging across pending scene loads
#pragma once

#include "geometry_pending.h"
#include "geometry_upload_flight.h"

/// Host staging + reservation ownership
/* One-flight-per-scene bounded journal. Vulkan objects are NOT owned by this
 * pure C module: the future async backend must keep staging GPU buffers,
 * fences and command buffers alive alongside these CPU-owned batches.
 * A backend must prove fence completion before calling observe(SIGNALED).
 */
typedef struct {
    ObsPendingScenes scenes;
    ObsGeometryFlight flights[OBS_PENDING_SCENES_MAX];
    uint64_t scene_tickets[OBS_PENDING_SCENES_MAX];
    uint64_t flight_tickets[OBS_PENDING_SCENES_MAX];
} ObsGeometrySceneUploads;

bool obs_scene_uploads_init(ObsGeometrySceneUploads *queue,
                            const ObsGeometryBudget *initial);
/* Prepare and own the eight CPU spans; no Vulkan is called. */
bool obs_scene_upload_prepare(ObsGeometrySceneUploads *queue, uint64_t scene_id,
                              const uint32_t requested[OBS_GEOM_POOL_COUNT],
                              const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                              uint64_t *scene_ticket);
/* Mark submitted ONLY after the backend has accepted the actual GPU work. */
bool obs_scene_upload_submitted(ObsGeometrySceneUploads *queue, uint64_t ticket);
bool obs_scene_upload_observe(ObsGeometrySceneUploads *queue, uint64_t ticket,
                              ObsPendingSceneFence fence);
/* Publishes reservation and retires prepared CPU bytes only after a signaled
 * fence; the renderer still owns scene visibility and GPU resource handles. */
bool obs_scene_upload_publish(ObsGeometrySceneUploads *queue, uint64_t ticket,
                              ObsGeometryReservation *out);
bool obs_scene_upload_cancel(ObsGeometrySceneUploads *queue, uint64_t ticket);
bool obs_scene_upload_retire_quiesced(ObsGeometrySceneUploads *queue,
                                     uint64_t ticket, bool gpu_quiesced);
