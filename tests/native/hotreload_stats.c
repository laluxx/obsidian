/// HotReloadStatsTests — bounded rolling latency percentiles
#include "hotreload_stats.h"
#include <assert.h>
#include <stdio.h>

/// Correctness
int main(void) {
    ObsReloadStats s = {0};
    assert(obs_reload_stats_percentile(&s, 50) == 0);
    assert(obs_reload_stats_percentile(NULL, 50) == 0);
    obs_reload_stats_record(NULL, 200, false);
    obs_reload_stats_record(&s, 200, true);
    assert(s.count == 1 && s.published == 1 && s.attempts == 1);
    assert(obs_reload_stats_percentile(&s, 1) == 200);
    assert(obs_reload_stats_percentile(&s, 100) == 200);
    for (unsigned i = 1; i <= OBS_RELOAD_SAMPLES + 20; ++i)
        obs_reload_stats_record(&s, i, i % 2 == 0);
    assert(s.count == OBS_RELOAD_SAMPLES);
    assert(s.attempts == OBS_RELOAD_SAMPLES + 21);
    assert(s.published + s.failed == s.attempts);
    /* Window has last 128 numbers: 21 through 148. */
    assert(obs_reload_stats_percentile(&s, 0) == 21);
    assert(obs_reload_stats_percentile(&s, 50) == 84);
    assert(obs_reload_stats_percentile(&s, 95) == 142);
    assert(obs_reload_stats_percentile(&s, 99) == 147);
    assert(obs_reload_stats_percentile(&s, 100) == 148);
    assert(obs_reload_stats_percentile(&s, 900) == 148);
    assert(s.max_us == 200 && s.last_us == 148);
    puts("ok: monotonic metrics, rolling-window eviction, p50/p95/p99");
    return 0;
}
