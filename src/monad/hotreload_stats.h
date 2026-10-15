/// HotReloadStats — bounded native-image publication timing, no allocation
#ifndef OBSIDIAN_MONAD_HOTRELOAD_STATS_H
#define OBSIDIAN_MONAD_HOTRELOAD_STATS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Public interface
#define OBS_RELOAD_SAMPLES 128u
typedef struct ObsReloadStats {
    uint64_t attempts;
    uint64_t published;
    uint64_t failed;
    uint64_t last_us;
    uint64_t max_us;
    uint64_t samples[OBS_RELOAD_SAMPLES];
    size_t count;
    size_t next;
} ObsReloadStats;

void obs_reload_stats_record(ObsReloadStats *stats, uint64_t elapsed_us, bool published);
/* Nearest-rank percentile of last 128 attempts; 0 for no observations. */
uint64_t obs_reload_stats_percentile(const ObsReloadStats *stats, unsigned percentile);
#endif
