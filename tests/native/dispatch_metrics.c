/// dispatch_metrics.c — CPU foreign-dispatch histogram regressions
#include "dispatch_metrics.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/// Boundary and deterministic replay tests
int main(void) {
    ObsDispatchMetrics snap = {0};
    obs_dispatch_metrics_reset(false);
    obs_dispatch_metrics_record(OBS_DISPATCH_FRAME, 10, 20, 30, false);
    obs_dispatch_metrics_snapshot(&snap);
    assert(!snap.enabled && !snap.lanes[0].phases[0].samples);
    obs_dispatch_metrics_reset(true);
    assert(obs_dispatch_metrics_enabled());
    obs_dispatch_metrics_record((ObsDispatchLane)999, 10, 20, 30, false);
    assert(obs_dispatch_histogram_percentile_ns(NULL, 95) == 0);
    assert(obs_dispatch_histogram_percentile_ns(&snap.lanes[0].phases[0], 95) == 0);
    obs_dispatch_metrics_record(OBS_DISPATCH_FRAME, 0, 1, 2, true);
    obs_dispatch_metrics_record(OBS_DISPATCH_FRAME, UINT64_MAX, 8, UINT64_MAX, false);
    obs_dispatch_metrics_snapshot(&snap);
    assert(snap.lanes[OBS_DISPATCH_FRAME].failures == 1);
    assert(snap.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_SETUP].buckets[0] == 1);
    assert(snap.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_SETUP].buckets[64] == 1);
    assert(obs_dispatch_histogram_percentile_ns(&snap.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_SETUP], 50) == 0);
    assert(obs_dispatch_histogram_percentile_ns(&snap.lanes[OBS_DISPATCH_FRAME].phases[OBS_DISPATCH_SETUP], 95) == UINT64_MAX);
    obs_dispatch_metrics_reset(true);
    uint64_t last = obs_dispatch_clock_ns();
    for (uint64_t i = 0; i < 50000; ++i) {
        uint64_t v = i % 1024;
        obs_dispatch_metrics_record(i & 1 ? OBS_DISPATCH_FRAME : OBS_DISPATCH_CAMERA,
                                    v, v * 2, v * 3, i % 7 != 0);
    }
    uint64_t now = obs_dispatch_clock_ns();
    assert(now >= last || !now || !last);
    obs_dispatch_metrics_snapshot(&snap);
    for (unsigned lane = 0; lane < OBS_DISPATCH_LANE_COUNT; ++lane) {
        const ObsDispatchLaneStats *s = &snap.lanes[lane];
        for (unsigned phase = 0; phase < OBS_DISPATCH_PHASE_COUNT; ++phase) {
            const ObsDispatchHistogram *h = &s->phases[phase];
            uint64_t sum = 0;
            for (unsigned b = 0; b < OBS_DISPATCH_BUCKETS; ++b) sum += h->buckets[b];
            assert(sum == 25000 && h->samples == 25000);
            assert(h->max_ns >= h->last_ns);
            uint64_t p50 = obs_dispatch_histogram_percentile_ns(h, 50);
            uint64_t p95 = obs_dispatch_histogram_percentile_ns(h, 95);
            uint64_t p99 = obs_dispatch_histogram_percentile_ns(h, 99);
            assert(p50 <= p95 && p95 <= p99);
            assert(obs_dispatch_histogram_percentile_ns(h, 0) == 0);
            assert(obs_dispatch_histogram_percentile_ns(h, 101) == 0);
        }
        assert(s->failures > 0);
    }
    obs_dispatch_metrics_reset(false);
    obs_dispatch_metrics_snapshot(&snap);
    assert(!snap.enabled && snap.lanes[0].failures == 0);
    puts("dispatch metrics: 50,000 replay samples and boundary cases passed");
    return 0;
}
