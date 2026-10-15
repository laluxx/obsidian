/* Cycle 58 — the real scene, SDF buffers, shared undo journal, and UI bridge.
 * No shadow editor model or fabricated source-side transaction. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_property_bridge.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int32_t result = tree->count++;
    tree->nodes[result].sdf_index = -1;
    return result;
}
static uint64_t make(double x) {
    uint64_t id = 0;
    assert(obs_sculpt_sphere((double[]){x,0,0}, 1.0,
                             (double[]){.3,.4,.5}, &id));
    return id;
}
static ObsSdfEditValues values(double x) {
    return (ObsSdfEditValues){
        .xyz = {x,1,2}, .rgba = {.3,.4,.5,1}, .metallic = .1,
        .roughness = .7, .radius = 1.0, .smoothness = .5,
        .operation = 1, .emissive = {.1,.2,.3}, .emissive_strength = 1.0};
}
static void assert_source(const SdfPrimitive *s, double x) {
    assert(s->inverseTransform[3][0] == (float)-x);
    assert(s->size[0] > 0);
    assert(isfinite(s->emissiveStrength));
}
int main(void) {
    for (unsigned f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) sdfSSBOMapped[f] = gpu[f];
    const uint32_t full = OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL |
                          OBS_SDF_EDIT_SHAPE | OBS_SDF_EDIT_EMISSION;
    uint64_t a = make(0), b = make(5);
    ObsSdfEditValues v = values(0);

    /* Invalid values never publish or create history. */
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_CONFLICT);
    v.radius = NAN;
    assert(obs_sdf_property_preview(0, full, &v) == OBS_SDF_EDIT_INVALID);
    assert(scene->sdfs[0].size[0] == 1);
    assert(obs_sdf_inspector_state().undo_count == 0);
    v.radius = 1;
    assert(obs_sdf_property_cancel() == OBS_SDF_EDIT_OK);

    /* Twelve thousand live samples cost ONE history record, including
     * simultaneous transform, shape, color and emission changes. */
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    for (unsigned i = 1; i <= 12000; ++i) {
        v.xyz[0] = (double)i / 1000.0;
        v.radius = 1.0 + (double)i / 24000.0;
        v.smoothness = (double)i / 12000.0;
        v.operation = (i & 1) ? 1 : 2;
        v.rgba[0] = (double)i / 12000.0;
        v.emissive[0] = (double)i / 12000.0;
        v.emissive_strength = (double)i / 6.0;
        assert(obs_sdf_property_preview(0, full, &v) == OBS_SDF_EDIT_OK);
        assert(obs_sdf_inspector_state().undo_count == 0);
        assert(scene->sdfs[0].size[0] == (float)v.radius);
    }
    assert(obs_sdf_property_preview(1, full, &v) == OBS_SDF_EDIT_STALE);
    assert(obs_sdf_inspector_state().selection == a);
    assert(obs_sdf_sync_frame(0) && obs_sdf_sync_frame(1));
    assert(gpu[0][0].operation == 2 && gpu[1][0].emissiveStrength == 2000);
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_OK);
    assert(!obs_sdf_property_active());
    assert(obs_sdf_inspector_state().undo_count == 1);
    assert_source(&scene->sdfs[0], 12);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(scene->sdfs[0].size[0] == 1 && scene->sdfs[0].operation == 0);
    assert(scene->sdfs[0].emissive[0] == 0);
    assert_source(&scene->sdfs[0], 0);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert_source(&scene->sdfs[0], 12);
    assert(scene->sdfs[0].size[0] == 1.5f);

    /* Cancel restores a real GPU-visible preview without consuming undo. */
    ObsSdfInspectorState history = obs_sdf_inspector_state();
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    v.radius = .25; v.xyz[0] = 22;
    assert(obs_sdf_property_preview(0, full, &v) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_sync_frame(1) && gpu[1][0].size[0] == .25f);
    assert(obs_sdf_property_cancel() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_sync_frame(0) && gpu[0][0].size[0] == 1.5f);
    assert(scene->sdfs[0].size[0] == 1.5f);
    assert(obs_sdf_inspector_state().undo_count == history.undo_count);

    /* Crossing back to the origin is not a command. */
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    v = values(0);
    v.xyz[0] = 19;
    assert(obs_sdf_property_preview(0, OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
    v.xyz[0] = 12;
    assert(obs_sdf_property_preview(0, OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_state().undo_count == history.undo_count);

    /* Out-of-band conflict refuses both rollback and commit without overwrite. */
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    v.radius = .6;
    assert(obs_sdf_property_preview(0, OBS_SDF_EDIT_SHAPE, &v) == OBS_SDF_EDIT_OK);
    scene->sdfs[0].size[0] = .7f;
    assert(obs_sdf_property_cancel() == OBS_SDF_EDIT_CONFLICT);
    assert(obs_sdf_property_active());
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_CONFLICT);
    assert(scene->sdfs[0].size[0] == .7f);
    scene->sdfs[0].size[0] = .6f;
    assert(obs_sdf_property_cancel() == OBS_SDF_EDIT_OK);
    assert(scene->sdfs[0].size[0] == 1.5f);

    /* A viewport-owned group cannot be joined, even if same sphere selected. */
    assert(obs_sdf_inspector_select(b) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_CONFLICT);
    assert(!obs_sdf_property_active());
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_property_begin(0) == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_begin_group() == OBS_SDF_EDIT_OK);
    assert(!obs_sdf_property_active());
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_group_active());
    assert(obs_sdf_inspector_abort_group() == OBS_SDF_EDIT_OK);

    vec3 box_xyz = {0,0,0};
    vec4 box_size = {1,1,1,0}, box_color = {.3f,.4f,.5f,1};
    int32_t box = obs_sdf_add_checked(scene, "Box", 1, box_xyz, box_size, box_color);
    assert(box >= 0);
    assert(obs_sdf_property_begin((uint32_t)box) == OBS_SDF_EDIT_OK);
    v = (ObsSdfEditValues){.box_extents = {2, 3, 4}};
    assert(obs_sdf_property_preview((uint32_t)box, OBS_SDF_EDIT_BOX, &v) ==
           OBS_SDF_EDIT_OK);
    assert(scene->sdfs[box].size[0] == 2 && scene->sdfs[box].size[1] == 3 &&
           scene->sdfs[box].size[2] == 4);
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(scene->sdfs[box].size[0] == 1 && scene->sdfs[box].size[1] == 1 &&
           scene->sdfs[box].size[2] == 1);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(scene->sdfs[box].size[0] == 2 && scene->sdfs[box].size[1] == 3 &&
           scene->sdfs[box].size[2] == 4);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);

    /* Scene epoch reset revokes the panel token and staged scene history. */
    assert(obs_sdf_property_begin(1) == OBS_SDF_EDIT_OK);
    v = values(9);
    assert(obs_sdf_property_preview(1, OBS_SDF_EDIT_POSITION, &v) == OBS_SDF_EDIT_OK);
    ++scene->sdf_lifetime_epoch;
    assert(!obs_sdf_property_active());
    assert(obs_sdf_property_commit() == OBS_SDF_EDIT_INVALID);
    assert(obs_sdf_inspector_state().undo_count == 0);
    assert(!obs_sculpt_index(b, &(uint32_t){0}));
    free(scene->sdfs); scene->sdfs = NULL;
    puts("Cycle 58: real scene inspector coalescing, geometry, emission, undo PASS");
    return 0;
}
