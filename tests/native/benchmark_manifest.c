#include "benchmark_manifest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ObsBenchmarkManifest sample(void) {
    return (ObsBenchmarkManifest){
        .name = "sponza-cold",
        .scene = "assets/gltf/Sponza/glTF/Sponza.gltf",
        .width = 1280,
        .height = 720,
        .warmup_frames = 120,
        .measured_frames = 600,
        .min_mesh_count = 1,
        .cache_mode = OBS_BENCHMARK_CACHE_COLD,
    };
}

int main(void) {
    ObsBenchmarkManifest manifest = sample();
    ObsBenchmarkManifest out = {0};
    assert(!obs_benchmark_manifest_stage(&manifest));
    obs_benchmark_manifest_begin();
    assert(obs_benchmark_manifest_stage(&manifest));
    assert(!obs_benchmark_manifest_stage(&manifest));
    assert(!obs_benchmark_manifest_get(&out));
    obs_benchmark_manifest_finish(false);
    assert(!obs_benchmark_manifest_get(&out));

    obs_benchmark_manifest_begin();
    assert(obs_benchmark_manifest_stage(&manifest));
    obs_benchmark_manifest_finish(true);
    assert(obs_benchmark_manifest_get(&out));
    assert(strcmp(out.name, manifest.name) == 0);
    assert(strcmp(out.scene, manifest.scene) == 0);
    assert(out.width == 1280 && out.height == 720);
    assert(out.warmup_frames == 120 && out.measured_frames == 600);
    assert(out.cache_mode == OBS_BENCHMARK_CACHE_COLD);

    manifest.width = 8193;
    obs_benchmark_manifest_begin();
    assert(!obs_benchmark_manifest_stage(&manifest));
    obs_benchmark_manifest_finish(false);
    manifest = sample();
    manifest.warmup_frames = 100000;
    obs_benchmark_manifest_begin();
    assert(!obs_benchmark_manifest_stage(&manifest));
    obs_benchmark_manifest_finish(false);

    obs_benchmark_manifest_begin();
    obs_benchmark_manifest_finish(true);
    assert(!obs_benchmark_manifest_get(&out));
    puts("benchmark manifest: staged, rollback, publish, and clear PASS");
    return 0;
}
