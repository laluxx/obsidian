/* Cycle 66: production undo journal across bounded inverse-record eviction. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static Scene scene_storage = {
    .sdf_lifetime_epoch = 1,
    .tree = {.count = 1}
};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];
void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name,
                            int32_t parent, int32_t mesh_index) {
    assert(name && parent >= 0 && mesh_index == -1);
    if (tree->count >= SCENE_TREE_MAX_NODES) return -1;
    int32_t index = tree->count++;
    tree->nodes[index].sdf_index = -1;
    return index;
}

static uint64_t ids[64];
static uint64_t rng_state = UINT64_C(0x4d595df4d0f33173);
static unsigned rng(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (unsigned)((x * UINT64_C(2685821657736338717)) >> 32);
}
static void fresh_n(unsigned count) {
    assert(count <= 64);
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    for (unsigned i = 0; i < count; ++i)
        assert(obs_sculpt_sphere((double[]){(double)(i * 20),0,0}, .5,
                                 (double[]){.3,.5,.7}, &ids[i]));
}
static void fresh(void) { fresh_n(4); }
static double pos(unsigned i) {
    double x = -1;
    assert(obs_sculpt_x(ids[i], &x));
    return x;
}
static void take(double dst[4]) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = pos(i);
}
static void equal_positions(const double expected[4], int round,
                            unsigned action, unsigned iteration) {
    for (unsigned i = 0; i < 4; ++i)
        if (pos(i) != expected[i]) {
            fprintf(stderr,
                    "Mismatch round=%d action=%u iter=%u object=%u expected=%g actual=%g rng=%" PRIu64 "\n",
                    round, action, iteration, i, expected[i], pos(i), rng_state);
            abort();
        }
}
static void select_i(unsigned i) {
    assert(obs_sdf_inspector_select(ids[i]) == OBS_SDF_EDIT_OK);
}
static void edit_i(unsigned i, double x) {
    select_i(i);
    assert(obs_sdf_inspector_move((double[]){x,0,0}) == OBS_SDF_EDIT_OK);
}

/* A 64-object command fills the history. Undo evicts all original roots;
 * redo must recreate a complete undoable command from its inverse snapshots. */
static void large_group_recovery(void) {
    fresh_n(64);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 0; i < 64; ++i)
        edit_i(i, (double)(i * 20 + 1));
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    for (unsigned repeat = 0; repeat < 12; ++repeat) {
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
        for (unsigned i = 0; i < 64; ++i)
            assert(pos(i) == (double)(i * 20));
        assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
        for (unsigned i = 0; i < 64; ++i)
            assert(pos(i) == (double)(i * 20 + 1));
        assert(obs_sdf_inspector_state().undo_count == 1);
        unsigned slot = repeat % MAX_FRAMES_IN_FLIGHT;
        assert(obs_sdf_sync_frame(slot));
        for (unsigned i = 0; i < 64; ++i)
            assert(gpu[slot][i].inverseTransform[3][0] ==
                   -(float)(i * 20 + 1));
    }
}

/* A selected 40-object command is evicted by inverse records while 24 later
 * unrelated edits fill the journal. Redo recovers the group without losing
 * those unrelated values or future selected-object undo. */
static void selective_eviction_recovery(void) {
    fresh_n(64);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 0; i < 40; ++i)
        edit_i(i, (double)(i * 20 + 2));
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 40; i < 64; ++i)
        edit_i(i, (double)(i * 20 + 3));
    select_i(0);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    for (unsigned i = 0; i < 40; ++i)
        assert(pos(i) == (double)(i * 20));
    for (unsigned i = 40; i < 64; ++i)
        assert(pos(i) == (double)(i * 20 + 3));
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    for (unsigned i = 0; i < 40; ++i)
        assert(pos(i) == (double)(i * 20 + 2));
    for (unsigned i = 40; i < 64; ++i)
        assert(pos(i) == (double)(i * 20 + 3));
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    for (unsigned i = 0; i < 40; ++i)
        assert(pos(i) == (double)(i * 20));
    for (unsigned i = 40; i < 64; ++i)
        assert(pos(i) == (double)(i * 20 + 3));
}

int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    large_group_recovery();
    selective_eviction_recovery();
    for (int round = 0; round < 120; ++round) {
        fresh();
        for (unsigned iteration = 0; iteration < 1200; ++iteration) {
            unsigned action = rng() % 11, i = rng() % 4;
            double before[4];
            take(before);
            ObsSdfEditStatus status = OBS_SDF_EDIT_OK;
            switch (action) {
            case 0: case 1: case 2: case 3:
                edit_i(i, (double)(i * 20) + (double)((rng() % 200) + 1));
                break;
            case 4: {
                assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
                unsigned count = 1 + rng() % 8;
                for (unsigned j = 0; j < count; ++j)
                    edit_i(rng() % 4, (double)(rng() % 200) + 200);
                assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
                break;
            }
            case 5: case 6:
                select_i(i);
                status = obs_sdf_inspector_undo_selected();
                break;
            case 7:
                status = obs_sdf_inspector_undo();
                break;
            case 8:
                status = obs_sdf_inspector_undo_only();
                break;
            case 9:
                status = obs_sdf_inspector_redo();
                break;
            case 10:
                obs_sdf_inspector_command_boundary();
                break;
            }
            if (status == OBS_SDF_EDIT_CONFLICT ||
                status == OBS_SDF_EDIT_STALE ||
                status == OBS_SDF_EDIT_EMPTY ||
                status == OBS_SDF_EDIT_INVALID)
                equal_positions(before, round, action, iteration);
            if ((action == 5 || action == 6 || action == 7 || action == 8) &&
                status == OBS_SDF_EDIT_OK) {
                ObsSdfEditStatus redo = obs_sdf_inspector_redo();
                if (redo != OBS_SDF_EDIT_OK) {
                    fprintf(stderr,
                            "undo+redo failure round=%d iter=%u action=%u redo=%d\n",
                            round, iteration, action, redo);
                    abort();
                }
                equal_positions(before, round, action, iteration);
            }
            if (iteration % 41 == 0) {
                unsigned slot = iteration % MAX_FRAMES_IN_FLIGHT;
                assert(obs_sdf_sync_frame(slot));
                for (unsigned j = 0; j < 4; ++j)
                    assert(gpu[slot][j].inverseTransform[3][0] ==
                           scene->sdfs[j].inverseTransform[3][0]);
            }
        }
    }
    free(scene->sdfs);
    puts("Cycle 66 adversarial: 144000 mixed scene/history transitions PASS");
    return 0;
}
