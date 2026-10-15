#include "geometry_scene_upload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Real owned source spans; no Vulkan dependencies
static void init(ObsGeometrySceneUploads *q) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        b.pools[i].used = 4;
        b.pools[i].element_bytes = i == OBS_GEOM_MESHLET_TRIANGLE ? 1 : 4;
        b.pools[i].capacity_bytes = 262144;
    }
    assert(obs_scene_uploads_init(q, &b));
}

static bool prepare(ObsGeometrySceneUploads *q, uint64_t scene_id, uint64_t *t) {
    uint32_t n[OBS_GEOM_POOL_COUNT];
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
    unsigned char bytes[16] = {1,2,3,4,5,6,7,8};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        n[i] = 2;
        spans[i] = (ObsGeometrySourceSpan){bytes,
            i == OBS_GEOM_MESHLET_TRIANGLE ? 2u : 8u};
    }
    return obs_scene_upload_prepare(q, scene_id, n, spans, t);
}

static void flow(void) {
    ObsGeometrySceneUploads q = {0}; init(&q);
    ObsGeometryBudget attempted = q.scenes.pool_layout;
    assert(!obs_scene_uploads_init(&q, &attempted));
    uint64_t a = 0, b = 0, c = 0;
    assert(prepare(&q, 11, &a));
    assert(prepare(&q, 12, &b));
    assert(!prepare(&q, 12, &c));
    assert(q.scenes.highwater[0] == 8);
    /* Reject malformed spans without allocating or changing reservations. */
    ObsGeometrySourceSpan missing[OBS_GEOM_POOL_COUNT] = {0};
    uint32_t n[OBS_GEOM_POOL_COUNT];
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) n[i] = 2;
    const uint32_t watermark = q.scenes.highwater[0];
    assert(!obs_scene_upload_prepare(&q, 40, n, missing, &c));
    assert(q.scenes.highwater[0] == watermark);
    ObsGeometryReservation published = { .base = { 900 } };
    assert(!obs_scene_upload_publish(&q, a, &published));
    assert(published.base[0] == 900);
    assert(obs_scene_upload_submitted(&q, a));
    assert(obs_scene_upload_submitted(&q, b));
    assert(obs_scene_upload_observe(&q, b, OBS_SCENE_FENCE_PENDING));
    assert(obs_scene_upload_observe(&q, b, OBS_SCENE_FENCE_SIGNALED));
    assert(obs_scene_upload_publish(&q, b, &published));
    assert(published.base[0] == 6 && published.end[0] == 8);
    assert(!obs_scene_upload_cancel(&q, a));
    assert(obs_scene_upload_observe(&q, a, OBS_SCENE_FENCE_UNKNOWN));
    assert(!obs_scene_upload_publish(&q, a, &published));
    assert(!obs_scene_upload_retire_quiesced(&q, a, false));
    assert(obs_scene_upload_retire_quiesced(&q, a, true));
    assert(obs_pending_scenes_drain(&q.scenes));
    assert(q.scenes.highwater[0] == 8);
    assert(prepare(&q, 13, &c));
    assert(c > b);
    assert(!obs_scene_upload_cancel(&q, b));
    assert(obs_scene_upload_cancel(&q, c));
    assert(q.scenes.highwater[0] == 8);
}

static void stress(void) {
    ObsGeometrySceneUploads q = {0}; init(&q);
    uint64_t seq = 0x82acb97f45612399ULL;
    for (unsigned i = 0; i < 50000; ++i) {
        uint64_t ticket = 0;
        assert(prepare(&q, 1, &ticket));
        seq ^= seq << 13; seq ^= seq >> 7; seq ^= seq << 17;
        if (seq & 1) {
            assert(obs_scene_upload_cancel(&q, ticket));
        } else {
            assert(obs_scene_upload_submitted(&q, ticket));
            if (seq & 2) {
                assert(obs_scene_upload_observe(&q, ticket, OBS_SCENE_FENCE_UNKNOWN));
                assert(obs_scene_upload_retire_quiesced(&q, ticket, true));
            } else {
                assert(obs_scene_upload_observe(&q, ticket, OBS_SCENE_FENCE_SIGNALED));
                ObsGeometryReservation reserved;
                assert(obs_scene_upload_publish(&q, ticket, &reserved));
            }
        }
        assert(!obs_scene_upload_cancel(&q, ticket));
        assert(obs_pending_scenes_drain(&q.scenes));
    }
    puts("geometry-scene-upload: 50,000 stage/publish/recover cycles passed");
}

int main(void) { flow(); stress(); }
