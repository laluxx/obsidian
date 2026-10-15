/// HotReloadStats — measured compile/publication latency
#include "hotreload_stats.h"

/// Samples
void obs_reload_stats_record(ObsReloadStats *s, uint64_t elapsed_us, bool published) {
    if (!s) return;
    ++s->attempts;
    if (published) ++s->published;
    else ++s->failed;
    s->last_us = elapsed_us;
    if (elapsed_us > s->max_us) s->max_us = elapsed_us;
    s->samples[s->next] = elapsed_us;
    s->next = (s->next + 1) % OBS_RELOAD_SAMPLES;
    if (s->count < OBS_RELOAD_SAMPLES) ++s->count;
}

/// Percentile
uint64_t obs_reload_stats_percentile(const ObsReloadStats *s, unsigned percentile) {
    if (!s || !s->count) return 0;
    uint64_t sorted[OBS_RELOAD_SAMPLES];
    for (size_t i = 0; i < s->count; ++i) sorted[i] = s->samples[i];
    /* Insertion sort runs only when an inspector queries a diagnostic, never
     * during publication or the frame's normal rendering path. */
    for (size_t i = 1; i < s->count; ++i) {
        uint64_t value = sorted[i];
        size_t j = i;
        while (j && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1]; --j;
        }
        sorted[j] = value;
    }
    if (percentile > 100) percentile = 100;
    if (!percentile) return sorted[0];
    size_t index = ((size_t)percentile * s->count + 99u) / 100u;
    return sorted[index ? index - 1 : 0];
}
