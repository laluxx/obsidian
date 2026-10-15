/// publication_metrics.c — deterministic owner-thread latency tests
#include "publication_metrics.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/// Clock and admission contracts
static void contracts(void) {
    ObsPublishMetrics m = {0};
    obs_publish_metrics_reset(&m, 5000);
    assert(m.budget_us == 5000);
    assert(!obs_publish_metrics_record(&m, OBS_PUBLISH_BOOT, 0, 0, 1, true));
    assert(!obs_publish_metrics_record(&m, OBS_PUBLISH_BOOT, 0, 2000, 1000, true));
    assert(!obs_publish_metrics_record(&m, OBS_PUBLISH_BOOT, 3000, 2000, 4000, true));
    assert(!obs_publish_metrics_record(&m, OBS_PUBLISH_KIND_COUNT, 0, 2000, 4000, true));
    assert(!obs_publish_metrics_record(&m, (ObsPublishKind)-1, 0, 2000, 4000, true));
    assert(m.invalid_timestamps == 5 && m.all.attempts == 0);
    assert(obs_publish_metrics_record(&m, OBS_PUBLISH_BOOT,
                                      UINT64_C(1000000), UINT64_C(3000000),
                                      UINT64_C(11000000), true));
    assert(m.all.attempts == 1 && m.all.over_budget == 1);
    assert(m.all.last_duration_us == 8000 && m.all.last_wait_us == 2000);
    assert(m.lane[OBS_PUBLISH_BOOT].succeeded == 1);
    assert(obs_publish_metrics_record(&m, OBS_PUBLISH_REPL,
                                      0, UINT64_C(15000000),
                                      UINT64_C(16000000), false));
    assert(m.all.failed == 1 && m.all.last_wait_us == 0);
    assert(m.lane[OBS_PUBLISH_BOOT].attempts == 1);
    assert(m.lane[OBS_PUBLISH_REPL].attempts == 1);
    assert(obs_publish_latency_percentile(&m.all, 50, false) == 1000);
    assert(obs_publish_latency_percentile(&m.all, 95, false) == 8000);
    assert(obs_publish_latency_percentile(&m.all, 95, true) == 2000);
    assert(obs_publish_latency_percentile(NULL, 95, true) == 0);
    obs_publish_metrics_reset(&m, 0);
    assert(m.budget_us == OBS_PUBLISH_BUDGET_DEFAULT_US && !m.all.count);
}

/// Deterministic rolling overwrite and queue wait tracking
static void stress(void) {
    ObsPublishMetrics m;
    obs_publish_metrics_reset(&m, 1000);
    ObsPublishQueue queue;
    obs_publish_queue_reset(&queue);
    for (uint64_t i = 0; i < 50000; ++i) {
        ObsPublishKind kind = i % 2 ? OBS_PUBLISH_BOOT : OBS_PUBLISH_REPL;
        uint64_t enqueued = UINT64_C(100000000) + i * UINT64_C(10000000);
        assert(obs_publish_queue_request_at(&queue, kind, enqueued));
        if (kind == OBS_PUBLISH_REPL)
            assert(obs_publish_queue_request_at(&queue, kind, enqueued + 1000));
        ObsPublishTicket ticket;
        assert(obs_publish_queue_begin(&queue, &ticket));
        assert(ticket.kind == kind);
        assert(queue.active_since_ns == enqueued);
        uint64_t started = enqueued + UINT64_C(2000000);
        uint64_t ended = started + (i % 100 + 1) * UINT64_C(100000);
        assert(obs_publish_metrics_record(&m, kind, queue.active_since_ns,
                                          started, ended, true));
        assert(obs_publish_queue_finish(&queue, ticket, true));
        assert(queue.active_since_ns == 0);
    }
    assert(m.all.attempts == 50000);
    assert(m.all.count == OBS_PUBLISH_METRIC_SAMPLES);
    assert(m.lane[OBS_PUBLISH_BOOT].count == OBS_PUBLISH_METRIC_SAMPLES);
    assert(m.lane[OBS_PUBLISH_REPL].count == OBS_PUBLISH_METRIC_SAMPLES);
    assert(m.all.failed == 0);
    assert(m.all.over_budget > 0);
    assert(obs_publish_latency_percentile(&m.all, 99, false) >= 9000);
    assert(obs_publish_latency_percentile(&m.all, 95, true) == 2000);
    assert(obs_publish_latency_percentile(&m.all, 100, false) == 10000);
    assert(m.invalid_timestamps == 0);
}
int main(void) {
    contracts(); stress();
    puts("publication_metrics: duration/wait/stall/p99/50K checkpoints OK");
    return 0;
}
