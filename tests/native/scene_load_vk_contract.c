#include "scene_load_vk_contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Fake Vulkan exports: exercise the REAL adapter code with a stubbed SDK
static uint64_t running;
static bool fail_start, uncertain_start, fail_commit;
static unsigned begins, commits, discards, recoveries;
static ObsAsyncState gpu_state;

bool megaGeometryAsyncBeginCooked(VulkanContext *ctx, uint64_t scene_id,
                                  const MegaGeometryCounts *counts,
                                  const MegaCookedGeometry *data, uint64_t *ticket) {
    assert(ctx && scene_id && counts && data && data->vertices);
    ++begins;
    if (fail_start && !uncertain_start) return false;
    running = ++(*ticket); /* Ticket is nonzero on accepted submission. */
    gpu_state = uncertain_start ? OBS_ASYNC_QUARANTINED : OBS_ASYNC_IN_FLIGHT;
    return !uncertain_start;
}
ObsAsyncState megaGeometryAsyncPoll(uint64_t ticket) {
    return ticket == running ? gpu_state : OBS_ASYNC_IDLE;
}
bool megaGeometryAsyncCommit(VulkanContext *ctx, uint64_t ticket,
                             const MegaGeometryCounts *counts, MegaGeometryBases *bases) {
    (void)ctx;
    assert(ticket == running && counts && bases);
    ++commits;
    if (fail_commit) return false;
    *bases = (MegaGeometryBases){.vertices=3, .indices=4, .meshlets=9};
    running = 0;
    return true;
}
bool megaGeometryAsyncDiscardReady(uint64_t ticket) {
    if (ticket != running) return false;
    ++discards; running=0; return true;
}
bool megaGeometryAsyncRecoverDeviceIdle(VulkanContext *ctx, uint64_t ticket) {
    (void)ctx;
    if (ticket != running) return false;
    ++recoveries; running=0; return true;
}

int main(void) {
    VulkanContext ctx = {0};
    ObsVkSceneLoadBridge bridge = {0};
    assert(!obs_vk_scene_load_bridge_init(NULL, &ctx));
    assert(obs_vk_scene_load_bridge_init(&bridge, &ctx));
    assert(!obs_vk_scene_load_bridge_init(&bridge, &ctx));
    ObsSceneLoadBackend api = obs_vk_scene_load_backend(&bridge);
    assert(api.start && api.commit && api.poll && api.recover_quiesced);
    uint32_t counts[OBS_GEOM_POOL_COUNT] = {0};
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT] = {0};
    unsigned char data[128] = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        counts[i] = 1;
        spans[i] = (ObsGeometrySourceSpan){data, sizeof(data)};
    }
    uint64_t t = 0;
    /* All bad lengths are rejected before any Vulkan call. */
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const size_t keep = spans[i].bytes;
        spans[i].bytes = 0;
        assert(api.start(api.user, 1, counts, spans, &t) == OBS_SCENE_START_REJECTED);
        spans[i].bytes = keep;
    }
    assert(begins == 0);
    counts[OBS_GEOM_BOUNDS] = 0;
    assert(api.start(api.user, 1, counts, spans, &t) == OBS_SCENE_START_REJECTED);
    counts[OBS_GEOM_BOUNDS] = 1;
    fail_start = true;
    assert(api.start(api.user, 1, counts, spans, &t) == OBS_SCENE_START_REJECTED);
    assert(!bridge.active_ticket);
    fail_start = false;
    assert(api.start(api.user, 1, counts, spans, &t) == OBS_SCENE_START_ACCEPTED);
    assert(t && bridge.active_ticket == t);
    assert(api.poll(api.user, t) == OBS_ASYNC_IN_FLIGHT);
    gpu_state = OBS_ASYNC_READY;
    assert(api.poll(api.user, t) == OBS_ASYNC_READY);
    ObsSceneGpuBases bases = {0};
    assert(!api.commit(api.user, t+9, &bases));
    assert(api.commit(api.user, t, &bases));
    assert(bases.vertices == 3 && bases.meshlets == 9);
    assert(bridge.active_ticket == 0);
    t=0;
    uncertain_start = true;
    assert(api.start(api.user, 2, counts, spans, &t) == OBS_SCENE_START_UNKNOWN);
    assert(!api.recover_quiesced(api.user, t, false));
    assert(api.recover_quiesced(api.user, t, true));
    uncertain_start = false;
    t=0;
    assert(api.start(api.user, 3, counts, spans, &t) == OBS_SCENE_START_ACCEPTED);
    assert(api.discard_ready(api.user, t));
    assert(begins == 4 && commits == 1 && recoveries == 1 && discards == 1);
    puts("Vulkan scene-load adapter contract: span bounds, rejection, commit, recovery PASS");
}
