/// dispatch_metrics.h — Allocation-free CPU profiling of embedded Monad hot paths
#ifndef OBSIDIAN_MONAD_DISPATCH_METRICS_H
#define OBSIDIAN_MONAD_DISPATCH_METRICS_H

#include <stdbool.h>
#include <stdint.h>

/// Observed dispatch paths and phases
/* No compiler/JIT/GPU measurements are implied by these owner-thread CPU
 * counters. Histograms are logarithmic in nanoseconds: percentile results
 * are conservative upper bounds, NOT exact percentile sample values. */
typedef enum ObsDispatchLane {
    OBS_DISPATCH_FRAME = 0,
    OBS_DISPATCH_CAMERA = 1,
    OBS_DISPATCH_LANE_COUNT = 2
} ObsDispatchLane;
typedef enum ObsDispatchPhase {
    OBS_DISPATCH_SETUP = 0,
    OBS_DISPATCH_EXECUTE = 1,
    OBS_DISPATCH_TOTAL = 2,
    OBS_DISPATCH_PHASE_COUNT = 3
} ObsDispatchPhase;
#define OBS_DISPATCH_BUCKETS 65u

typedef struct ObsDispatchHistogram {
    uint64_t buckets[OBS_DISPATCH_BUCKETS]; /* zero + log2(ns) buckets */
    uint64_t samples;
    uint64_t max_ns;
    uint64_t last_ns;
} ObsDispatchHistogram;
typedef struct ObsDispatchLaneStats {
    ObsDispatchHistogram phases[OBS_DISPATCH_PHASE_COUNT];
    uint64_t failures;
} ObsDispatchLaneStats;
typedef struct ObsDispatchMetrics {
    ObsDispatchLaneStats lanes[OBS_DISPATCH_LANE_COUNT];
    bool enabled;
} ObsDispatchMetrics;

/// Owner-thread metrics API
void obs_dispatch_metrics_reset(bool enable);
bool obs_dispatch_metrics_enabled(void);
uint64_t obs_dispatch_clock_ns(void);
void obs_dispatch_metrics_record(ObsDispatchLane lane, uint64_t setup_ns,
                                 uint64_t execute_ns, uint64_t total_ns,
                                 bool succeeded);
void obs_dispatch_metrics_snapshot(ObsDispatchMetrics *out);
uint64_t obs_dispatch_histogram_percentile_ns(const ObsDispatchHistogram *histogram,
                                               unsigned percentile);
#endif
