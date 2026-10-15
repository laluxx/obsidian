#include "shadow_cache.h"

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = data;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

#define HASH_FIELD(hash, value) hash_bytes((hash), &(value), sizeof(value))

bool obs_shadow_cache_build_key(const Scene *scene,
                                const LightingData *lighting,
                                bool shadows_enabled,
                                uint32_t shadow_draw_count,
                                uint64_t *out_key) {
  if (!scene || !lighting || !out_key || !shadows_enabled ||
      shadow_draw_count == 0)
    return false;

  uint64_t hash = UINT64_C(14695981039346656037);
  const uint64_t mesh_count = (uint64_t)scene->meshes.count;
  hash = HASH_FIELD(hash, scene->mesh_revision);
  hash = HASH_FIELD(hash, scene->gltf_revision);
  hash = HASH_FIELD(hash, mesh_count);
  hash = hash_bytes(hash, lighting->cascadeSpace,
                    sizeof(lighting->cascadeSpace));

  for (size_t i = 0; i < scene->meshes.count; ++i) {
    const Mesh *mesh = &scene->meshes.items[i];
    if (!mesh->visible || mesh->meshletCount == 0 || mesh->wireframe ||
        mesh->megaBaseMeshlet == UINT32_MAX ||
        mesh->megaBaseVertex == UINT32_MAX)
      continue;
    if (mesh->jointOffset >= 0 || mesh->morphCount > 0)
      return false;

    const uint64_t mesh_index = (uint64_t)i;
    hash = HASH_FIELD(hash, mesh_index);
    hash = hash_bytes(hash, mesh->model, sizeof(mesh->model));
    hash = hash_bytes(hash, mesh->aabbMin, sizeof(mesh->aabbMin));
    hash = hash_bytes(hash, mesh->aabbMax, sizeof(mesh->aabbMax));
    hash = HASH_FIELD(hash, mesh->megaBaseVertex);
    hash = HASH_FIELD(hash, mesh->megaBaseMeshlet);
    hash = HASH_FIELD(hash, mesh->megaBaseMeshletVertex);
    hash = HASH_FIELD(hash, mesh->megaBaseMeshletTriangle);
    hash = HASH_FIELD(hash, mesh->meshletCount);
    hash = HASH_FIELD(hash, mesh->vertexCount);
    hash = HASH_FIELD(hash, mesh->indexCount);
    hash = HASH_FIELD(hash, mesh->textureIndex);
    hash = HASH_FIELD(hash, mesh->materialIndex);
    hash = HASH_FIELD(hash, mesh->alpha_mode);
    hash = HASH_FIELD(hash, mesh->alpha_cutoff);
    hash = hash_bytes(hash, mesh->baseColorFactor,
                      sizeof(mesh->baseColorFactor));
    const VkImage texture_image =
        mesh->texture ? mesh->texture->image : VK_NULL_HANDLE;
    const VkImageView texture_view =
        mesh->texture ? mesh->texture->view : VK_NULL_HANDLE;
    hash = HASH_FIELD(hash, texture_image);
    hash = HASH_FIELD(hash, texture_view);
  }
  *out_key = hash;
  return true;
}

#undef HASH_FIELD
