/* Cycle 65: production scene/journal/GPU path with scene-snapshot oracles. */
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

static uint64_t id[3];
static uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
static uint32_t rnd(void) {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return (uint32_t)((seed * UINT64_C(2685821657736338717)) >> 32);
}
static void fresh(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    for (unsigned i = 0; i < 3; ++i)
        assert(obs_sculpt_sphere((double[]){(double)(i * 5),0,0}, .5,
                                 (double[]){.2,.4,.6}, &id[i]));
    assert(obs_sdf_inspector_state().undo_count == 0);
}
static void move(unsigned object, double x) {
    assert(object < 3);
    assert(obs_sdf_inspector_select(id[object]) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){x,0,0}) == OBS_SDF_EDIT_OK);
}
static double xof(unsigned object) {
    double x = -777;
    assert(obs_sculpt_x(id[object], &x));
    return x;
}
static void positions(double x[3]) {
    for (unsigned i = 0; i < 3; ++i) x[i] = xof(i);
}
static void check(const double x[3], unsigned iteration, const char *phase) {
    for (unsigned i = 0; i < 3; ++i)
        if (xof(i) != x[i]) {
            fprintf(stderr,
                    "iteration %u %s object %u expected %.0f actual %.0f seed=%" PRIu64 "\n",
                    iteration, phase, i, x[i], xof(i), seed);
            abort();
        }
}
static void verify_gpu(unsigned slot) {
    assert(obs_sdf_sync_frame(slot));
    for (unsigned i = 0; i < 3; ++i)
        assert(gpu[slot][i].inverseTransform[3][0] ==
               scene->sdfs[i].inverseTransform[3][0]);
}

static void masked_intervening_mutation(void) {
    fresh();
    move(0, 1);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(0, 2);
    assert(obs_sculpt_move(id[0], (double[]){9,0,0}));
    ObsSdfInspectorState previous = obs_sdf_inspector_state();
    assert(obs_sdf_inspector_select(id[0]) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){3,0,0}) ==
           OBS_SDF_EDIT_CONFLICT);
    assert(xof(0) == 9);
    assert(obs_sdf_inspector_group_active());
    assert(obs_sdf_inspector_state().undo_count == previous.undo_count);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_CONFLICT);
    /* Restoring the last previewed value makes cancellation safe again. */
    assert(obs_sculpt_move(id[0], (double[]){2,0,0}));
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    assert(xof(0) == 1);
    assert(obs_sdf_inspector_state().undo_count == 1);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(xof(0) == 0);
}

static void net_zero_and_interleaving(void) {
    fresh();
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(0, 1); move(1, 6); move(0, 0); move(1, 5);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == 0);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_EMPTY);

    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(0, 1); move(1, 6); move(0, 2); move(1, 5); move(0, 3);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == 1);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(xof(0) == 0 && xof(1) == 5);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(xof(0) == 3 && xof(1) == 5);
}

static void capacity_preserved(void) {
    fresh();
    for (unsigned i = 1; i <= 63; ++i) move(0, (double)i);
    assert(obs_sdf_inspector_state().undo_count == 63);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 64; i <= 127; ++i) move(0, (double)i);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == OBS_SDF_EDIT_HISTORY_CAP);
    for (unsigned i = 0; i < 16; ++i)
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(xof(0) == 48);
    verify_gpu(0);
}

static void randomized_roundtrips(void) {
    fresh();
    for (unsigned iteration = 0; iteration < 4000; ++iteration) {
        if (iteration % 97 == 0) fresh();
        double before[3], after[3];
        positions(before);
        unsigned steps = 1u + rnd() % 64u;
        uint32_t undo_before = obs_sdf_inspector_state().undo_count;
        assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
        for (unsigned k = 0; k < steps; ++k) {
            unsigned object = rnd() % 3u;
            double x = (double)((int)(rnd() % 101u) - 50);
            move(object, x);
        }
        positions(after);
        if (iteration % 13 == 0) {
            assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
            check(before, iteration, "cancel");
            assert(obs_sdf_inspector_state().undo_count == undo_before);
        } else {
            assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
            bool changed = false;
            for (unsigned i = 0; i < 3; ++i)
                changed |= before[i] != after[i];
            if (changed) {
                assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
                check(before, iteration, "undo");
                assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
                check(after, iteration, "redo");
            } else {
                assert(obs_sdf_inspector_state().undo_count == undo_before);
            }
        }
        if (iteration % 17 == 0)
            verify_gpu(iteration % MAX_FRAMES_IN_FLIGHT);
        assert(obs_sdf_inspector_state().undo_count <=
               OBS_SDF_EDIT_HISTORY_CAP);
    }
}

int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    masked_intervening_mutation();
    net_zero_and_interleaving();
    capacity_preserved();
    randomized_roundtrips();
    free(scene->sdfs);
    puts("Cycle 65 undo transactions: 4000 snapshot oracles, 64-slot retention, masked conflicts PASS");
    return 0;
}
