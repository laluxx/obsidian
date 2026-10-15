/// publication_metrics.c — Bounded, no-allocation publication-stall measurement
#include "publication_metrics.h"
#include <string.h>

/// Clock conversion and safe counters
static uint64_t elapsed_us(uint64_t begin_ns, uint64_t end_ns) {
    return (end_ns - begin_ns) / UINT64_C(1000);
}

void obs_publish_metrics_reset(ObsPublishMetrics *metrics, uint64_t budget_us) {
    if (!metrics) return;
    memset(metrics, 0, sizeof(*metrics));
    metrics->budget_us = budget_us ? budget_us : OBS_PUBLISH_BUDGET_DEFAULT_US;
}

static void record(ObsPublishLatency *lane, uint64_t duration, uint64_t wait,
                   uint64_t budget_us, bool success) {
    ++lane->attempts;
    if (success) ++lane->succeeded;
    else ++lane->failed;
    if (duration > budget_us) ++lane->over_budget;
    lane->last_duration_us = duration;
    lane->last_wait_us = wait;
    if (duration > lane->max_duration_us) lane->max_duration_us = duration;
    lane->duration_us[lane->next] = duration;
    lane->wait_us[lane->next] = wait;
    lane->next = (lane->next + 1) % OBS_PUBLISH_METRIC_SAMPLES;
    if (lane->count < OBS_PUBLISH_METRIC_SAMPLES) ++lane->count;
}

/// Checked owner-thread checkpoint sample
bool obs_publish_metrics_record(ObsPublishMetrics *metrics, ObsPublishKind kind,
                                uint64_t queued_ns, uint64_t begin_ns,
                                uint64_t end_ns, bool success) {
    if (!metrics || (kind != OBS_PUBLISH_BOOT && kind != OBS_PUBLISH_REPL) ||
        !begin_ns ||
        end_ns < begin_ns || (queued_ns && queued_ns > begin_ns)) {
        if (metrics) ++metrics->invalid_timestamps;
        return false;
    }
    uint64_t duration = elapsed_us(begin_ns, end_ns);
    uint64_t wait = queued_ns ? elapsed_us(queued_ns, begin_ns) : 0;
    record(&metrics->all, duration, wait, metrics->budget_us, success);
    record(&metrics->lane[kind], duration, wait, metrics->budget_us, success);
    return true;
}

uint64_t obs_publish_latency_percentile(const ObsPublishLatency *lane,
                                        unsigned percentile, bool wait_time) {
    if (!lane || !lane->count || lane->count > OBS_PUBLISH_METRIC_SAMPLES) return 0;
    uint64_t sorted[OBS_PUBLISH_METRIC_SAMPLES];
    for (size_t i = 0; i < lane->count; ++i)
        sorted[i] = wait_time ? lane->wait_us[i] : lane->duration_us[i];
    /* Outside the hot path; a bounded insertion sort is cheap to inspect. */
    for (size_t i = 1; i < lane->count; ++i) {
        uint64_t value = sorted[i];
        size_t j = i;
        while (j && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    if (percentile > 100) percentile = 100;
    if (!percentile) return sorted[0];
    size_t index = ((size_t)percentile * lane->count + 99u) / 100u;
    return sorted[index ? index - 1 : 0];
}
