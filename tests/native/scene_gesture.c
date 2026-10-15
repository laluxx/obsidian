/* Cycle 54: source-bound transactional inspector gestures (no shadow scene). */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
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
static uint64_t a,b;
static void fresh(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    assert(obs_sculpt_sphere((double[]){0,0,0}, 0.5,
                             (double[]){1,0.3,0.4}, &a));
    assert(obs_sculpt_sphere((double[]){5,0,0}, 0.5,
                             (double[]){0.2,0.4,0.5}, &b));
    assert(obs_sdf_inspector_select(a) == OBS_SDF_EDIT_OK);
}
static void move(uint64_t t, double x) {
    assert(obs_sdf_inspector_select(t) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){x,0,0}) == OBS_SDF_EDIT_OK);
}
static void check(uint64_t ticket, double x) {
    double current;
    assert(obs_sculpt_x(ticket, &current));
    assert(current == x);
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) sdfSSBOMapped[i] = gpu[i];
    fresh(); move(a, 1);
    const uint32_t baseline = obs_sdf_inspector_state().undo_count;
    assert(baseline == 1);
    /* Live preview is visible, but invisible to the committed undo history. */
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(a, 2); move(a, 3); move(b, 6);
    /* The preview is published to a real, fence-safe SDF slot even before
     * commit; the second slot receives cancellation on its own safe fence. */
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][0].inverseTransform[3][0] == -3);
    assert(gpu[0][1].inverseTransform[3][0] == -6);
    assert(obs_sdf_inspector_state().undo_count == baseline);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_clear() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    check(a, 1); check(b, 5);
    assert(obs_sdf_sync_frame(1));
    assert(gpu[1][0].inverseTransform[3][0] == -1);
    assert(gpu[1][1].inverseTransform[3][0] == -5);
    assert(obs_sdf_inspector_state().undo_count == baseline);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK); check(a, 0);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK); check(a, 1);

    /* Commit is one reversible command, spanning multiple object identities. */
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(a, 3); move(b, 6);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == 2);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    check(a,1); check(b,5);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    check(a,3); check(b,6);

    /* One outside mutation must not cause a half-rollback of the other object.
     * The session stays open, recoverable once the conflicting value returns. */
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(a, 7); move(b, 8);
    assert(obs_sculpt_move(a, (double[]){99,0,0}));
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_CONFLICT);
    check(a,99); check(b,8);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_CONFLICT);
    assert(obs_sdf_inspector_state().undo_count == 2);
    assert(obs_sculpt_move(a, (double[]){7,0,0}));
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    check(a,3); check(b,6);
    assert(obs_sdf_inspector_state().undo_count == 2);

    /* No-op previews do not consume budget. The 65th real change must fail
     * BEFORE touching scene state, even with a full uncommitted gesture. */
    fresh();
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 1; i <= OBS_SDF_EDIT_HISTORY_CAP; ++i) move(a, (double)i);
    assert(obs_sdf_inspector_move((double[]){64,0,0}) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){65,0,0}) == OBS_SDF_EDIT_FULL);
    check(a,64);
    assert(obs_sdf_inspector_state().undo_count == 0);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    check(a,0); assert(obs_sdf_inspector_state().undo_count == 0);

    /* Entire oldest groups, never their individual entries, are evicted. */
    fresh();
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(a,1); move(a,2); move(a,3);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    for (unsigned i = 4; i <= 65; ++i) move(a,(double)i);
    assert(obs_sdf_inspector_state().undo_count == 63);
    /* Inverse records consume the same 64-slot journal, so only a bounded
     * prefix of these changes can be traversed during a long undo sequence. */
    unsigned undone = 0;
    for (unsigned i = 65; i > 3; --i) {
        ObsSdfEditStatus result = obs_sdf_inspector_undo();
        if (result == OBS_SDF_EDIT_EMPTY) break;
        assert(result == OBS_SDF_EDIT_OK);
        check(a, i - 1);
        ++undone;
    }
    assert(undone >= 20 && undone < 62);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_EMPTY);
    double current;
    assert(obs_sculpt_x(a, &current) && current >= 3);
    /* The evicted 3-edit original group is never exposed in fragments. */

    /* Thousands of entire gestures, alternating accept/undo/redo and abort,
     * exercise tickets, staged inverse order and bounded history lifetimes. */
    for (unsigned i = 0; i < 2000; ++i) {
        fresh();
        double px = (double)(i % 23 + 1), bx = (double)(i % 17 + 6);
        assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
        move(a, px); move(b, bx);
        if (i & 1) {
            assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
            check(a, 0); check(b, 5);
            assert(obs_sdf_inspector_state().undo_count == 0);
        } else {
            assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
            assert(obs_sdf_inspector_state().undo_count == 1);
            assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
            check(a, 0); check(b, 5);
            assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
            check(a, px); check(b, bx);
        }
    }

    /* Discarding a group is not scene reset. Epoch reset does invalidate it. */
    fresh(); assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move(a,1);
    fresh();
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_INVALID);
    check(a,0);
    assert(obs_sdf_inspector_state().undo_count == 0);
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][0].inverseTransform[3][0] == 0);
    free(scene->sdfs);
    puts("Cycle 54 transactional scene gesture / cancellation / grouped eviction PASS");
    return 0;
}
