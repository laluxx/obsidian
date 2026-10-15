#include "renderer.h"

#include <stdlib.h>

MeshBoneOverride *mesh_bone_override_get(Mesh *mesh, uint32_t local_joint,
                                         bool create) {
  if (!mesh || mesh->jointCount <= 0 ||
      local_joint >= (uint32_t)mesh->jointCount ||
      local_joint >= MAX_MESH_BONE_OVERRIDES)
    return NULL;

  if (!mesh->bone_overrides && create) {
    uint32_t capacity = (uint32_t)mesh->jointCount;
    if (capacity > MAX_MESH_BONE_OVERRIDES)
      capacity = MAX_MESH_BONE_OVERRIDES;
    MeshBoneOverride *overrides = calloc(capacity, sizeof(*overrides));
    if (!overrides)
      return NULL;
    mesh->bone_overrides = overrides;
    mesh->bone_override_capacity = capacity;
  }

  if (!mesh->bone_overrides || local_joint >= mesh->bone_override_capacity)
    return NULL;
  return &mesh->bone_overrides[local_joint];
}

void mesh_bone_overrides_clear(Mesh *mesh) {
  if (!mesh)
    return;
  free(mesh->bone_overrides);
  mesh->bone_overrides = NULL;
  mesh->bone_override_capacity = 0;
}
