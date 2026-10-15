#include "scene.h"

typedef struct {
    int32_t node;
    bool parent_visible;
} VisibilityWalk;

bool scene_tree_set_selected_visibility(SceneTree* tree, bool visible) {
    if (!tree) return false;
    bool found = false;
    for (int32_t i = 1; i < tree->count; i++) {
        SceneNode* node = &tree->nodes[i];
        if (!node->selected) continue;
        found = true;
    }
    if (!found) return false;

    for (int32_t i = 1; i < tree->count; i++) {
        SceneNode* node = &tree->nodes[i];
        if (node->selected) node->visible = visible;
    }
    return true;
}

bool scene_tree_collect_sdf_visibility(const SceneTree* tree,
                                       uint32_t sdf_count,
                                       bool visible[MAX_SDF_PRIMITIVES]) {
    if (!tree || !visible || tree->count > SCENE_TREE_MAX_NODES ||
        sdf_count > MAX_SDF_PRIMITIVES) return false;

    for (uint32_t i = 0; i < sdf_count; ++i) visible[i] = true;
    if (tree->count == 0) return sdf_count == 0;

    VisibilityWalk stack[SCENE_TREE_MAX_NODES];
    bool visited[SCENE_TREE_MAX_NODES] = {false};
    uint32_t top = 0;
    stack[top++] = (VisibilityWalk){0, true};
    while (top) {
        const VisibilityWalk entry = stack[--top];
        if (entry.node < 0 || entry.node >= tree->count ||
            visited[entry.node]) return false;
        visited[entry.node] = true;

        const SceneNode* node = &tree->nodes[entry.node];
        const bool effective = entry.parent_visible && node->visible;
        if (node->sdf_index >= 0 && (uint32_t)node->sdf_index < sdf_count)
            visible[node->sdf_index] = effective;

        int32_t child = node->first_child;
        while (child >= 0) {
            if (child >= tree->count || top >= SCENE_TREE_MAX_NODES)
                return false;
            stack[top++] = (VisibilityWalk){child, effective};
            child = tree->nodes[child].next_sibling;
        }
    }
    return true;
}
