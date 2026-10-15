#include "texture_refs.h"
#include "cache_textures.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Deterministic texture pool simulator
static ObsTextureRefs *registry;
static int creates;
static int retirements;
static int next_slot;

static bool managed_acquire(void *unused, const char *path, int32_t *out) {
    (void)unused;
    uint32_t id;
    if (obs_texture_refs_find(registry, path, &id)) {
        if (!obs_texture_refs_retain(registry, id)) return false;
    } else {
        if (next_slot >= (int)OBS_TEXTURE_REF_LIMIT) return false;
        id = (uint32_t)next_slot++;
        if (!obs_texture_refs_enroll(registry, id, path)) return false;
        ++creates;
    }
    *out = (int32_t)id;
    return true;
}
static void managed_release(void *unused, int32_t index) {
    (void)unused;
    assert(obs_texture_refs_release(registry, (uint32_t)index));
}
static void sweep(void) {
    for (int i = 0; i < next_slot; ++i) {
        if (obs_texture_refs_state(registry, (uint32_t)i) == OBS_TEXTURE_REF_PENDING) {
            assert(obs_texture_refs_retire(registry, (uint32_t)i));
            ++retirements;
        }
    }
    assert(!obs_texture_refs_pending(registry));
}

/// Deduplication, cancelled retirements, and no premature slot reuse
static void test_lifetime(void) {
    int32_t a = -1, b = -1, c = -1;
    assert(managed_acquire(NULL, "stone.png", &a));
    assert(managed_acquire(NULL, "stone.png", &b) && a == b);
    assert(creates == 1 && obs_texture_refs_count(registry, (uint32_t)a) == 2);
    managed_release(NULL, a);
    assert(obs_texture_refs_state(registry, (uint32_t)a) == OBS_TEXTURE_REF_LIVE);
    managed_release(NULL, b);
    assert(obs_texture_refs_pending(registry) == 1);
    assert(managed_acquire(NULL, "stone.png", &c) && c == a);
    assert(obs_texture_refs_pending(registry) == 0);
    assert(!obs_texture_refs_retire(registry, (uint32_t)a));
    managed_release(NULL, c);
    sweep();
    assert(obs_texture_refs_state(registry, (uint32_t)a) == OBS_TEXTURE_REF_RETIRED);
    assert(!obs_texture_refs_retain(registry, (uint32_t)a));
    assert(!obs_texture_refs_release(registry, (uint32_t)a));
    assert(managed_acquire(NULL, "stone.png", &c) && c != a);
    managed_release(NULL, c);
    sweep();
    assert(!obs_texture_refs_release(registry, 8192));
    assert(!obs_texture_refs_enroll(registry, (uint32_t)a, "other"));
}

/// Exercise Cycle 38's actual lease API with repeated references
struct Input { char path[OBS_CACHE_TEXTURE_CHANNELS][OBS_CACHE_TEXTURE_PATH_MAX]; };
static const char *get_path(const void *source, size_t mesh, ObsCacheTextureChannel c) {
    (void)mesh;
    return ((const struct Input *)source)->path[(unsigned)c];
}
static void test_leases(void) {
    struct Input sample = {0};
    strcpy(sample.path[0], "albedo.png");
    strcpy(sample.path[1], "albedo.png");
    strcpy(sample.path[2], "normal.png");
    ObsCacheTexturePlan plan = {0};
    ObsCacheTextureLease lease = {0};
    assert(obs_cache_textures_prepare(&plan, 1, get_path, &sample));
    assert(plan.unique_count == 2);
    assert(obs_cache_textures_stage_retained(&plan, managed_acquire,
                                              managed_release, NULL, &lease));
    assert(lease.count == 2 && plan.resolved);
    int32_t values[OBS_CACHE_TEXTURE_CHANNELS];
    assert(obs_cache_textures_indices(&plan, 0, values));
    assert(values[0] == values[1]);
    assert(values[0] != values[2]);
    ObsCacheTextureLease visible = {0};
    assert(obs_cache_texture_lease_move(&visible, &lease));
    obs_cache_textures_dispose(&plan);
    /* Published materials may use the IDs while their scene owns the lease. */
    assert(obs_texture_refs_count(registry, (uint32_t)values[0]) == 1);
    obs_cache_texture_lease_release(&visible);
    assert(obs_texture_refs_pending(registry) == 2);
    sweep();
}

/// Fuzz-style pressure: retirements, tombstones, collisions, and refcounts
static void test_stress(void) {
    static const char *paths[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    int32_t leases[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    uint32_t rng = 0xC001D00Du;
    for (size_t step = 0; step < 50000; ++step) {
        rng = rng * 1664525u + 1013904223u;
        unsigned i = (rng >> 16) & 7u;
        if (leases[i] >= 0) {
            managed_release(NULL, leases[i]);
            leases[i] = -1;
        } else if (next_slot < (int)OBS_TEXTURE_REF_LIMIT) {
            assert(managed_acquire(NULL, paths[i], &leases[i]));
        }
        if (step % 47 == 0) sweep();
    }
    for (size_t i = 0; i < 8; ++i)
        if (leases[i] >= 0) managed_release(NULL, leases[i]);
    sweep();
    assert(next_slot < (int)OBS_TEXTURE_REF_LIMIT);
}

static void test_invalid(void) {
    uint32_t id = 77;
    assert(!obs_texture_refs_find(registry, "", &id));
    assert(!obs_texture_refs_find(registry, NULL, &id));
    char oversized[OBS_TEXTURE_REF_PATH_MAX + 1];
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized)-1] = 0;
    assert(!obs_texture_refs_enroll(registry, (uint32_t)next_slot, oversized));
    assert(!obs_texture_refs_enroll(registry, OBS_TEXTURE_REF_LIMIT, "z"));
    assert(!obs_texture_refs_retain(registry, OBS_TEXTURE_REF_LIMIT));
    assert(!obs_texture_refs_retire(registry, OBS_TEXTURE_REF_LIMIT));
}

int main(void) {
    registry = calloc(1, sizeof(*registry));
    assert(registry);
    obs_texture_refs_init(registry);
    test_lifetime();
    test_leases();
    test_stress();
    test_invalid();
    printf("texture refs: PASS (%d created, %d safely retired)\n", creates, retirements);
    free(registry);
    return 0;
}
