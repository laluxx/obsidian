/* Cycle 60: real SDF primitive geometry, viewport/journal and frame-slot path.
 * Native GPU buffers are fixture allocations; not proof of Vulkan pixels. */
#define _POSIX_C_SOURCE 200809L
#include "agent_engine_contract.h"
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include "sdf_viewport.h"
#include "sdf_property_bridge.h"
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
    int32_t result = tree->count++;
    tree->nodes[result].sdf_index = -1;
    return result;
}

static uint32_t create(int type, float x, float y, float z, vec4 shape) {
    vec3 xyz = {x, y, z};
    vec4 color = {.4f, .5f, .6f, 1};
    int32_t index = obs_sdf_add_checked(scene, "Cycle60", type, xyz,
                                        shape, color);
    assert(index >= 0);
    return (uint32_t)index;
}

static bool hit(double x, double y, double z, double dx, double dy, double dz,
                ObsSculptRayHit *out) {
    return obs_sculpt_ray_cast_primitive((double[]){x, y, z},
                                         (double[]){dx, dy, dz}, 100, out);
}

static bool press(double px) {
    return obs_sdf_viewport_press((double[]){px, 0, 10},
                                  (double[]){0, 0, -1},
                                  (double[]){0, 0, -1}, 100);
}

static void move_pointer(double px) {
    assert(obs_sdf_viewport_motion((double[]){px, 0, 10},
                                   (double[]){0, 0, -1}));
    assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_OK);
}

int main(void) {
    for (unsigned i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        sdfSSBOMapped[i] = gpu[i];
    uint32_t box = create(SDF_TYPE_BOX, 0, 0, 0, (vec4){1, 2, 3, 0});
    uint32_t sphere = create(SDF_TYPE_SPHERE, 10, 0, 0,
                             (vec4){.5f, 0, 0, 0});
    uint64_t box_ticket = obs_sculpt_ticket_any_at(box);
    uint64_t sphere_ticket = obs_sculpt_ticket_at(sphere);
    assert(box_ticket && sphere_ticket && box_ticket != sphere_ticket);
    assert(obs_sculpt_ticket_at(box) == 0);
    double where[3] = {0};
    assert(obs_sculpt_position_any(box_ticket, where) && where[0] == 0);
    assert(!obs_sculpt_position(box_ticket, where));

    ObsSculptRayHit pick = {.ticket = UINT64_MAX};
    assert(hit(0, 0, 10, 0, 0, -2, &pick));
    assert(pick.ticket == box_ticket && pick.index == box &&
           pick.distance == 7);
    assert(pick.position[2] == 3 && pick.normal[2] == 1);
    assert(hit(0, 0, -10, 0, 0, 1, &pick) && pick.distance == 7 &&
           pick.normal[2] == -1);
    assert(hit(0, 0, 0, 0, 1, 0, &pick) && pick.distance == 2 &&
           pick.normal[1] == 1);
    assert(hit(0, 0, 10, 0, 0, -1, &pick) && pick.normal[2] == 1);
    assert(hit(1, 2, 10, 0, 0, -1, &pick) &&
           pick.ticket == box_ticket);
    assert(hit(0, 0, 10, 0, 0, -1, &pick));
    assert(!obs_sculpt_ray_cast((double[]){0, 0, 10},
                                (double[]){0, 0, -1}, 100, &pick));
    assert(!hit(1.01, 0, 10, 0, 0, -1, &pick) &&
           pick.ticket == box_ticket);
    assert(!hit(0, 2.01, 10, 0, 0, -1, &pick));
    assert(hit(0, 0, 10, 0, 0, -1, &pick));
    assert(!obs_sculpt_ray_cast_primitive((double[]){0, 0, 10},
                                          (double[]){0, 0, -1}, 6.99,
                                          &pick));
    assert(!obs_sculpt_ray_cast_primitive((double[]){0, 0, 10},
                                          (double[]){NAN, 0, -1}, 100,
                                          &pick));
    assert(!obs_sculpt_ray_cast_primitive((double[]){0, 0, 10},
                                          (double[]){0, 0, 0}, 100, &pick));
    /* A nearer sphere wins when both primitives lie on the same ray. */
    assert(obs_sculpt_move(sphere_ticket, (double[]){0, 0, 6}));
    assert(hit(0, 0, 10, 0, 0, -1, &pick) &&
           pick.ticket == sphere_ticket &&
           fabs(pick.distance - 3.5) < 1e-12);
    assert(obs_sculpt_move(sphere_ticket, (double[]){10, 0, 0}));

    /* Unsupported affine transforms and malformed geometry fail closed. */
    scene->sdfs[box].inverseTransform[0][0] = 2;
    assert(!hit(0, 0, 10, 0, 0, -1, &pick));
    scene->sdfs[box].inverseTransform[0][0] = 1;
    scene->sdfs[box].size[0] = 0;
    assert(!hit(0, 0, 10, 0, 0, -1, &pick));
    scene->sdfs[box].size[0] = 1;

    /* Typed box-extent property edits update the CPU pick volume, and undo /
     * redo immediately update it again. */
    ObsSdfEditValues extent = {.box_extents = {1.75, 2, 3}};
    assert(!hit(1.5, 0, 10, 0, 0, -1, &pick));
    assert(obs_sdf_property_apply(box, OBS_SDF_EDIT_BOX, &extent) ==
           OBS_SDF_EDIT_OK);
    assert(hit(1.5, 0, 10, 0, 0, -1, &pick) &&
           pick.ticket == box_ticket);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(!hit(1.5, 0, 10, 0, 0, -1, &pick));
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(hit(1.5, 0, 10, 0, 0, -1, &pick));
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(obs_sdf_inspector_clear() == OBS_SDF_EDIT_OK);

    assert(obs_sdf_viewport_set_enabled(true));
    assert(press(0));
    assert(obs_sdf_viewport_drag_ticket() == box_ticket);
    assert(obs_sdf_inspector_state().selection == box_ticket);
    for (unsigned i = 0; i < 10000; ++i)
        move_pointer((double)i / 1000);
    assert(obs_sdf_viewport_release());
    assert(!obs_sdf_viewport_dragging());
    assert(obs_sdf_inspector_state().undo_count == 1);
    assert(obs_sculpt_position_any(box_ticket, where) && where[0] > 9.9);
    assert(obs_sdf_sync_frame(0));
    assert(gpu[0][box].inverseTransform[3][0] < -9.9f);
    assert(gpu[1][box].inverseTransform[3][0] == 0);
    assert(obs_sdf_inspector_undo() == OBS_SDF_EDIT_OK);
    assert(obs_sculpt_position_any(box_ticket, where) && where[0] == 0);
    assert(obs_sdf_sync_frame(1));
    assert(gpu[1][box].inverseTransform[3][0] == 0);
    assert(obs_sdf_inspector_redo() == OBS_SDF_EDIT_OK);
    assert(obs_sculpt_position_any(box_ticket, where) && where[0] > 9.9);

    assert(press(10));
    move_pointer(12);
    assert(obs_sdf_viewport_cancel() && !obs_sdf_viewport_dragging());
    assert(obs_sculpt_position_any(box_ticket, where) &&
           where[0] > 9.9 && where[0] < 10.1);
    assert(obs_sdf_inspector_state().undo_count == 1);

    assert(press(10));
    move_pointer(11);
    const float expected_preview = scene->sdfs[box].inverseTransform[3][0];
    scene->sdfs[box].inverseTransform[3][0] = -77;
    assert(obs_sdf_viewport_cancel() && obs_sdf_viewport_dragging());
    assert(obs_sdf_viewport_error() == OBS_SDF_EDIT_CONFLICT);
    scene->sdfs[box].inverseTransform[3][0] = expected_preview;
    assert(obs_sdf_viewport_cancel() && !obs_sdf_viewport_dragging());
    assert(obs_sculpt_position_any(box_ticket, where) && where[0] < 10.1);

    free(scene->sdfs);
    scene->sdfs = NULL;
    scene->sdf_count = scene->sdf_capacity = 0;
    ++scene->sdf_lifetime_epoch;
    assert(!obs_sculpt_position_any(box_ticket, where));
    assert(!hit(0, 0, 10, 0, 0, -1, &pick));
    puts("Cycle 60: typed box picking, drag, journal, GPU slot and lifetime PASS");
    return 0;
}
