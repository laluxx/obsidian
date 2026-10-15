#include "pending_load.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/// Deterministic mock GPU with the REAL pending geometry journal
static struct {
    ObsGeometryAsync geometry;
    ObsAsyncSubmitResult start_result;
    ObsAsyncPollResult poll_result;
    bool fail_retire;
    uint64_t live_ticket;
    unsigned starts, polls, retires;
    unsigned disposed, activated;
    bool allow_activation;
    void *visible;
} gpu;

typedef struct {
    unsigned char *graph, *materials, *textures, *animations, *physics;
    uint64_t id;
} Candidate;

static ObsAsyncSubmitResult submit_gpu(void *user, uint64_t ticket,
                                       const ObsGeometryUploadBatch *batch) {
    (void)user;
    assert(batch && batch->state == OBS_UPLOAD_PREPARED);
    ++gpu.starts;
    if (gpu.start_result != OBS_ASYNC_REJECTED) gpu.live_ticket = ticket;
    return gpu.start_result;
}
static ObsAsyncPollResult poll_gpu(void *user, uint64_t ticket) {
    (void)user;
    assert(gpu.live_ticket == ticket);
    ++gpu.polls;
    return gpu.poll_result;
}
static bool retire_gpu(void *user, uint64_t ticket, bool proof) {
    (void)user;
    assert(proof && gpu.live_ticket == ticket);
    ++gpu.retires;
    if (gpu.fail_retire) return false;
    gpu.live_ticket = 0;
    return true;
}

/// Renderer adapter enforces GPU-cursor commit, not just fence visibility
static ObsSceneStartResult start_scene(void *ctx, uint64_t scene_id,
            const uint32_t counts[OBS_GEOM_POOL_COUNT],
            const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT], uint64_t *ticket) {
    ObsGeometryAsync *a = ctx;
    uint64_t issued;
    if (!obs_geometry_async_prepare(a, scene_id, counts, spans, &issued))
        return OBS_SCENE_START_REJECTED;
    if (!obs_geometry_async_submit(a, issued)) {
        if (a->state == OBS_ASYNC_PREPARED) {
            assert(obs_geometry_async_cancel_prepared(a, issued));
            return OBS_SCENE_START_REJECTED;
        }
        *ticket = issued;
        return OBS_SCENE_START_UNKNOWN;
    }
    *ticket = issued;
    return OBS_SCENE_START_ACCEPTED;
}
static ObsAsyncState poll_scene(void *ctx, uint64_t ticket) {
    return obs_geometry_async_poll(ctx, ticket);
}
static bool commit_scene(void *ctx, uint64_t ticket, ObsSceneGpuBases *out) {
    ObsGeometryReservation r;
    if (!obs_geometry_async_commit(ctx, ticket, &r)) return false;
    *out = (ObsSceneGpuBases){
        .vertices = r.base[OBS_GEOM_VERTEX],
        .indices = r.base[OBS_GEOM_INDEX],
        .morphs = r.base[OBS_GEOM_MORPH],
        .meshlets = r.base[OBS_GEOM_MESHLET],
        .meshlet_vertices = r.base[OBS_GEOM_MESHLET_VERTEX],
        .meshlet_triangles = r.base[OBS_GEOM_MESHLET_TRIANGLE],
    };
    return true;
}
static bool discard_scene(void *ctx, uint64_t ticket) {
    return obs_geometry_async_discard_ready(ctx, ticket);
}
static bool recover_scene(void *ctx, uint64_t ticket, bool quiesced) {
    return obs_geometry_async_retire_quiesced(ctx, ticket, quiesced);
}

/// Owned graph/material/texture/animation/physics fixture
static Candidate *candidate_new(uint64_t id) {
    Candidate *c = calloc(1, sizeof(*c));
    assert(c);
    c->id = id;
    c->graph = malloc(64);
    c->materials = malloc(16);
    c->textures = malloc(16);
    c->animations = malloc(16);
    c->physics = malloc(16);
    assert(c->graph && c->materials && c->textures && c->animations && c->physics);
    memset(c->graph, (int)(id & 0xff), 64);
    return c;
}
static void dispose(void *object, void *user) {
    (void)user;
    Candidate *c = object;
    assert(c && c->graph[0] == (unsigned char)(c->id & 0xff));
    free(c->graph); free(c->materials); free(c->textures);
    free(c->animations); free(c->physics); free(c);
    ++gpu.disposed;
}
static bool ready(const void *object, void *user) {
    (void)user;
    const Candidate *c = object;
    return gpu.allow_activation && c && c->graph && c->materials &&
           c->textures && c->animations && c->physics;
}
static void activate(void *object, void *user, const ObsSceneGpuBases *bases) {
    (void)user;
    Candidate *c = object;
    assert(!gpu.visible && c && c->graph && bases->vertices < 10000000);
    gpu.visible = object;
    ++gpu.activated;
}
static ObsSceneLoadPayload payload(uint64_t id) {
    return (ObsSceneLoadPayload){ .object = candidate_new(id),
        .dispose = dispose, .ready = ready, .activate = activate };
}
static void release_visible(void) {
    assert(gpu.visible);
    dispose(gpu.visible, NULL);
    gpu.visible = NULL;
}
static void init(ObsPendingSceneLoad *owner) {
    memset(&gpu, 0, sizeof(gpu));
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i].used = 4;
        b.pools[i].element_bytes = i == OBS_GEOM_MESHLET_TRIANGLE ? 1 : 4;
        b.pools[i].capacity_bytes = 16000000;
    }
    ObsGeometryAsyncBackend geom = {
        .start = submit_gpu, .poll = poll_gpu, .retire = retire_gpu,
    };
    assert(obs_geometry_async_init(&gpu.geometry, &b, &geom));
    ObsSceneLoadBackend backend = {
        .start = start_scene, .poll = poll_scene, .commit = commit_scene,
        .discard_ready = discard_scene, .recover_quiesced = recover_scene,
        .user = &gpu.geometry,
    };
    memset(owner, 0, sizeof(*owner));
    assert(obs_scene_load_init(owner, &backend));
    assert(!obs_scene_load_init(owner, &backend));
    gpu.allow_activation = true;
    gpu.start_result = OBS_ASYNC_ACCEPTED;
    gpu.poll_result = OBS_ASYNC_PENDING;
}
static bool begin(ObsPendingSceneLoad *owner, uint64_t id,
                  ObsSceneLoadPayload *candidate, uint64_t *ticket) {
    static const unsigned char input[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t counts[OBS_GEOM_POOL_COUNT];
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        counts[i] = 2;
        spans[i].data = input;
        spans[i].bytes = i == OBS_GEOM_MESHLET_TRIANGLE ? 2 : 8;
    }
    return obs_scene_load_prepare(owner, id, counts, spans, candidate, ticket);
}

/// Safe-point tests
static void success_and_reuse(void) {
    ObsPendingSceneLoad a = {0}; init(&a);
    ObsSceneLoadPayload first = payload(1);
    uint64_t t = 0, stale = 0;
    assert(begin(&a, 1, &first, &t) && t && !first.object);
    assert(!obs_scene_load_close(&a));
    assert(!obs_scene_load_cancel(&a, t));
    assert(obs_scene_load_poll(&a, t) == OBS_LOAD_IN_FLIGHT);
    assert(gpu.visible == NULL);
    gpu.poll_result = OBS_ASYNC_SIGNALED;
    assert(obs_scene_load_poll(&a, t) == OBS_LOAD_READY);
    ObsSceneGpuBases bases = {0};
    gpu.allow_activation = false;
    assert(!obs_scene_load_publish(&a, t, &bases) && a.state == OBS_LOAD_READY);
    assert(!gpu.visible);
    gpu.allow_activation = true;
    assert(!obs_scene_load_publish(&a, t + 1, &bases));
    assert(obs_scene_load_publish(&a, t, &bases));
    assert(bases.vertices == 4 && bases.indices == 4);
    assert(gpu.visible && a.state == OBS_LOAD_EMPTY && gpu.activated == 1);
    assert(!obs_scene_load_poll(&a, t));
    release_visible();
    stale = t;
    gpu.poll_result = OBS_ASYNC_SIGNALED;
    ObsSceneLoadPayload second = payload(2);
    assert(begin(&a, 2, &second, &t) && t != stale);
    assert(!obs_scene_load_cancel(&a, stale));
    assert(obs_scene_load_poll(&a, t) == OBS_LOAD_READY);
    assert(obs_scene_load_cancel(&a, t) && !gpu.visible);
    assert(a.discarded == 1 && a.activated == 1);
    assert(obs_scene_load_close(&a));
}
static void rejection_and_recovery(void) {
    ObsPendingSceneLoad a = {0}; init(&a);
    ObsSceneLoadPayload c = payload(10);
    gpu.start_result = OBS_ASYNC_REJECTED;
    uint64_t ticket = 99;
    assert(!begin(&a, 10, &c, &ticket));
    assert(c.object && ticket == 99 && a.state == OBS_LOAD_EMPTY);
    dispose(c.object, c.user);
    gpu.start_result = OBS_ASYNC_UNKNOWN;
    c = payload(11);
    assert(begin(&a, 11, &c, &ticket));
    assert(!c.object && a.state == OBS_LOAD_QUARANTINED);
    assert(!obs_scene_load_close(&a));
    assert(!obs_scene_load_cancel(&a, ticket));
    assert(!obs_scene_load_recover_quiesced(&a, ticket, false));
    assert(obs_scene_load_recover_quiesced(&a, ticket, true));
    assert(a.state == OBS_LOAD_EMPTY && gpu.disposed == 2);
    assert(obs_scene_load_close(&a));
}
static void commit_failure(void) {
    ObsPendingSceneLoad a = {0}; init(&a);
    ObsSceneLoadPayload c = payload(20);
    uint64_t t = 0;
    assert(begin(&a, 20, &c, &t));
    gpu.poll_result = OBS_ASYNC_SIGNALED;
    assert(obs_scene_load_poll(&a, t) == OBS_LOAD_READY);
    gpu.fail_retire = true;
    ObsSceneGpuBases bases = {0};
    assert(!obs_scene_load_publish(&a, t, &bases));
    assert(!gpu.visible && a.state == OBS_LOAD_QUARANTINED);
    assert(!obs_scene_load_recover_quiesced(&a, t, true));
    gpu.fail_retire = false;
    assert(obs_scene_load_recover_quiesced(&a, t, true));
    assert(gpu.disposed == 1 && gpu.activated == 0);
    assert(obs_scene_load_close(&a));
}
static void stress(void) {
    ObsPendingSceneLoad a = {0}; init(&a);
    uint64_t last = 0;
    for (uint64_t i = 1; i <= 50000; ++i) {
        ObsSceneLoadPayload c = payload(i);
        uint64_t t = 0;
        assert(begin(&a, i, &c, &t) && t > last);
        last = t;
        gpu.poll_result = OBS_ASYNC_SIGNALED;
        assert(obs_scene_load_poll(&a, t) == OBS_LOAD_READY);
        if (i % 3 == 0) {
            assert(obs_scene_load_cancel(&a, t));
        } else {
            ObsSceneGpuBases bases;
            assert(obs_scene_load_publish(&a, t, &bases));
            release_visible();
        }
        gpu.poll_result = OBS_ASYNC_PENDING;
    }
    assert(a.state == OBS_LOAD_EMPTY && !gpu.visible);
    assert(a.accepted == 50000 && a.activated + a.discarded == 50000);
    assert(gpu.starts == 50000 && gpu.disposed == 50000);
    assert(obs_scene_load_close(&a));
}
int main(void) {
    success_and_reuse(); rejection_and_recovery(); commit_failure(); stress();
    puts("pending SceneLoad ownership: 50,000 cycles, rejection, stale tickets,\n"
         "  quarantine, activation checks, teardown: PASS");
}
