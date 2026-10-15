/// publication_metrics.h — Bounded, allocation-free checkpoint latency telemetry
#ifndef OBSIDIAN_PUBLICATION_METRICS_H
#define OBSIDIAN_PUBLICATION_METRICS_H

#include "publication_queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OBS_PUBLISH_METRIC_SAMPLES 128u
#define OBS_PUBLISH_BUDGET_DEFAULT_US UINT64_C(16667)

/// Per-lane latency, kept separate from compiler-only load timing
/* Duration measures CPU work at the owner-thread publication checkpoint,
 * including dispatch, host binding updates, and compiler calls. It does NOT
 * measure GPU frame time or guarantee the renderer remained responsive. */
typedef struct ObsPublishLatency {
    uint64_t duration_us[OBS_PUBLISH_METRIC_SAMPLES];
    uint64_t wait_us[OBS_PUBLISH_METRIC_SAMPLES];
    uint64_t attempts, succeeded, failed, over_budget;
    uint64_t last_duration_us, max_duration_us, last_wait_us;
    size_t count, next;
} ObsPublishLatency;

typedef struct ObsPublishMetrics {
    ObsPublishLatency all;
    ObsPublishLatency lane[OBS_PUBLISH_KIND_COUNT];
    uint64_t budget_us;
    uint64_t invalid_timestamps;
} ObsPublishMetrics;

void obs_publish_metrics_reset(ObsPublishMetrics *metrics, uint64_t budget_us);
/* Times come from ONE clock domain (CLOCK_MONOTONIC); enqueue time may be 0.
 * If timestamps regress, no latency sample is manufactured. */
bool obs_publish_metrics_record(ObsPublishMetrics *metrics, ObsPublishKind kind,
                                uint64_t queued_ns, uint64_t begin_ns,
                                uint64_t end_ns, bool success);
/* Nearest-rank percentile of the rolling 128 samples; 0 if empty. */
uint64_t obs_publish_latency_percentile(const ObsPublishLatency *lane,
                                        unsigned percentile, bool wait_time);

#endif
