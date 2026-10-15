#include "geometry_upload_flight.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/// Real prepared input
static ObsGeometryUploadBatch make_batch(void) {
    ObsGeometryBudget b = {0};
    unsigned char payload[16] = { 1,2,3,4,5,6,7,8 };
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i] = (ObsGeometryPoolBudget){
            .element_bytes = 4, .used = 4, .requested = 1,
            .capacity_bytes = 128
        };
        spans[i] = (ObsGeometrySourceSpan){ payload, 4 };
    }
    ObsGeometryUploadBatch batch = {0};
    assert(obs_geometry_upload_prepare_memory(spans, &b, &batch));
    return batch;
}

/// Publication must await a real completion proof
static void complete(void) {
    ObsGeometryFlight f = {0};
    ObsGeometryUploadBatch b = make_batch();
    uint64_t token = 0;
    assert(obs_geometry_flight_take(&f, &b, &token) && token == 1);
    assert(b.state == OBS_UPLOAD_EMPTY && !b.bytes);
    assert(!obs_geometry_flight_take(&f, &b, &token));
    ObsGeometryReservation untouched = { .base = { 123 } };
    assert(!obs_geometry_flight_publish(&f, token, &untouched));
    assert(untouched.base[0] == 123);
    assert(obs_geometry_flight_submitted(&f, token));
    assert(!obs_geometry_flight_cancel(&f, token));
    assert(obs_geometry_flight_observe(&f, token, OBS_FENCE_PENDING));
    assert(!obs_geometry_flight_publish(&f, token, &untouched));
    assert(!obs_geometry_flight_observe(&f, token, (ObsGeometryFenceObservation)99));
    assert(obs_geometry_flight_observe(&f, token, OBS_FENCE_SIGNALED));
    assert(!obs_geometry_flight_observe(&f, token, OBS_FENCE_SIGNALED));
    assert(obs_geometry_flight_publish(&f, token, &untouched));
    assert(untouched.base[0] == 4);
    assert(f.state == OBS_FLIGHT_EMPTY);
    assert(!obs_geometry_flight_submitted(&f, token));
    b = make_batch();
    uint64_t newer = 0;
    assert(obs_geometry_flight_take(&f, &b, &newer) && newer > token);
    assert(!obs_geometry_flight_cancel(&f, token));
    assert(obs_geometry_flight_cancel(&f, newer));
}

/// Unknown submission is never reclaimed without device quiescence
static void quarantine(void) {
    ObsGeometryFlight f = {0};
    ObsGeometryUploadBatch b = make_batch();
    uint64_t ticket = 0;
    assert(obs_geometry_flight_take(&f, &b, &ticket));
    assert(obs_geometry_flight_submitted(&f, ticket));
    assert(obs_geometry_flight_observe(&f, ticket, OBS_FENCE_UNKNOWN));
    assert(f.state == OBS_FLIGHT_UNKNOWN);
    assert(!obs_geometry_flight_recover_quiesced(&f, ticket, false));
    assert(!obs_geometry_flight_publish(&f, ticket, &(ObsGeometryReservation){0}));
    assert(!obs_geometry_flight_cancel(&f, ticket));
    assert(obs_geometry_flight_recover_quiesced(&f, ticket, true));
    assert(f.state == OBS_FLIGHT_EMPTY);
    /* Defensive recovery when backend submission succeeded but recording its
       state failed: GPU quiescence, not journal state, licenses cleanup. */
    b = make_batch();
    assert(obs_geometry_flight_take(&f, &b, &ticket));
    assert(!obs_geometry_flight_recover_quiesced(&f, ticket, false));
    assert(obs_geometry_flight_recover_quiesced(&f, ticket, true));
    b = make_batch();
    assert(obs_geometry_flight_take(&f, &b, &ticket));
    assert(obs_geometry_flight_submitted(&f, ticket));
    assert(obs_geometry_flight_recover_quiesced(&f, ticket, true));
}

/// Adversarial repeated lifetime changes
static void stress(void) {
    ObsGeometryFlight f = {0};
    uint64_t x = UINT64_C(0x4d433234fabc89f1);
    for (unsigned n = 0; n < 50000; ++n) {
        ObsGeometryUploadBatch b = make_batch();
        uint64_t ticket = 0;
        assert(obs_geometry_flight_take(&f, &b, &ticket));
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if ((x & 3) == 0) {
            assert(obs_geometry_flight_cancel(&f, ticket));
            continue;
        }
        assert(obs_geometry_flight_submitted(&f, ticket));
        if ((x & 3) == 1) {
            assert(obs_geometry_flight_observe(&f, ticket, OBS_FENCE_UNKNOWN));
            assert(obs_geometry_flight_recover_quiesced(&f, ticket, true));
        } else {
            assert(obs_geometry_flight_observe(&f, ticket, OBS_FENCE_PENDING));
            assert(obs_geometry_flight_observe(&f, ticket, OBS_FENCE_SIGNALED));
            ObsGeometryReservation r = {0};
            assert(obs_geometry_flight_publish(&f, ticket, &r));
            assert(r.end[0] > r.base[0]);
        }
        assert(!obs_geometry_flight_cancel(&f, ticket));
    }
    ObsGeometryUploadBatch b = make_batch();
    f.serial = UINT64_MAX;
    assert(!obs_geometry_flight_take(&f, &b, &(uint64_t){0}));
    assert(obs_geometry_upload_discard(&b));
    puts("geometry-upload-flight: 50,000 state/lifetime cases passed");
}

int main(void) {
    complete(); quarantine(); stress();
    return 0;
}
