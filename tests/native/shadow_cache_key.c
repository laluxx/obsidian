#include "shadow_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t key(Scene *scene, LightingData *lighting) {
  uint64_t result = 0;
  assert(obs_shadow_cache_build_key(scene, lighting, true, 1, &result));
  return result;
}

int main(void) {
  Scene scene = {0};
  Mesh mesh = {0};
  LightingData lighting = {0};
  assert(!obs_lighting_has_directional_sun(&lighting));
  assert(!obs_lighting_needs_shadow_map(&lighting, true));
  lighting.sun.color[3] = 3.0f;
  assert(obs_lighting_has_directional_sun(&lighting));
  assert(obs_lighting_needs_shadow_map(&lighting, true));
  assert(!obs_lighting_needs_shadow_map(&lighting, false));
  lighting.sun.color[3] = 0.0f;

  scene.meshes.items = &mesh;
  scene.meshes.count = 1;
  mesh.visible = true;
  mesh.meshletCount = 1;
  mesh.megaBaseVertex = 0;
  mesh.megaBaseMeshlet = 0;
  mesh.jointOffset = -1;
  mesh.alpha_mode = 0;
  mesh.textureIndex = -1;
  mesh.materialIndex = -1;
  mesh.alpha_cutoff = 0.5f;
  glm_mat4_identity(mesh.model);
  glm_vec3_zero(mesh.aabbMin);
  glm_vec3_one(mesh.aabbMax);

  const uint64_t baseline = key(&scene, &lighting);
  mesh.model[3][0] = 1.0f;
  assert(key(&scene, &lighting) != baseline);
  mesh.model[3][0] = 0.0f;
  lighting.cascadeSpace[0][3][1] = 2.0f;
  assert(key(&scene, &lighting) != baseline);
  lighting.cascadeSpace[0][3][1] = 0.0f;

  mesh.alpha_mode = 1;
  const uint64_t alpha_mask = key(&scene, &lighting);
  mesh.alpha_cutoff = 0.25f;
  assert(key(&scene, &lighting) != alpha_mask);
  mesh.alpha_cutoff = 0.5f;
  mesh.textureIndex = 7;
  assert(key(&scene, &lighting) != alpha_mask);
  mesh.textureIndex = -1;
  Texture2D texture = {0};
  mesh.texture = &texture;
  const uint64_t old_texture = key(&scene, &lighting);
  texture.image = (VkImage)(uintptr_t)1;
  assert(key(&scene, &lighting) != old_texture);
  mesh.texture = NULL;

  const uint64_t old_bounds = key(&scene, &lighting);
  mesh.aabbMax[0] = 2.0f;
  assert(key(&scene, &lighting) != old_bounds);
  mesh.aabbMax[0] = 1.0f;
  const uint64_t old_revision = key(&scene, &lighting);
  ++scene.mesh_revision;
  assert(key(&scene, &lighting) != old_revision);

  mesh.jointOffset = 0;
  uint64_t ignored = 0;
  assert(!obs_shadow_cache_build_key(&scene, &lighting, true, 1, &ignored));
  mesh.jointOffset = -1;
  mesh.morphCount = 1;
  assert(!obs_shadow_cache_build_key(&scene, &lighting, true, 1, &ignored));
  mesh.morphCount = 0;
  assert(!obs_shadow_cache_build_key(&scene, &lighting, false, 1, &ignored));
  assert(!obs_shadow_cache_build_key(&scene, &lighting, true, 0, &ignored));
  assert(!obs_shadow_cache_build_key(NULL, &lighting, true, 1, &ignored));

  puts("shadow cache key: transform, cascades, alpha material, and dynamic bypass PASS");
  return 0;
}
