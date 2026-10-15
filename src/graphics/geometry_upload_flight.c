#include "geometry_upload_flight.h"
#include <limits.h>
#include <string.h>

/// Generation validation
static bool current(const ObsGeometryFlight *f, uint64_t ticket) {
    return f && ticket != 0 && f->ticket == ticket &&
           f->state != OBS_FLIGHT_EMPTY;
}

/// Prepared batch ownership
bool obs_geometry_flight_take(ObsGeometryFlight *f,
                              ObsGeometryUploadBatch *prepared,
                              uint64_t *ticket) {
    if (!f || !prepared || !ticket || f->state != OBS_FLIGHT_EMPTY ||
        prepared->state != OBS_UPLOAD_PREPARED || f->serial == UINT64_MAX)
        return false;
    uint64_t next = f->serial + 1;
    if (!next) return false;
    f->batch = *prepared;
    memset(prepared, 0, sizeof(*prepared));
    f->serial = f->ticket = next;
    f->state = OBS_FLIGHT_PREPARED;
    *ticket = next;
    return true;
}

bool obs_geometry_flight_submitted(ObsGeometryFlight *f, uint64_t ticket) {
    if (!current(f, ticket) || f->state != OBS_FLIGHT_PREPARED ||
        !obs_geometry_upload_submit(&f->batch)) return false;
    f->state = OBS_FLIGHT_IN_FLIGHT;
    return true;
}

/// Fence polling
bool obs_geometry_flight_observe(ObsGeometryFlight *f, uint64_t ticket,
                                  ObsGeometryFenceObservation observation) {
    if (!current(f, ticket) || f->state != OBS_FLIGHT_IN_FLIGHT) return false;
    switch (observation) {
    case OBS_FENCE_PENDING:
        return true;
    case OBS_FENCE_SIGNALED:
        if (!obs_geometry_upload_retire(&f->batch)) return false;
        f->state = OBS_FLIGHT_SIGNALED;
        return true;
    case OBS_FENCE_UNKNOWN:
        f->state = OBS_FLIGHT_UNKNOWN;
        return true;
    default:
        return false;
    }
}

/// Publication and cancellation
static void clear_flight(ObsGeometryFlight *f) {
    /* Retain serial to keep stale tickets invalid after reuse. */
    uint64_t serial = f->serial;
    memset(f, 0, sizeof(*f));
    f->serial = serial;
}

bool obs_geometry_flight_publish(ObsGeometryFlight *f, uint64_t ticket,
                                  ObsGeometryReservation *out) {
    if (!out || !current(f, ticket) || f->state != OBS_FLIGHT_SIGNALED) return false;
    ObsGeometryReservation next = f->batch.plan.reservation;
    if (!obs_geometry_upload_discard(&f->batch)) return false;
    *out = next;
    clear_flight(f);
    return true;
}

bool obs_geometry_flight_cancel(ObsGeometryFlight *f, uint64_t ticket) {
    if (!current(f, ticket) || f->state != OBS_FLIGHT_PREPARED ||
        !obs_geometry_upload_discard(&f->batch)) return false;
    clear_flight(f);
    return true;
}

bool obs_geometry_flight_recover_quiesced(ObsGeometryFlight *f,
                                         uint64_t ticket, bool gpu_quiesced) {
    if (!gpu_quiesced || !current(f, ticket) ||
        (f->state != OBS_FLIGHT_PREPARED && f->state != OBS_FLIGHT_IN_FLIGHT &&
         f->state != OBS_FLIGHT_UNKNOWN && f->state != OBS_FLIGHT_SIGNALED)) return false;
    if (f->batch.state == OBS_UPLOAD_SUBMITTED &&
        !obs_geometry_upload_retire(&f->batch)) return false;
    if (!obs_geometry_upload_discard(&f->batch)) return false;
    clear_flight(f);
    return true;
}
