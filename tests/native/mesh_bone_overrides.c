#include "renderer.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(sizeof(Mesh) < 8192);
  printf("sizeof(Mesh)=%zu bytes\n", sizeof(Mesh));

  Mesh mesh = {0};
  assert(mesh_bone_override_get(&mesh, 0, false) == NULL);
  assert(mesh.bone_overrides == NULL);
  assert(mesh_bone_override_get(&mesh, 0, true) == NULL);
  assert(mesh.bone_overrides == NULL);

  mesh.jointCount = 4;
  assert(mesh_bone_override_get(&mesh, 3, false) == NULL);
  MeshBoneOverride *override = mesh_bone_override_get(&mesh, 3, true);
  assert(override != NULL);
  assert(mesh.bone_override_capacity == 4);
  assert(!override->active);

  glm_mat4_identity(override->world_offset);
  override->active = true;
  assert(mesh_bone_override_get(&mesh, 3, false)->active);
  assert(mesh_bone_override_get(&mesh, 4, true) == NULL);
  assert(mesh.bone_override_capacity == 4);

  mesh_bone_overrides_clear(&mesh);
  assert(mesh.bone_overrides == NULL);
  assert(mesh.bone_override_capacity == 0);
  mesh_bone_overrides_clear(&mesh);

  mesh.jointCount = (int)MAX_MESH_BONE_OVERRIDES + 1;
  assert(mesh_bone_override_get(&mesh, MAX_MESH_BONE_OVERRIDES, true) == NULL);
  assert(mesh.bone_overrides == NULL);
  override = mesh_bone_override_get(&mesh, MAX_MESH_BONE_OVERRIDES - 1, true);
  assert(override != NULL);
  assert(mesh.bone_override_capacity == MAX_MESH_BONE_OVERRIDES);
  mesh_bone_overrides_clear(&mesh);

  puts("ok: mesh bone overrides allocate only on demand and stay bounded");
  return 0;
}
