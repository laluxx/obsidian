/* Cycle 63: production scene/journal/SSBO object-scoped undo regression. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static Scene scene_storage = {.sdf_lifetime_epoch = 1, .tree = {.count = 1}};
Scene *scene = &scene_storage;
SdfPrimitive *sdfSSBOMapped[MAX_FRAMES_IN_FLIGHT];
uint32_t active_sdf_count;
static SdfPrimitive gpu[MAX_FRAMES_IN_FLIGHT][MAX_SDF_PRIMITIVES];
static uint64_t a, b;

void scene_bump_sdf_revision(Scene *s) { ++s->sdf_revision; }
int32_t scene_tree_add_node(SceneTree *tree, const char *name,
                            int32_t parent, int32_t mesh_index) {
    assert(name && parent >= 0 && mesh_index == -1);
    if (tree->count >= SCENE_TREE_MAX_NODES) return -1;
    const int32_t i = (int32_t)tree->count++;
    tree->nodes[i].sdf_index = -1;
    return i;
}

static void reset(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    assert(obs_sculpt_sphere((double[3]){0, 0, 0}, .5,
                             (double[3]){.7, .2, .3}, &a));
    assert(obs_sculpt_sphere((double[3]){5, 0, 0}, .5,
                             (double[3]){.2, .7, .3}, &b));
    assert(obs_sdf_inspector_select(a) == OBS_SDF_EDIT_OK);
}
static void select_object(uint64_t id) {
    assert(obs_sdf_inspector_select(id) == OBS_SDF_EDIT_OK);
}
static void move_object(uint64_t id, double x) {
    select_object(id);
    ObsSdfEditValues v = {.xyz = {x, 0, 0}};
    assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
}
static void check_x(uint64_t id, double expected) {
    double x = -1e9;
    assert(obs_sculpt_x(id, &x) && x == expected);
}
static void interleaved_history(void) {
    reset();
    move_object(a, 1);
    move_object(b, 6);
    move_object(a, 2);
    move_object(b, 7);
    select_object(a);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 1);
    check_x(b, 7);
    /* Explicit redo restores only the selected object's skipped command. */
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    check_x(a, 2);
    check_x(b, 7);
    select_object(a);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 1);
    assert(obs_sdf_inspector_state().undo_count == 3);
    /* Ordinary history traverses the unrelated newer B edit and skips the
     * already-inactive A command instead of attempting a conflicting replay. */
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    check_x(a, 1);
    check_x(b, 6);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    check_x(b, 5);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    check_x(a, 0);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_EMPTY);
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][0].inverseTransform[3][0] == 0);
    assert(gpu[0][1].inverseTransform[3][0] == -5);
}
static void groups_remain_atomic(void) {
    reset();
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    move_object(a, 1);
    move_object(b, 6);
    move_object(a, 2);
    assert(obs_sdf_inspector_end_group() == OBS_SDF_EDIT_OK);
    move_object(b, 7);
    select_object(a);
    const uint32_t history = obs_sdf_inspector_state().undo_count;
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_CONFLICT);
    assert(obs_sdf_inspector_state().undo_count == history);
    check_x(a, 2);
    check_x(b, 7);
    select_object(b);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 2);
    check_x(b, 6);
    select_object(a);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 0);
    check_x(b, 5);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    check_x(a, 2);
    check_x(b, 6);
}
static void conflicts_and_lifetime(void) {
    reset();
    move_object(a, 1);
    move_object(b, 6);
    select_object(a);
    assert(obs_sculpt_move(a, (double[3]){9, 0, 0}));
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_CONFLICT);
    check_x(a, 9);
    check_x(b, 6);
    assert(obs_sculpt_move(a, (double[3]){1, 0, 0}));
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 0);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    const uint64_t stale = a;
    reset();
    assert(obs_sdf_inspector_select(stale) == OBS_SDF_EDIT_STALE);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_EMPTY);
}
static void bounded_history(void) {
    reset();
    for (unsigned i = 1; i <= 80; ++i)
        move_object((i & 1u) ? a : b,
                    (double)(i + ((i & 1u) ? 0 : 5)));
    select_object(a);
    assert(obs_sdf_inspector_undo_selected() == OBS_SDF_EDIT_OK);
    check_x(a, 77);
    check_x(b, 85);
    assert(obs_sdf_inspector_state().undo_count <=
           OBS_SDF_EDIT_HISTORY_CAP);
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    interleaved_history();
    groups_remain_atomic();
    conflicts_and_lifetime();
    bounded_history();
    free(scene->sdfs);
    puts("Cycle 63 selected-object undo: history, atomic groups, conflict and GPU slot PASS");
    return 0;
}
