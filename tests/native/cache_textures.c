#include "cache_textures.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Private OMDL-like material metadata
#define MESHES 36
typedef struct { char path[OBS_CACHE_TEXTURE_CHANNELS][OBS_CACHE_TEXTURE_PATH_MAX]; } Fixture;
static const char *fetch(const void *source, size_t mesh, ObsCacheTextureChannel slot) {
    const Fixture *input = source;
    return input[mesh].path[slot];
}
static unsigned calls;
static int32_t load(void *user, const char *path) {
    (void)user;
    ++calls;
    if (strcmp(path, "unavailable.png") == 0) return -1;
    return (int32_t)(strlen(path) + 100);
}

/// Ownership, deduplication and deferred side effects
static void test_owned_interned(void) {
    Fixture *source = calloc(MESHES, sizeof(*source));
    assert(source);
    for (unsigned m = 0; m < MESHES; ++m) {
        strcpy(source[m].path[OBS_CACHE_ALBEDO], "albedo.png");
        strcpy(source[m].path[OBS_CACHE_NORMAL], "normal.png");
        if ((m % 3) == 0)
            strcpy(source[m].path[OBS_CACHE_EMISSIVE], "unavailable.png");
    }
    ObsCacheTexturePlan p = {0};
    calls = 0;
    assert(obs_cache_textures_prepare(&p, MESHES, fetch, source));
    assert(p.unique_count == 3 && p.mesh_count == MESHES && !p.resolved);
    assert(calls == 0);
    int32_t ids[OBS_CACHE_TEXTURE_CHANNELS] = {77, 77, 77, 77, 77, 77, 77};
    assert(!obs_cache_textures_indices(&p, 0, ids));
    for (unsigned i = 0; i < OBS_CACHE_TEXTURE_CHANNELS; ++i) assert(ids[i] == 77);
    memset(source, 0xab, MESHES * sizeof(*source));
    free(source); // The private manifest must not borrow OMDL metadata.
    assert(obs_cache_textures_resolve(&p, load, NULL));
    assert(calls == 3 && !obs_cache_textures_resolve(&p, load, NULL));
    assert(obs_cache_textures_indices(&p, 0, ids));
    assert(ids[OBS_CACHE_ALBEDO] == 110);
    assert(ids[OBS_CACHE_NORMAL] == 110);
    assert(ids[OBS_CACHE_EMISSIVE] == -1);
    assert(ids[OBS_CACHE_OCCLUSION] == -1);
    assert(obs_cache_textures_indices(&p, 1, ids));
    assert(ids[OBS_CACHE_EMISSIVE] == -1);
    assert(!obs_cache_textures_indices(&p, MESHES, ids));
    obs_cache_textures_dispose(&p);
    assert(!p.paths && !p.indices && !p.references && !p.mesh_count);
    obs_cache_textures_dispose(&p);
}

/// Fail-atomic validation
static void test_invalid(void) {
    Fixture f[1] = {0};
    ObsCacheTexturePlan p = {0};
    memset(f[0].path[2], 'x', sizeof(f[0].path[2]));
    assert(!obs_cache_textures_prepare(&p, 1, fetch, f));
    assert(!p.paths && !p.references && !p.indices);
    f[0].path[2][0] = '\0';
    assert(obs_cache_textures_prepare(&p, 1, fetch, f));
    assert(!obs_cache_textures_prepare(&p, 1, fetch, f));
    assert(!obs_cache_textures_resolve(&p, NULL, NULL));
    assert(!obs_cache_textures_indices(&p, 0, (int32_t[7]){0}));
    assert(obs_cache_textures_resolve(&p, load, NULL));
    int32_t result[7] = {0};
    assert(obs_cache_textures_indices(&p, 0, result));
    for (unsigned j = 0; j < 7; ++j) assert(result[j] == -1);
    obs_cache_textures_dispose(&p);
    assert(!obs_cache_textures_prepare(&p, (size_t)UINT32_MAX, fetch, f));
    assert(!obs_cache_textures_prepare(&p, 1, NULL, f));
    assert(!obs_cache_textures_prepare(&p, 1, fetch, NULL));
    assert(obs_cache_textures_prepare(&p, 0, fetch, f));
    assert(obs_cache_textures_resolve(&p, load, NULL));
    assert(!obs_cache_textures_indices(&p, 0, result));
    obs_cache_textures_dispose(&p);
}

/// Hash collisions, permutations, and 50,000 generated plans
static void test_deterministic_stress(void) {
    Fixture f[3] = {0};
    unsigned x = 0x5a32f1de;
    for (unsigned n = 0; n < 50000; ++n) {
        for (unsigned m = 0; m < 3; ++m) for (unsigned c = 0; c < 7; ++c) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            unsigned id = x % 23;
            if (id % 7 == 0) f[m].path[c][0] = '\0';
            else snprintf(f[m].path[c], sizeof(f[m].path[c]), "texture/%u.png", id);
        }
        ObsCacheTexturePlan p = {0};
        assert(obs_cache_textures_prepare(&p, 3, fetch, f));
        assert(!p.resolved && p.unique_count <= 21);
        calls = 0;
        assert(obs_cache_textures_resolve(&p, load, NULL));
        assert(calls == p.unique_count);
        for (unsigned m = 0; m < 3; ++m) {
            int32_t ids[7];
            assert(obs_cache_textures_indices(&p, m, ids));
            for (unsigned c = 0; c < 7; ++c) {
                const char *path = f[m].path[c];
                int expected = !*path ? -1 : (int)strlen(path) + 100;
                assert(ids[c] == expected);
            }
        }
        obs_cache_textures_dispose(&p);
    }
}
int main(void) {
    test_owned_interned();
    test_invalid();
    test_deterministic_stress();
    puts("cache texture descriptors: PASS (50,000 generated plans)");
    return 0;
}
