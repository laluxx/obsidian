/* Cycle 67: selected-object undo is not allowed to invalidate later active
 * commands even when their net scene value happens to match an older edit.
 * Links the production journal/scene/renderer SSBO ownership code. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

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
static uint64_t ids[64];
static void fresh(unsigned count) {
    assert(count <= 64);
    free(scene->sdfs); scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    for (unsigned i = 0; i < count; ++i)
        assert(obs_sculpt_sphere((double[]){20.0 * i, 0, 0}, .5,
                                 (double[]){.3, .6, .8}, &ids[i]));
}
static double pos(unsigned i) {
    double x = -100;
    assert(obs_sculpt_x(ids[i], &x));
    return x;
}
static void edit(unsigned i, double x) {
    assert(obs_sdf_inspector_select(ids[i]) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_move((double[]){x, 0, 0}) == OBS_SDF_EDIT_OK);
}
static void select_i(unsigned i) {
    assert(obs_sdf_inspector_select(ids[i]) == OBS_SDF_EDIT_OK);
}
static void check_slot(unsigned i) {
    unsigned slot = i % MAX_FRAMES_IN_FLIGHT;
    assert(obs_sdf_sync_frame(slot));
    for (unsigned j = 0; j < scene->sdf_count; ++j)
        assert(gpu[slot][j].inverseTransform[3][0] ==
               scene->sdfs[j].inverseTransform[3][0]);
}
/* Prior version lets selected undo reverse A+B when A has newer LIVE
 * commands A:1->2->1. The scene equals the old postimage but the historical
 * dependency is NOT independent. The following later undo would then fail. */
static void returned_value_collision(void) {
    fresh(3);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    edit(0, 1); edit(1, 21);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    edit(0, 2); edit(0, 1);  /* newer commands cancel visually */
    edit(2, 41);             /* independent later command */
    select_i(1);
    ObsSdfInspectorState prior = obs_sdf_inspector_state();
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_CONFLICT);
    assert(pos(0) == 1 && pos(1) == 21 && pos(2) == 41);
    assert(obs_sdf_inspector_state().undo_count == prior.undo_count);
    /* Other object's independent later edit is still normally undoable. */
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 1 && pos(1) == 21 && pos(2) == 40);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 2 && pos(1) == 21);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 1 && pos(1) == 21);
    /* A's newer original edits are now inactive: the grouped ancestor is
     * independent of all remaining active commands and can be undone. */
    select_i(1);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 0 && pos(1) == 20);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 1 && pos(1) == 21);
    check_slot(0);
}
/* An unrelated later active edit is not a dependency: selected undo must
 * continue to be useful in multi-object scenes with interleaved edits. */
static void unrelated_edit(void) {
    fresh(3);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    edit(0, 1); edit(1, 21);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    edit(2, 42);
    select_i(1);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 0 && pos(1) == 20 && pos(2) == 42);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(pos(0) == 1 && pos(1) == 21 && pos(2) == 42);
}
/* Even a single ticket of a large atomic command creates a dependency.
 * Run many independent scene epochs so stale identities cannot shortcut the
 * check. An active later command can revisit its earlier numerical value. */
static void grouped_stress(void) {
    for (unsigned round = 0; round < 4000; ++round) {
        unsigned count = 2 + round % 29;
        fresh(count);
        assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
        for (unsigned i = 0; i < count; ++i)
            edit(i, i * 20.0 + 1);
        assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
        unsigned i = (round * 17u) % count;
        edit(i, i * 20.0 + 2);
        edit(i, i * 20.0 + 1);
        unsigned selection = (i + 1) % count;
        select_i(selection);
        assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_CONFLICT);
        for (unsigned j = 0; j < count; ++j)
            assert(pos(j) == j * 20.0 + 1);
        /* Recover by undoing both newer commands; then atomically undo the
         * entire original command and redo it. */
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
        assert(pos(i) == i * 20.0 + 2);
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
        assert(pos(i) == i * 20.0 + 1);
        select_i(selection);
        assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
        for (unsigned j = 0; j < count; ++j)
            assert(pos(j) == j * 20.0);
        assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
        for (unsigned j = 0; j < count; ++j)
            assert(pos(j) == j * 20.0 + 1);
        if ((round % 31) == 0) check_slot(round);
    }
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    returned_value_collision();
    unrelated_edit();
    grouped_stress();
    free(scene->sdfs);
    puts("Cycle 67 selective dependency: 4000 real-scene group traces PASS");
    return 0;
}
