#pragma once

#include <stdio.h>

#include "scene.h"

static inline void editor_hierarchy_add_range(SceneTree *tree,
                                               const int32_t *selection_nodes,
                                               int item_count, int first_row,
                                               int last_row) {
  if (!tree || !selection_nodes || item_count <= 0 || first_row < 0 ||
      first_row > last_row || last_row >= item_count)
    return;
  for (int i = first_row; i <= last_row; ++i) {
    const int32_t node_index = selection_nodes[i];
    if (node_index < 0 || node_index >= tree->count)
      continue;
    SceneNode *node = &tree->nodes[node_index];
    node->selected = true;
    node->selected_group =
        node->mesh_index < 0 && node->sdf_index < 0;
  }
}

static inline void editor_hierarchy_promote_complete_gltfs(Scene *owner) {
  if (!owner)
    return;
  for (size_t g = 0; g < owner->gltf_instance_count; ++g) {
    GLTFInstance *instance = &owner->gltf_instances[g];
    const int32_t first = (int32_t)instance->mesh_start_index;
    const int32_t end = first + (int32_t)instance->mesh_count;
    int32_t selected = 0, group_idx = -1;
    for (int32_t i = 0; i < owner->tree.count; ++i) {
      SceneNode *node = &owner->tree.nodes[i];
      if (node->mesh_index >= first && node->mesh_index < end &&
          node->selected)
        ++selected;
      if (!node->selected || !node->selected_group)
        continue;
      for (int32_t child = node->first_child; child >= 0;
           child = owner->tree.nodes[child].next_sibling) {
        const int32_t mesh = owner->tree.nodes[child].mesh_index;
        if (mesh >= first && mesh < end) {
          group_idx = i;
          break;
        }
      }
    }
    ObsMonadGltfSelectionTransition transition =
        OBS_MONAD_GLTF_SELECTION_NONE;
    if (!obs_monad_live_editor_hierarchy_gltf_selection_transition(
            selected, (int)instance->mesh_count, group_idx >= 0,
            &transition)) {
      fprintf(stderr, "Monad glTF hierarchy selection policy failed\n");
      return;
    }
    if (transition == OBS_MONAD_GLTF_SELECTION_DEMOTE) {
      for (int32_t i = 0; i < owner->tree.count; ++i) {
        SceneNode *node = &owner->tree.nodes[i];
        if (node->mesh_index >= first && node->mesh_index < end)
          node->selected = false;
      }
      continue;
    }
    if (transition != OBS_MONAD_GLTF_SELECTION_PROMOTE)
      continue;
    for (int32_t i = 0; i < owner->tree.count; ++i) {
      SceneNode *node = &owner->tree.nodes[i];
      if (node->mesh_index >= first && node->mesh_index < end)
        node->selected = false;
    }
    for (int32_t i = 0; i < owner->tree.count; ++i) {
      SceneNode *group = &owner->tree.nodes[i];
      for (int32_t child = group->first_child; child >= 0;
           child = owner->tree.nodes[child].next_sibling) {
        const int32_t mesh = owner->tree.nodes[child].mesh_index;
        if (mesh >= first && mesh < end) {
          group->selected = true;
          group->selected_group = true;
          break;
        }
      }
    }
  }
}
