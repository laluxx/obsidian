#include "geometry_async.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// Simulated Vulkan backend, deterministic and nonblocking
static struct {
    ObsAsyncSubmitResult submit;
    ObsAsyncPollResult fence;
    uint64_t live_ticket;
    unsigned starts, polls, retires;
    bool fail_retire;
} gpu;

static ObsAsyncSubmitResult start(void *ctx, uint64_t ticket,
                                  const ObsGeometryUploadBatch *batch) {
    (void)ctx;
    assert(batch->state == OBS_UPLOAD_PREPARED);
    assert(batch->plan.staging_bytes > 0 && batch->bytes);
    ++gpu.starts;
    if (gpu.submit != OBS_ASYNC_REJECTED) gpu.live_ticket = ticket;
    return gpu.submit;
}
static ObsAsyncPollResult poll_gpu(void *ctx, uint64_t ticket) {
    (void)ctx;
    assert(gpu.live_ticket == ticket);
    ++gpu.polls;
    return gpu.fence;
}
static bool retire(void *ctx, uint64_t ticket, bool quiesced) {
    (void)ctx;
    assert(quiesced && gpu.live_ticket == ticket);
    ++gpu.retires;
    if (gpu.fail_retire) return false;
    gpu.live_ticket = 0;
    return true;
}
static void init(ObsGeometryAsync *async) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i].used = 4;
        b.pools[i].element_bytes = i == OBS_GEOM_MESHLET_TRIANGLE ? 1 : 4;
        b.pools[i].capacity_bytes = 524288;
    }
    const ObsGeometryAsyncBackend backend = {
        .start = start, .poll = poll_gpu, .retire = retire,
    };
    assert(obs_geometry_async_init(async, &b, &backend));
}
static bool prepare(ObsGeometryAsync *async, uint64_t scene,
                    uint64_t *ticket) {
    static const unsigned char data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t counts[OBS_GEOM_POOL_COUNT];
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        counts[i] = 2;
        spans[i] = (ObsGeometrySourceSpan){data,
            i == OBS_GEOM_MESHLET_TRIANGLE ? 2u : 8u};
    }
    return obs_geometry_async_prepare(async, scene, counts, spans, ticket);
}

/// Tests: no early publication, rejected, accepted, unknown, stale tickets
static void lifecycle(void) {
    ObsGeometryAsync a = {0}; init(&a);
    assert(!obs_geometry_async_init(&a, &a.uploads.scenes.pool_layout,
                                    &a.backend));
    uint64_t first = 0, second = 0;
    assert(prepare(&a, 5, &first) && first != 0);
    assert(obs_geometry_async_busy(&a));
    assert(!prepare(&a, 6, &second));
    assert(!obs_geometry_async_submit(&a, first + 100));
    gpu.submit = OBS_ASYNC_REJECTED;
    assert(!obs_geometry_async_submit(&a, first));
    assert(gpu.live_ticket == 0);
    assert(obs_geometry_async_cancel_prepared(&a, first));
    assert(!obs_geometry_async_busy(&a));
    assert(prepare(&a, 6, &second) && second > first);
    assert(!obs_geometry_async_cancel_prepared(&a, first));
    gpu.submit = OBS_ASYNC_ACCEPTED;
    gpu.fence = OBS_ASYNC_PENDING;
    assert(obs_geometry_async_submit(&a, second));
    assert(obs_geometry_async_poll(&a, second) == OBS_ASYNC_IN_FLIGHT);
    ObsGeometryReservation out = { .base = { 999 } };
    assert(!obs_geometry_async_commit(&a, second, &out));
    assert(out.base[0] == 999);
    assert(!obs_geometry_async_cancel_prepared(&a, second));
    gpu.fence = OBS_ASYNC_SIGNALED;
    assert(obs_geometry_async_poll(&a, second) == OBS_ASYNC_READY);
    assert(obs_geometry_async_poll(&a, second) == OBS_ASYNC_READY);
    assert(obs_geometry_async_commit(&a, second, &out));
    assert(!obs_geometry_async_busy(&a));
    assert(out.base[0] == 4 && out.end[0] == 6);
    assert(gpu.live_ticket == 0);
    assert(!obs_geometry_async_commit(&a, second, &out));
    ObsGeometryBudget refreshed = a.uploads.scenes.pool_layout;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        refreshed.pools[i].used = a.uploads.scenes.published[i];
    assert(obs_geometry_async_sync_idle(&a, &refreshed));
    ObsGeometryBudget invalid = refreshed;
    invalid.pools[0].element_bytes = 8;
    assert(!obs_geometry_async_sync_idle(&a, &invalid));
    invalid = refreshed;
    invalid.pools[0].used = 0;
    assert(!obs_geometry_async_sync_idle(&a, &invalid));

    uint64_t third = 0;
    assert(prepare(&a, 7, &third) && third > second);
    gpu.submit = OBS_ASYNC_ACCEPTED;
    gpu.fence = OBS_ASYNC_UNCERTAIN;
    assert(obs_geometry_async_submit(&a, third));
    assert(obs_geometry_async_poll(&a, third) == OBS_ASYNC_QUARANTINED);
    assert(!obs_geometry_async_commit(&a, third, &out));
    assert(!obs_geometry_async_retire_quiesced(&a, third, false));
    assert(obs_geometry_async_retire_quiesced(&a, third, true));
    assert(!obs_geometry_async_busy(&a));
    assert(out.base[0] == 4);

    uint64_t discarded = 0;
    assert(prepare(&a, 99, &discarded));
    gpu.submit = OBS_ASYNC_ACCEPTED;
    gpu.fence = OBS_ASYNC_SIGNALED;
    assert(obs_geometry_async_submit(&a, discarded));
    assert(obs_geometry_async_poll(&a, discarded) == OBS_ASYNC_READY);
    assert(obs_geometry_async_discard_ready(&a, discarded));
    assert(!obs_geometry_async_busy(&a));
    assert(gpu.live_ticket == 0);
    assert(!obs_geometry_async_discard_ready(&a, discarded));

    uint64_t fourth = 0;
    assert(prepare(&a, 8, &fourth));
    gpu.submit = OBS_ASYNC_UNKNOWN;
    assert(!obs_geometry_async_submit(&a, fourth));
    assert(a.state == OBS_ASYNC_QUARANTINED);
    assert(!obs_geometry_async_cancel_prepared(&a, fourth));
    assert(obs_geometry_async_retire_quiesced(&a, fourth, true));

    /* Shutdown after an accepted but still-pending transfer must keep ownership
       until the caller provides a real device-idle proof. */
    uint64_t shutdown_ticket = 0;
    assert(prepare(&a, 9, &shutdown_ticket));
    gpu.submit = OBS_ASYNC_ACCEPTED;
    gpu.fence = OBS_ASYNC_PENDING;
    assert(obs_geometry_async_submit(&a, shutdown_ticket));
    assert(obs_geometry_async_retire_quiesced(&a, shutdown_ticket, true));
    assert(!obs_geometry_async_busy(&a));
    assert(gpu.live_ticket == 0);

    /* A prepared-but-not-submitted batch has no backend GPU ownership. */
    uint64_t prepared_ticket = 0;
    assert(prepare(&a, 10, &prepared_ticket));
    assert(obs_geometry_async_retire_quiesced(&a, prepared_ticket, true));
    assert(!obs_geometry_async_busy(&a));
}

/// Stress: many GPU polls are allowed without waiting or modifying cursors
static void stress(void) {
    ObsGeometryAsync a = {0}; init(&a);
    uint64_t seed = UINT64_C(0x863d117b20e195b3);
    for (unsigned i = 0; i < 50000; ++i) {
        uint64_t ticket = 0;
        assert(prepare(&a, (uint64_t)i + 1, &ticket));
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        if ((seed & 3u) == 0) {
            assert(obs_geometry_async_cancel_prepared(&a, ticket));
            continue;
        }
        gpu.submit = OBS_ASYNC_ACCEPTED;
        gpu.fence = OBS_ASYNC_PENDING;
        assert(obs_geometry_async_submit(&a, ticket));
        for (unsigned j = 0; j < 3; ++j)
            assert(obs_geometry_async_poll(&a, ticket) == OBS_ASYNC_IN_FLIGHT);
        if ((seed & 7u) == 1) {
            gpu.fence = OBS_ASYNC_UNCERTAIN;
            assert(obs_geometry_async_poll(&a, ticket) == OBS_ASYNC_QUARANTINED);
            assert(obs_geometry_async_retire_quiesced(&a, ticket, true));
        } else {
            gpu.fence = OBS_ASYNC_SIGNALED;
            assert(obs_geometry_async_poll(&a, ticket) == OBS_ASYNC_READY);
            ObsGeometryReservation r;
            assert(obs_geometry_async_commit(&a, ticket, &r));
        }
        assert(!obs_geometry_async_busy(&a));
    }
    puts("geometry-async: 50,000 controlled GPU handoffs passed");
}
int main(void) { lifecycle(); stress(); }
