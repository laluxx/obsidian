#include "benchmark_manifest.h"

#include <string.h>

static bool s_transaction_active;
static bool s_staged;
static bool s_published;
static ObsBenchmarkManifest s_staged_manifest;
static ObsBenchmarkManifest s_published_manifest;

void obs_benchmark_manifest_begin(void) {
    s_transaction_active = true;
    s_staged = false;
    memset(&s_staged_manifest, 0, sizeof(s_staged_manifest));
}

bool obs_benchmark_manifest_stage(const ObsBenchmarkManifest *manifest) {
    if (!s_transaction_active || s_staged || !manifest ||
        !manifest->name[0] || !manifest->scene[0] ||
        !manifest->width || manifest->width > 8192 ||
        !manifest->height || manifest->height > 8192 ||
        !manifest->measured_frames || !manifest->min_mesh_count ||
        manifest->measured_frames > 100000 ||
        manifest->warmup_frames > 100000 - manifest->measured_frames ||
        manifest->cache_mode < OBS_BENCHMARK_CACHE_COLD ||
        manifest->cache_mode > OBS_BENCHMARK_CACHE_WARM ||
        !memchr(manifest->name, '\0', sizeof(manifest->name)) ||
        !memchr(manifest->scene, '\0', sizeof(manifest->scene)))
        return false;
    s_staged_manifest = *manifest;
    s_staged = true;
    return true;
}

void obs_benchmark_manifest_finish(bool published) {
    if (!s_transaction_active) return;
    if (published && s_staged) {
        s_published_manifest = s_staged_manifest;
        s_published = true;
    } else if (published) {
        memset(&s_published_manifest, 0, sizeof(s_published_manifest));
        s_published = false;
    }
    s_transaction_active = false;
    s_staged = false;
    memset(&s_staged_manifest, 0, sizeof(s_staged_manifest));
}

bool obs_benchmark_manifest_get(ObsBenchmarkManifest *out) {
    if (!out || !s_published) return false;
    *out = s_published_manifest;
    return true;
}
