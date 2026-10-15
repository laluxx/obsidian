#include "geometry_pending.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/// Fixtures
static ObsGeometryBudget budget(uint32_t used, uint32_t capacity) {
    ObsGeometryBudget b = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        const uint64_t stride = i == OBS_GEOM_MESHLET_TRIANGLE ? 1 : 4;
        b.pools[i] = (ObsGeometryPoolBudget){
            .used = used, .element_bytes = stride,
            .capacity_bytes = (uint64_t)capacity * stride
        };
    }
    return b;
}
static void counts(uint32_t out[OBS_GEOM_POOL_COUNT], uint32_t value) {
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) out[i] = value;
}

/// Failure atomicity and checked identity
static void validation(void) {
    ObsPendingScenes journal = {0};
    ObsGeometryBudget b = budget(5, 20);
    assert(obs_pending_scenes_init(&journal, &b));
    assert(!obs_pending_scenes_init(&journal, &b));
    assert(journal.highwater[0] == 5);
    uint32_t n[OBS_GEOM_POOL_COUNT]; counts(n, 3);
    uint64_t t = 777;
    assert(!obs_pending_scene_reserve(&journal, 0, n, &t));
    assert(t == 777 && journal.highwater[0] == 5);
    n[OBS_GEOM_BOUNDS] = 2;
    assert(!obs_pending_scene_reserve(&journal, 1, n, &t));
    counts(n, 21);
    assert(!obs_pending_scene_reserve(&journal, 1, n, &t));
    counts(n, 3);
    assert(obs_pending_scene_reserve(&journal, 1, n, &t));
    assert(t == 1 && journal.highwater[0] == 8);
    assert(!obs_pending_scene_reserve(&journal, 1, n, &t));
    ObsPendingScene inspect = {0};
    assert(obs_pending_scene_snapshot(&journal, t, &inspect));
    assert(inspect.scene_id == 1 && inspect.state == OBS_SCENE_PREPARED);
    assert(!obs_pending_scene_submitted(&journal, 400));
    assert(!obs_pending_scene_observe(&journal, t, OBS_SCENE_FENCE_SIGNALED));
    ObsGeometryReservation out = { .base = { 777 } };
    assert(!obs_pending_scene_publish(&journal, t, &out));
    assert(out.base[0] == 777);
    assert(obs_pending_scene_cancel(&journal, t));
    assert(journal.highwater[0] == 5 && journal.live == 0);
    /* Triangle stream is byte-addressed: first offset 5 rounds to 8;
       3 source bytes occupy 4 reserved bytes, not 3. */
    assert(journal.highwater[OBS_GEOM_MESHLET_TRIANGLE] == 5);
    assert(obs_pending_scene_reserve(&journal, 99, n, &t));
    assert(obs_pending_scene_snapshot(&journal, t, &inspect));
    assert(inspect.geometry.base[OBS_GEOM_MESHLET_TRIANGLE] == 8);
    assert(inspect.geometry.end[OBS_GEOM_MESHLET_TRIANGLE] == 12);
    assert(obs_pending_scene_cancel(&journal, t));
    assert(!obs_pending_scene_cancel(&journal, t));
    assert(obs_pending_scene_reserve(&journal, 1, n, &t));
    assert(t == 3);
    assert(obs_pending_scene_submitted(&journal, t));
    assert(!obs_pending_scene_cancel(&journal, t));
    assert(!obs_pending_scene_observe(&journal, t, (ObsPendingSceneFence)19));
    assert(obs_pending_scene_observe(&journal, t, OBS_SCENE_FENCE_PENDING));
    assert(!obs_pending_scene_publish(&journal, t, &out));
    assert(obs_pending_scene_observe(&journal, t, OBS_SCENE_FENCE_SIGNALED));
    assert(!obs_pending_scene_observe(&journal, t, OBS_SCENE_FENCE_SIGNALED));
    assert(obs_pending_scene_publish(&journal, t, &out));
    assert(out.base[0] == 5 && out.end[0] == 8);
    assert(!obs_pending_scene_submitted(&journal, t));
    assert(obs_pending_scenes_drain(&journal));
    assert(journal.published[0] == 8 && journal.highwater[0] == 8);

    /* Init rejection leaves previous state intact. */
    b.pools[OBS_GEOM_BOUNDS].used = 4;
    assert(!obs_pending_scenes_init(&journal, &b));
    assert(journal.highwater[0] == 8);
    b = budget(5, 4);
    assert(!obs_pending_scenes_init(&journal, &b));
    assert(journal.highwater[0] == 8);
}

/// Adversarial completion, unknown fences, and out-of-order publication
static void scenes(void) {
    ObsPendingScenes journal = {0};
    ObsGeometryBudget b = budget(0, 1000);
    assert(obs_pending_scenes_init(&journal, &b));
    uint32_t n[OBS_GEOM_POOL_COUNT]; counts(n, 10);
    uint64_t a = 0, c = 0, d = 0;
    assert(obs_pending_scene_reserve(&journal, 1, n, &a));
    assert(obs_pending_scene_reserve(&journal, 2, n, &c));
    assert(obs_pending_scene_reserve(&journal, 3, n, &d));
    assert(journal.highwater[0] == 30);
    assert(obs_pending_scene_submitted(&journal, a));
    assert(obs_pending_scene_submitted(&journal, c));
    assert(obs_pending_scene_submitted(&journal, d));
    assert(!obs_pending_scenes_drain(&journal));
    assert(obs_pending_scene_observe(&journal, a, OBS_SCENE_FENCE_UNKNOWN));
    assert(!obs_pending_scene_observe(&journal, a, OBS_SCENE_FENCE_SIGNALED));
    assert(!obs_pending_scene_retire_quiesced(&journal, a, false));
    assert(!obs_pending_scene_publish(&journal, a, &(ObsGeometryReservation){0}));
    assert(obs_pending_scene_observe(&journal, d, OBS_SCENE_FENCE_SIGNALED));
    ObsGeometryReservation r;
    assert(obs_pending_scene_publish(&journal, d, &r));
    assert(r.base[0] == 20 && r.end[0] == 30);
    assert(obs_pending_scene_observe(&journal, c, OBS_SCENE_FENCE_SIGNALED));
    assert(obs_pending_scene_publish(&journal, c, &r));
    assert(r.base[0] == 10 && r.end[0] == 20);
    assert(obs_pending_scene_retire_quiesced(&journal, a, true));
    assert(obs_pending_scenes_drain(&journal));
    assert(journal.highwater[0] == 30);
    assert(journal.published[0] == 30);
    assert(obs_pending_scene_reserve(&journal, 4, n, &a));
    assert(journal.highwater[0] == 40);
    assert(obs_pending_scene_cancel(&journal, a));
    assert(journal.highwater[0] == 30);
}

/// Bounded slots and capacity, including non-tail holes
static void capacity(void) {
    ObsPendingScenes j = {0};
    ObsGeometryBudget b = budget(0, 1024);
    assert(obs_pending_scenes_init(&j, &b));
    uint32_t n[OBS_GEOM_POOL_COUNT]; counts(n, 1);
    uint64_t ts[OBS_PENDING_SCENES_MAX];
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i)
        assert(obs_pending_scene_reserve(&j, i+1, n, &ts[i]));
    assert(!obs_pending_scene_reserve(&j, 99, n, &(uint64_t){0}));
    assert(j.live == OBS_PENDING_SCENES_MAX);
    /* A cancelled interior gap is not reused while a later upload owns data. */
    assert(obs_pending_scene_cancel(&j, ts[0]));
    assert(j.highwater[0] == OBS_PENDING_SCENES_MAX);
    assert(obs_pending_scene_reserve(&j, 99, n, &(uint64_t){0}));
    assert(j.highwater[0] == OBS_PENDING_SCENES_MAX+1);
    for (unsigned i = 1; i < OBS_PENDING_SCENES_MAX; ++i)
        assert(obs_pending_scene_cancel(&j, ts[i]));
    /* Last issued reservation can roll back, earlier tombstones can't alias it. */
    assert(j.live == 1);
    assert(!obs_pending_scenes_drain(&j));
    ObsPendingScene last = {0};
    assert(obs_pending_scene_snapshot(&j, j.serial, &last));
    assert(obs_pending_scene_cancel(&j, last.ticket));
    assert(obs_pending_scenes_drain(&j));
    assert(j.highwater[0] == 0);
    j.serial = UINT64_MAX;
    assert(!obs_pending_scene_reserve(&j, 7, n, &(uint64_t){0}));
}

/// 100k randomized scene lifecycles and cross-pool disjointness
static void stress(void) {
    ObsGeometryBudget b = budget(11, 250000);
    ObsPendingScenes j = {0};
    assert(obs_pending_scenes_init(&j, &b));
    uint64_t state = UINT64_C(0xc0deface19283745);
    uint64_t tickets[OBS_PENDING_SCENES_MAX] = {0};
    uint32_t counts_buf[OBS_GEOM_POOL_COUNT];
    uint32_t max_seen[OBS_GEOM_POOL_COUNT] = {0};
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        max_seen[i] = 11;
    for (unsigned step = 0; step < 100000; ++step) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        unsigned slot = (unsigned)(state % OBS_PENDING_SCENES_MAX);
        unsigned op = (unsigned)((state >> 16) % 6);
        if (tickets[slot] == 0) {
            unsigned count = 1 + (unsigned)((state >> 32) % 3);
            counts(counts_buf, count);
            uint64_t ticket = 0;
            if (obs_pending_scene_reserve(&j, slot + 1, counts_buf, &ticket)) {
                tickets[slot] = ticket;
                ObsPendingScene entry;
                assert(obs_pending_scene_snapshot(&j, ticket, &entry));
                for (unsigned k = 0; k < OBS_GEOM_POOL_COUNT; ++k) {
                    /* No reservation overlaps an earlier reservation unless
                       an explicitly cancelled tail has been safely rewound. */
                    assert(entry.geometry.end[k] > entry.geometry.base[k]);
                    assert(entry.geometry.end[k] == j.highwater[k]);
                }
            }
            continue;
        }
        ObsPendingScene e;
        assert(obs_pending_scene_snapshot(&j, tickets[slot], &e));
        switch (e.state) {
        case OBS_SCENE_PREPARED:
            if (op & 1) assert(obs_pending_scene_submitted(&j, tickets[slot]));
            else {
                assert(obs_pending_scene_cancel(&j, tickets[slot]));
                tickets[slot] = 0;
            }
            break;
        case OBS_SCENE_SUBMITTED:
            if (op <= 2) assert(obs_pending_scene_observe(&j, tickets[slot], OBS_SCENE_FENCE_PENDING));
            else if (op == 3) assert(obs_pending_scene_observe(&j, tickets[slot], OBS_SCENE_FENCE_UNKNOWN));
            else assert(obs_pending_scene_observe(&j, tickets[slot], OBS_SCENE_FENCE_SIGNALED));
            break;
        case OBS_SCENE_READY: {
            ObsGeometryReservation r;
            assert(obs_pending_scene_publish(&j, tickets[slot], &r));
            for (unsigned k = 0; k < OBS_GEOM_POOL_COUNT; ++k) {
                assert(r.end[k] <= j.highwater[k]);
                assert(j.published[k] >= r.end[k]);
                if (j.published[k] > max_seen[k]) max_seen[k] = j.published[k];
            }
            tickets[slot] = 0;
            break;
        }
        case OBS_SCENE_UNKNOWN:
            assert(!obs_pending_scene_retire_quiesced(&j, tickets[slot], false));
            if (op == 5) {
                assert(obs_pending_scene_retire_quiesced(&j, tickets[slot], true));
                tickets[slot] = 0;
            }
            break;
        default: assert(0);
        }
        assert(j.live <= OBS_PENDING_SCENES_MAX);
        if (!j.live) assert(obs_pending_scenes_drain(&j));
        for (unsigned k = 0; k < OBS_GEOM_POOL_COUNT; ++k) {
            assert(j.highwater[k] >= j.published[k]);
            assert(j.published[k] >= max_seen[k]);
        }
    }
    /* All outstanding submitted work is declared quiescent by the simulated
       device. Never do this after a real timeout without an actual proof. */
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i) {
        if (!tickets[i]) continue;
        ObsPendingScene e;
        assert(obs_pending_scene_snapshot(&j, tickets[i], &e));
        assert(obs_pending_scene_retire_quiesced(&j, tickets[i], true));
    }
    assert(obs_pending_scenes_drain(&j));
    printf("geometry-pending: 100,000 scene lifecycle cases passed (final cursor %" PRIu32 ")\n", j.highwater[0]);
}

int main(void) {
    validation(); scenes(); capacity(); stress();
    return 0;
}
