#include "cache_textures.h"
#include "pending_load.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Deterministic texture-pool simulator with balanced reference accounting
#define CHANNELS OBS_CACHE_TEXTURE_CHANNELS
#define MAX_MOCK_RESOURCES 64
static struct {
    int refs[MAX_MOCK_RESOURCES];
    int acquire_calls, release_calls, fail_call;
    unsigned disposals, activations;
} mock;

typedef struct { char path[CHANNELS][OBS_CACHE_TEXTURE_PATH_MAX]; } Metadata;
static const char *fetch(const void *src, size_t mesh, ObsCacheTextureChannel c) {
    return ((const Metadata *)src)[mesh].path[c];
}
static bool acquire(void *user, const char *path, int32_t *out) {
    (void)user;
    int call = ++mock.acquire_calls;
    if (mock.fail_call && call == mock.fail_call) return false;
    if (!strcmp(path, "optional-missing")) { *out = -1; return true; }
    /* Deliberate collisions: distinct paths may share a refcounted pool ID. */
    unsigned id = (unsigned)(strlen(path) % MAX_MOCK_RESOURCES);
    ++mock.refs[id];
    *out = (int32_t)id;
    return true;
}
static void release(void *user, int32_t id) {
    (void)user;
    assert(id >= 0 && id < MAX_MOCK_RESOURCES);
    assert(mock.refs[id] > 0);
    --mock.refs[id];
    ++mock.release_calls;
}
static void assert_clean(void) {
    for (unsigned i = 0; i < MAX_MOCK_RESOURCES; ++i) assert(mock.refs[i] == 0);
}
static void fixture(Metadata *m) {
    memset(m, 0, sizeof(*m));
    strcpy(m->path[0], "a.png");
    strcpy(m->path[1], "b.png");
    strcpy(m->path[2], "optional-missing");
    strcpy(m->path[3], "longer.png");
}

/// Acquire/release pairs, rollback and lease ownership
static void test_retained(void) {
    Metadata m;
    fixture(&m);
    for (unsigned fail = 1; fail <= 5; ++fail) {
        ObsCacheTexturePlan plan = {0};
        ObsCacheTextureLease lease = {0};
        assert(obs_cache_textures_prepare(&plan, 1, fetch, &m));
        assert(plan.unique_count == 4);
        mock.acquire_calls = mock.release_calls = 0;
        mock.fail_call = (int)fail;
        bool ok = obs_cache_textures_stage_retained(&plan, acquire, release, NULL, &lease);
        assert(ok == (fail == 5));
        if (!ok) {
            assert(!plan.resolved && !lease.initialized);
            assert_clean();
            mock.fail_call = 0;
            assert(obs_cache_textures_stage_retained(&plan, acquire, release, NULL, &lease));
        }
        assert(plan.resolved && lease.initialized && lease.count == 3);
        int32_t material[CHANNELS] = {0};
        assert(obs_cache_textures_indices(&plan, 0, material));
        assert(material[2] == -1);
        ObsCacheTextureLease transferred = {0};
        assert(obs_cache_texture_lease_move(&transferred, &lease));
        assert(!obs_cache_texture_lease_move(&lease, &lease));
        assert(!lease.initialized && transferred.initialized);
        obs_cache_textures_dispose(&plan);
        /* Resolved indices have gone, but the scene still retains the pool. */
        assert(mock.refs[(unsigned)material[0]] > 0);
        obs_cache_texture_lease_release(&transferred);
        obs_cache_texture_lease_release(&transferred);
        assert_clean();
    }
}

/// Invalid resources, empty manifests, and occupied destination leases
static bool acquire_invalid(void *user, const char *path, int32_t *out) {
    if (!strcmp(path, "b.png")) { *out = -2; return true; }
    return acquire(user, path, out);
}
static void test_contract_edges(void) {
    Metadata m;
    fixture(&m);
    ObsCacheTexturePlan p = {0};
    ObsCacheTextureLease l = {0};
    mock.fail_call = 0;
    mock.acquire_calls = 0;
    assert(obs_cache_textures_prepare(&p, 1, fetch, &m));
    assert(!obs_cache_textures_stage_retained(&p, acquire_invalid, release, NULL, &l));
    assert(!p.resolved && !l.initialized);
    assert_clean();
    assert(obs_cache_textures_stage_retained(&p, acquire, release, NULL, &l));
    assert(!obs_cache_textures_stage_retained(&p, acquire, release, NULL, &l));
    ObsCacheTextureLease occupied = { .initialized = true };
    assert(!obs_cache_texture_lease_move(&occupied, &l));
    assert(l.initialized && occupied.initialized);
    obs_cache_texture_lease_release(&occupied);
    obs_cache_texture_lease_release(&l);
    obs_cache_textures_dispose(&p);
    assert_clean();

    ObsCacheTexturePlan empty = {0};
    ObsCacheTextureLease empty_lease = {0};
    assert(obs_cache_textures_prepare(&empty, 0, fetch, &m));
    assert(obs_cache_textures_stage_retained(&empty, acquire, release,
                                             NULL, &empty_lease));
    assert(empty.resolved && empty_lease.initialized && !empty_lease.count);
    ObsCacheTextureLease transferred = {0};
    assert(obs_cache_texture_lease_move(&transferred, &empty_lease));
    obs_cache_textures_dispose(&empty);
    obs_cache_texture_lease_release(&transferred);
    assert_clean();
}

/// A pending SceneLoad owns texture resources until GPU publication
struct Candidate {
    ObsCacheTexturePlan plan;
    ObsCacheTextureLease lease;
};
static struct Candidate *visible;
static struct {
    ObsSceneStartResult start_result;
    ObsAsyncState poll_result;
    bool commit_ok, discard_ok, recover_ok;
    uint64_t ticket;
} backend;
static ObsSceneStartResult backend_start(void *u, uint64_t scene,
                         const uint32_t counts[OBS_GEOM_POOL_COUNT],
                         const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                         uint64_t *ticket) {
    (void)u; (void)scene; (void)counts; (void)spans;
    *ticket = ++backend.ticket;
    return backend.start_result;
}
static ObsAsyncState backend_poll(void *u, uint64_t ticket) {
    (void)u; assert(ticket == backend.ticket); return backend.poll_result;
}
static bool backend_commit(void *u, uint64_t ticket, ObsSceneGpuBases *bases) {
    (void)u; assert(ticket == backend.ticket);
    if (!backend.commit_ok) return false;
    *bases = (ObsSceneGpuBases){.vertices = 42};
    return true;
}
static bool backend_discard(void *u, uint64_t ticket) {
    (void)u; assert(ticket == backend.ticket); return backend.discard_ok;
}
static bool backend_recover(void *u, uint64_t ticket, bool quiesced) {
    (void)u; assert(ticket == backend.ticket); return quiesced && backend.recover_ok;
}
static bool ready(const void *ptr, void *u) {
    (void)u;
    const struct Candidate *c = ptr;
    return c->plan.resolved && c->lease.initialized;
}
static void dispose(void *ptr, void *u) {
    (void)u;
    struct Candidate *c = ptr;
    obs_cache_texture_lease_release(&c->lease);
    obs_cache_textures_dispose(&c->plan);
    free(c);
    ++mock.disposals;
}
static void activate(void *ptr, void *u, const ObsSceneGpuBases *bases) {
    (void)u;
    assert(bases->vertices == 42 && !visible);
    visible = ptr;
    ++mock.activations;
}
static ObsSceneLoadPayload candidate(void) {
    Metadata m;
    fixture(&m);
    struct Candidate *c = calloc(1, sizeof(*c));
    assert(c);
    assert(obs_cache_textures_prepare(&c->plan, 1, fetch, &m));
    mock.fail_call = 0;
    assert(obs_cache_textures_stage_retained(&c->plan, acquire, release, NULL, &c->lease));
    return (ObsSceneLoadPayload){.object=c, .dispose=dispose, .ready=ready,
                                  .activate=activate};
}
static void test_pending_scene_lifecycle(void) {
    ObsSceneLoadBackend ops = {.start=backend_start, .poll=backend_poll,
       .commit=backend_commit, .discard_ready=backend_discard,
       .recover_quiesced=backend_recover};
    ObsPendingSceneLoad pending = {0};
    assert(obs_scene_load_init(&pending, &ops));
    uint32_t counts[OBS_GEOM_POOL_COUNT] = {0};
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
    for (unsigned i = 0; i < 2500; ++i) {
        backend.start_result = OBS_SCENE_START_ACCEPTED;
        backend.poll_result = OBS_ASYNC_READY;
        backend.commit_ok = backend.discard_ok = backend.recover_ok = true;
        ObsSceneLoadPayload payload = candidate();
        uint64_t ticket = 0;
        assert(obs_scene_load_prepare(&pending, i + 1, counts, spans, &payload, &ticket));
        assert(!payload.object && !visible);
        assert(obs_scene_load_poll(&pending, ticket) == OBS_LOAD_READY);
        if (i % 3 == 0) {
            ObsSceneGpuBases bases = {0};
            assert(obs_scene_load_publish(&pending, ticket, &bases));
            assert(visible && bases.vertices == 42);
            dispose(visible, NULL);
            visible = NULL;
        } else if (i % 3 == 1) {
            assert(obs_scene_load_cancel(&pending, ticket));
        } else {
            backend.commit_ok = false;
            ObsSceneGpuBases bases = {0};
            assert(!obs_scene_load_publish(&pending, ticket, &bases));
            assert(pending.state == OBS_LOAD_QUARANTINED);
            assert(!obs_scene_load_recover_quiesced(&pending, ticket, false));
            assert(obs_scene_load_recover_quiesced(&pending, ticket, true));
        }
        assert_clean();
    }
    assert(obs_scene_load_close(&pending));
}

/// Randomized acquisition failure positions and scene-owner churn
static void test_stress(void) {
    Metadata m;
    fixture(&m);
    unsigned state = 0x9e3779b9u;
    for (unsigned i = 0; i < 50000; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        mock.acquire_calls = 0;
        mock.fail_call = (int)(state % 6);
        ObsCacheTexturePlan p = {0};
        ObsCacheTextureLease l = {0};
        assert(obs_cache_textures_prepare(&p, 1, fetch, &m));
        bool ok = obs_cache_textures_stage_retained(&p, acquire, release, NULL, &l);
        if (ok) {
            if (state & 1u) {
                ObsCacheTextureLease new_owner = {0};
                assert(obs_cache_texture_lease_move(&new_owner, &l));
                obs_cache_textures_dispose(&p);
                obs_cache_texture_lease_release(&new_owner);
            } else {
                obs_cache_texture_lease_release(&l);
                obs_cache_textures_dispose(&p);
            }
        } else {
            assert(!l.initialized && !p.resolved);
            obs_cache_textures_dispose(&p);
        }
        assert_clean();
    }
}
int main(void) {
    test_retained();
    test_contract_edges();
    test_pending_scene_lifecycle();
    test_stress();
    assert_clean();
    printf("retained texture leases: PASS (50,000 stress + 2,500 pending scenes)\n");
    return 0;
}
