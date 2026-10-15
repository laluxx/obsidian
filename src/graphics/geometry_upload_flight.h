/// geometry_upload_flight — single-owner GPU completion protocol
#pragma once

#include "geometry_upload_batch.h"
#include <stdbool.h>
#include <stdint.h>

/// Explicit lifecycle
/* This module owns CPU-side prepared staging bytes and reservation metadata;
 * the graphics backend separately owns Vulkan handles/fences. A ticket is
 * valid only for its own generation. No GPU-owned object may be released on an
 * indeterminate fence result. All entry points are owner-thread operations.
 *
 * State transitions:
 *   EMPTY -> PREPARED -> IN_FLIGHT -> SIGNALED -> EMPTY (publish)
 *                       \-> UNKNOWN -> EMPTY (only after device quiescence)
 *   PREPARED -> EMPTY (cancel before submission)
 */
typedef enum {
    OBS_FLIGHT_EMPTY = 0,
    OBS_FLIGHT_PREPARED,
    OBS_FLIGHT_IN_FLIGHT,
    OBS_FLIGHT_SIGNALED,
    OBS_FLIGHT_UNKNOWN
} ObsGeometryFlightState;

typedef enum {
    OBS_FENCE_PENDING = 0,
    OBS_FENCE_SIGNALED,
    OBS_FENCE_UNKNOWN
} ObsGeometryFenceObservation;

typedef struct {
    ObsGeometryUploadBatch batch;
    ObsGeometryFlightState state;
    uint64_t ticket;
    uint64_t serial;
} ObsGeometryFlight;

/// Allocation-free ownership handoff
/* The caller's batch is cleared ONLY on success. Allocation failures must be
 * handled before handing it over. Ticket zero is never issued. */
bool obs_geometry_flight_take(ObsGeometryFlight *flight,
                              ObsGeometryUploadBatch *prepared,
                              uint64_t *ticket);
bool obs_geometry_flight_submitted(ObsGeometryFlight *flight, uint64_t ticket);
bool obs_geometry_flight_observe(ObsGeometryFlight *flight, uint64_t ticket,
                                  ObsGeometryFenceObservation observation);
/* Publish is the only operation that releases a successfully submitted batch.
 * The output reservation remains unchanged on failure. */
bool obs_geometry_flight_publish(ObsGeometryFlight *flight, uint64_t ticket,
                                  ObsGeometryReservation *out);
/* Cancellation is legal only before the backend actually submits GPU work. */
bool obs_geometry_flight_cancel(ObsGeometryFlight *flight, uint64_t ticket);
/* A prior backend fence/queue/device-idle proof is REQUIRED. This function
 * cannot establish GPU quiescence. A PREPARED flight is also accepted in case
 * GPU submit succeeded but the host failed to record that transition.
 * Never call merely on timeout. */
bool obs_geometry_flight_recover_quiesced(ObsGeometryFlight *flight,
                                         uint64_t ticket, bool gpu_quiesced);
