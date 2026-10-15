#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OBS_BENCHMARK_NAME_CAP 64
#define OBS_BENCHMARK_SCENE_CAP 512

typedef enum ObsBenchmarkCacheMode {
    OBS_BENCHMARK_CACHE_COLD = 0,
    OBS_BENCHMARK_CACHE_WARM = 1
} ObsBenchmarkCacheMode;

typedef struct ObsBenchmarkManifest {
    char name[OBS_BENCHMARK_NAME_CAP];
    char scene[OBS_BENCHMARK_SCENE_CAP];
    uint32_t width;
    uint32_t height;
    uint32_t warmup_frames;
    uint32_t measured_frames;
    uint32_t min_mesh_count;
    ObsBenchmarkCacheMode cache_mode;
} ObsBenchmarkManifest;

void obs_benchmark_manifest_begin(void);
bool obs_benchmark_manifest_stage(const ObsBenchmarkManifest *manifest);
void obs_benchmark_manifest_finish(bool published);
bool obs_benchmark_manifest_get(ObsBenchmarkManifest *out);
