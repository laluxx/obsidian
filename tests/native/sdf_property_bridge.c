/* Cycle 57: GUI-to-journal bridge tests using production scene, SDF storage,
 * journal, tokens and adapter C code. No simulated undo implementation. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_property_bridge.h"
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
static uint64_t create(double x) {
    uint64_t ticket = 0;
    assert(obs_sculpt_sphere((double[]){x,0,0}, 0.5,
                             (double[]){0.4,0.4,0.4}, &ticket));
    return ticket;
}
static void fresh(void) {
    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    scene->tree.count = 1;
    ++scene->sdf_lifetime_epoch;
}
static double pos(uint64_t ticket) {
    double x;
    assert(obs_sculpt_x(ticket, &x));
    return x;
}
int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) sdfSSBOMapped[i] = gpu[i];
    uint64_t a = create(0), b = create(5);
    ObsSdfEditValues change = {.xyz = {2,1,-3}, .rgba = {.1,.2,.3,.9},
                               .metallic = .6, .roughness = .2};
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL,
                                  &change) == OBS_SDF_EDIT_OK);
    assert(pos(a) == 2);
    assert(scene->sdfs[0].color[0] > .099f && scene->sdfs[0].metallic > .59f);
    assert(obs_sdf_inspector_state().undo_count == 1);
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][0].inverseTransform[3][0] == -2);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(pos(a) == 0 && fabsf(scene->sdfs[0].metallic) < .001f);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(pos(a) == 2 && scene->sdfs[0].color[0] > .099f);
    const unsigned old_count = obs_sdf_inspector_state().undo_count;
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL,
                                  &change) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == old_count);
    /* Editing another sphere selects its own stable token and does not
     * accidentally mutate or undo the first sphere. */
    change.xyz[0] = 7;
    assert(obs_sdf_property_apply(1, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_OK);
    uint64_t selected = 0;
    assert(obs_sdf_inspector_selected(&selected) == OBS_SDF_EDIT_OK && selected == b);
    assert(pos(b) == 7 && pos(a) == 2);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK && pos(b) == 5);
    assert(pos(a) == 2);
    /* Forbidden: sidebar edits cannot be merged into an active viewport drag
     * or steal its selection, even if they target another sphere. */
    assert(obs_sdf_inspector_select(a) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_group_active());
    assert(obs_sdf_inspector_drag_move((double[]){3,1,-3}) == OBS_SDF_EDIT_OK);
    change.xyz[0] = 99;
    assert(obs_sdf_property_apply(1, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_CONFLICT);
    assert(pos(b) == 5);
    assert(obs_sdf_inspector_selected(&selected) == OBS_SDF_EDIT_OK && selected == a);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    assert(!obs_sdf_inspector_group_active() && pos(a) == 2);
    /* Numeric validation rejects without dirtying production geometry. */
    change.xyz[0] = NAN;
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_INVALID);
    assert(pos(a) == 2);
    /* Invalid input must not steal a different currently selected sphere. */
    assert(obs_sdf_inspector_select(b) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_selected(&selected) == OBS_SDF_EDIT_OK && selected == b);
    change.xyz[0] = 42;
    assert(obs_sdf_property_apply(55, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_STALE);
    assert(obs_sdf_property_apply(0, 0, &change) == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, NULL) == OBS_SDF_EDIT_INVALID);
    /* A scene reset revokes all old tickets. The same slot is not the same
     * object, and undo on the new scene must not replay the old command. */
    fresh();
    assert(!obs_sculpt_index(a, &(uint32_t){0}));
    uint64_t c = create(12);
    assert(c != a && c != b);
    change.xyz[0] = 14;
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_OK);
    assert(pos(c) == 14);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK && pos(c) == 12);
    /* UI-added spheres begin without preexisting Monad/agent handles. The
     * panel bridge must mint a ticket lazily and use the same scene journal. */
    fresh();
    vec3 ui_position = {4,0,0};
    vec4 ui_size = {.5f,0,0,0}, ui_color = {.4f,.4f,.4f,1};
    assert(obs_sdf_add_checked(scene, "UI sphere", SDF_TYPE_SPHERE,
                               ui_position, ui_size, ui_color) == 0);
    change.xyz[0] = 9;
    assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_OK);
    uint64_t ui_ticket = obs_sculpt_ticket_at(0);
    assert(ui_ticket && pos(ui_ticket) == 9);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK && pos(ui_ticket) == 4);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK && pos(ui_ticket) == 9);

    /* Repeated complete lifecycle using production scene and renderer SSBO. */
    for (unsigned i = 0; i < 1500; ++i) {
        fresh();
        c = create(0);
        change.xyz[0] = (double)(i % 17 + 1);
        assert(obs_sdf_property_apply(0, OBS_SDF_EDIT_POSITION, &change) == OBS_SDF_EDIT_OK);
        assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK && pos(c) == 0);
        assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK && pos(c) == change.xyz[0]);
    }
    free(scene->sdfs);
    puts("Cycle 57: production sidebar + Emacs undo shared journal PASS (1,500 scene resets)");
    return 0;
}
