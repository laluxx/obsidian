#define _POSIX_C_SOURCE 200809L
/// dispatch_metrics.c — Constant-cost sample recording for hot foreign calls
#include "dispatch_metrics.h"
#include <limits.h>
#include <string.h>
#include <time.h>

/// Owned host instrumentation
/* The engine's Monad runtime is owner-thread-affine. Collection is likewise
 * owner-thread-only, avoiding atomics, allocation and scheduler contention. */
static ObsDispatchMetrics s_metrics;

static uint64_t saturating_add_one(uint64_t value) {
    return value == UINT64_MAX ? value : value + 1;
}
static unsigned histogram_bucket(uint64_t nanoseconds) {
    if (!nanoseconds) return 0;
#if defined(__GNUC__) || defined(__clang__)
    return 1u + (unsigned)(63u - (unsigned)__builtin_clzll(nanoseconds));
#else
    unsigned bucket = 1;
    while (nanoseconds >>= 1) ++bucket;
    return bucket;
#endif
}
static void histogram_record(ObsDispatchHistogram *histogram, uint64_t ns) {
    unsigned bucket = histogram_bucket(ns);
    histogram->buckets[bucket] = saturating_add_one(histogram->buckets[bucket]);
    histogram->samples = saturating_add_one(histogram->samples);
    histogram->last_ns = ns;
    if (histogram->max_ns < ns) histogram->max_ns = ns;
}

/// Readout and stable percentile bounds
void obs_dispatch_metrics_reset(bool enable) {
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.enabled = enable;
}
bool obs_dispatch_metrics_enabled(void) { return s_metrics.enabled; }
uint64_t obs_dispatch_clock_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
void obs_dispatch_metrics_record(ObsDispatchLane lane, uint64_t setup_ns,
                                 uint64_t execute_ns, uint64_t total_ns,
                                 bool succeeded) {
    if (!s_metrics.enabled || (unsigned)lane >= OBS_DISPATCH_LANE_COUNT) return;
    ObsDispatchLaneStats *stat = &s_metrics.lanes[lane];
    histogram_record(&stat->phases[OBS_DISPATCH_SETUP], setup_ns);
    histogram_record(&stat->phases[OBS_DISPATCH_EXECUTE], execute_ns);
    histogram_record(&stat->phases[OBS_DISPATCH_TOTAL], total_ns);
    if (!succeeded) stat->failures = saturating_add_one(stat->failures);
}
void obs_dispatch_metrics_snapshot(ObsDispatchMetrics *out) {
    if (out) *out = s_metrics;
}
uint64_t obs_dispatch_histogram_percentile_ns(const ObsDispatchHistogram *h,
                                               unsigned percentile) {
    if (!h || !h->samples || percentile < 1 || percentile > 100) return 0;
    /* ceil(samples * percentage / 100) without overflow. */
    uint64_t rank = (h->samples / 100) * percentile +
                    ((h->samples % 100) * percentile + 99) / 100;
    if (!rank) rank = 1;
    uint64_t seen = 0;
    for (unsigned b = 0; b < OBS_DISPATCH_BUCKETS; ++b) {
        if (UINT64_MAX - seen < h->buckets[b]) seen = UINT64_MAX;
        else seen += h->buckets[b];
        if (seen < rank) continue;
        if (!b) return 0;
        /* Bucket b contains [2^(b-1), 2^b-1] ns. Upper-bound estimate. */
        if (b == 64) return UINT64_MAX;
        return (UINT64_C(1) << b) - 1;
    }
    return h->max_ns;
}
