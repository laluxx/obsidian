/* Cycle 64: production scene journal plus its input-boundary adapter. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include "scene_undo_input.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

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

static uint64_t sphere;
static void fresh(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
    assert(obs_sculpt_sphere((double[]){0,0,0}, .5,
                             (double[]){.3,.4,.5}, &sphere));
    assert(obs_sdf_inspector_select(sphere) == OBS_SDF_EDIT_OK);
}
static void edit(double x) {
    ObsSdfEditValues v = {.xyz = {x,0,0}};
    assert(obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
}
static void check(double expected) {
    double actual = -9;
    assert(obs_sculpt_x(sphere, &actual) && actual == expected);
}
static void undo(void) {
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
}

int main(void) {
    for (unsigned slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        sdfSSBOMapped[slot] = gpu[slot];

    /* Modifier traffic and releases do not split a repeated undo command. */
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_key(false, OBS_SCENE_KEY_OTHER);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_MODIFIER);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_UNDO);
    undo(); check(0);

    /* A non-undo key or character input starts a new command sequence. */
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    undo(); check(2);
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER); /* text/IME */
    undo(); check(2);

    /* Ordinary boundaries reset traversal but preserve explicit redo and
     * undo-only semantics over the production inverse records. */
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK); check(2);
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    assert(obs_sdf_inspector_undo_only() == OBS_SDF_EDIT_OK); check(0);

    /* Mouse press, real scroll, and focus loss are command boundaries. */
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_mouse_press(); undo(); check(2);
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_scroll(0, 0); undo(); check(0);
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_scroll(0, 1); undo(); check(2);
    fresh(); edit(1); edit(2); undo(); check(1);
    obs_scene_undo_on_focus_lost(); undo(); check(2);

    /* Boundary events neither close nor cancel a speculative transaction. */
    fresh(); edit(1);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    edit(3);
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    obs_scene_undo_on_mouse_press();
    obs_scene_undo_on_focus_lost();
    assert(obs_sdf_inspector_group_active());
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    check(1);
    undo(); check(0);

    for (unsigned j = 0; j < 1000; ++j) {
        fresh(); edit(1); edit(2); undo(); check(1);
        obs_scene_undo_on_key(true, OBS_SCENE_KEY_MODIFIER);
        undo(); check(0);
        obs_scene_undo_on_mouse_press(); undo(); check(1);
        if (j & 1) obs_scene_undo_on_scroll(0, 2);
        else obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
        undo(); check(0);
        assert(obs_sdf_sync_frame(j % MAX_FRAMES_IN_FLIGHT));
        assert(gpu[j % MAX_FRAMES_IN_FLIGHT][0].inverseTransform[3][0] == 0);
    }
    free(scene->sdfs);
    puts("Cycle 64 scene undo input boundaries PASS");
    return 0;
}
