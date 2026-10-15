#define _POSIX_C_SOURCE 200809L
/// dispatch_metrics_bench.c — Cost of optional CPU-side foreign call telemetry
#include "dispatch_metrics.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/// Controlled workload
int main(int argc, char **argv) {
    uint64_t iterations = 2000000;
    if (argc > 1) {
        char *end = NULL;
        unsigned long long count = strtoull(argv[1], &end, 10);
        if (!end || *end || !count || count > 50000000) return 2;
        iterations = (uint64_t)count;
    }
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        obs_dispatch_metrics_reset(enabled != 0);
        uint64_t start = obs_dispatch_clock_ns();
        for (uint64_t i = 0; i < iterations; ++i)
            obs_dispatch_metrics_record((ObsDispatchLane)(i & 1),
                                        16 + (i & 31), 32 + (i & 255),
                                        64 + (i & 511), true);
        uint64_t end = obs_dispatch_clock_ns();
        ObsDispatchMetrics metrics = {0};
        obs_dispatch_metrics_snapshot(&metrics);
        printf("dispatch profiler %s: %.3f ns/record, %" PRIu64 " observed records\n",
               enabled ? "enabled" : "disabled",
               (double)(end - start) / (double)iterations,
               metrics.lanes[0].phases[OBS_DISPATCH_TOTAL].samples +
               metrics.lanes[1].phases[OBS_DISPATCH_TOTAL].samples);
    }
    puts("CPU histogram microbenchmark only; NOT engine FPS or native JIT latency");
    return 0;
}
