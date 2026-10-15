#pragma once

#include "scene.h"
#include <math.h>

static inline bool editor_gltf_group_is_selected(const GLTFInstance *instance) {
  if (!scene || !instance)
    return false;
  const int32_t end =
      (int32_t)(instance->mesh_start_index + instance->mesh_count);
  for (int32_t i = 0; i < scene->tree.count; ++i) {
    const SceneNode *group = &scene->tree.nodes[i];
    if (!group->selected || !group->selected_group)
      continue;
    for (int32_t child = group->first_child; child >= 0;
         child = scene->tree.nodes[child].next_sibling) {
      const int32_t mesh = scene->tree.nodes[child].mesh_index;
      if (mesh >= (int32_t)instance->mesh_start_index && mesh < end)
        return true;
    }
  }
  return false;
}

static inline int32_t editor_gltf_mesh_node(const GLTFInstance *instance,
                                             int mesh_index) {
  if (!instance || !instance->gltf_data)
    return -1;
  const OmdlSceneGraph *graph = instance->gltf_data;
  if (mesh_index < (int)instance->mesh_start_index ||
      mesh_index >= (int)(instance->mesh_start_index + instance->mesh_count))
    return -1;
  const Mesh *mesh = &scene->meshes.items[mesh_index];
  if (mesh->node) {
    const uintptr_t idx = (uintptr_t)mesh->node;
    return idx < graph->node_count ? (int32_t)idx : -1;
  }
  const int32_t relative = mesh_index - (int)instance->mesh_start_index;
  for (uint32_t i = 0; i < graph->node_count; ++i)
    if (graph->nodes[i].mesh_idx == relative)
      return (int32_t)i;
  return -1;
}

static inline bool editor_gltf_parent_world(GLTFInstance *instance,
                                             int32_t node_index,
                                             mat4 out_parent) {
  if (!instance || !instance->gltf_data || node_index < 0)
    return false;
  const OmdlSceneGraph *graph = instance->gltf_data;
  const int32_t parent = graph->nodes[node_index].parent;
  if (parent >= 0)
    glm_mat4_copy(graph->world_transforms[parent], out_parent);
  else
    glm_mat4_copy(instance->instance_transform, out_parent);
  return true;
}

static inline bool editor_gltf_store_node_local(GLTFInstance *instance,
                                                 int32_t node_index,
                                                 Mesh *mesh,
                                                 mat4 local) {
  if (!instance || !instance->gltf_data || !mesh || node_index < 0)
    return false;
  OmdlSceneGraph *graph = instance->gltf_data;
  OmdlNode *node = &graph->nodes[node_index];
  glm_mat4_copy(local, node->base_matrix);
  glm_mat4_copy(local, graph->local_transforms[node_index]);
  for (size_t i = 0; i < instance->mesh_count; ++i) {
    const int mesh_index = (int)(instance->mesh_start_index + i);
    if (editor_gltf_mesh_node(instance, mesh_index) == node_index)
      glm_mat4_copy(local, scene->meshes.items[mesh_index].local_transform);
  }
  glm_mat4_copy(local, mesh->local_transform);
  glm_vec3_copy(local[3], node->translation);

  mat3 rotation;
  glm_mat4_pick3(local, rotation);
  for (int axis = 0; axis < 3; ++axis) {
    const float scale = glm_vec3_norm(rotation[axis]);
    node->scale[axis] = scale;
    if (scale > 1e-8f)
      glm_vec3_scale(rotation[axis], 1.0f / scale, rotation[axis]);
  }
  glm_mat3_quat(rotation, node->rotation);
  return true;
}

static inline void editor_gltf_store_node_world(GLTFInstance *instance,
                                                 int32_t node_index,
                                                 mat4 world) {
  if (!instance || !instance->gltf_data || node_index < 0)
    return;
  for (size_t i = 0; i < instance->mesh_count; ++i) {
    const int mesh_index = (int)(instance->mesh_start_index + i);
    if (editor_gltf_mesh_node(instance, mesh_index) == node_index)
      glm_mat4_copy(world, scene->meshes.items[mesh_index].model);
  }
}
