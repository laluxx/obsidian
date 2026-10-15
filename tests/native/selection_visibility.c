#include <cglm/cglm.h>
#include "scene.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    SceneTree tree = {0};
    tree.count = 5;
    tree.nodes[0].visible = true;
    tree.nodes[0].sdf_index = -1;
    tree.nodes[0].first_child = 1;
    tree.nodes[1] = (SceneNode){.parent = 0, .first_child = 3,
                                .sdf_index = -1,
                                .next_sibling = 2, .visible = true};
    tree.nodes[2] = (SceneNode){.parent = 0, .first_child = 4,
                                .sdf_index = -1,
                                .next_sibling = -1, .visible = false};
    tree.nodes[3] = (SceneNode){.parent = 1, .first_child = -1,
                                .sdf_index = -1,
                                .next_sibling = -1, .visible = true};
    tree.nodes[4] = (SceneNode){.parent = 2, .first_child = -1,
                                .sdf_index = -1,
                                .next_sibling = -1, .visible = true};

    assert(!scene_tree_set_selected_visibility(&tree, false));
    assert(tree.nodes[1].visible && !tree.nodes[2].visible);

    tree.nodes[1].selected = true;
    tree.nodes[2].selected = true;
    tree.nodes[3].selected = true;
    tree.nodes[4].selected = true;
    assert(scene_tree_set_selected_visibility(&tree, false));
    assert(!tree.nodes[1].visible && !tree.nodes[2].visible);
    assert(!tree.nodes[3].visible && !tree.nodes[4].visible);

    assert(scene_tree_set_selected_visibility(&tree, true));
    assert(tree.nodes[1].visible && tree.nodes[2].visible);
    assert(tree.nodes[3].visible && tree.nodes[4].visible);

    tree.nodes[1].selected = false;
    tree.nodes[2].selected = false;
    tree.nodes[3].selected = false;
    tree.nodes[4].selected = false;
    tree.nodes[1].visible = false;
    tree.nodes[3].visible = true;
    tree.nodes[3].selected = true;
    assert(scene_tree_set_selected_visibility(&tree, false));
    assert(!tree.nodes[1].visible && !tree.nodes[3].visible);
    assert(tree.nodes[2].visible && tree.nodes[4].visible);

    tree.nodes[1].visible = true;
    tree.nodes[2].visible = false;
    tree.nodes[3].visible = true;
    tree.nodes[2].sdf_index = 0;
    tree.nodes[3].sdf_index = 1;
    tree.nodes[4].sdf_index = 2;
    bool sdf_visible[MAX_SDF_PRIMITIVES] = {0};
    assert(scene_tree_collect_sdf_visibility(&tree, 3, sdf_visible));
    assert(!sdf_visible[0] && sdf_visible[1] && !sdf_visible[2]);
    tree.nodes[1].visible = false;
    assert(scene_tree_collect_sdf_visibility(&tree, 3, sdf_visible));
    assert(!sdf_visible[0] && !sdf_visible[1] && !sdf_visible[2]);
    tree.nodes[1].visible = true;
    tree.nodes[2].visible = true;
    assert(scene_tree_collect_sdf_visibility(&tree, 3, sdf_visible));
    assert(sdf_visible[0] && sdf_visible[1] && sdf_visible[2]);

    puts("selection visibility: explicit selected-node targets and inherited SDF visibility pass");
    return 0;
}
