#define _POSIX_C_SOURCE 200809L
#define CGLTF_IMPLEMENTATION
#include "gltf_material.h"
#include "static_cook_contract.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

void obs_gltf_cache_destroy(ObsGltfCacheCandidate *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->mesh_count; ++i)
    free(c->meshes[i].name);
  free(c->meshes);
  if (c->graph) {
    free(c->graph->nodes);
    free(c->graph->world_transforms);
    free(c->graph->local_transforms);
    free(c->graph->script_translations);
    free(c->graph->topology_order);
    free(c->graph);
  }
  for (size_t i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
    free(c->streams[i]);
  free(c->metadata);
  free(c->source_path);
  obs_cache_textures_dispose(&c->textures);
  free(c);
}

static void sample(const char *path, bool expected) {
  ObsGltfCacheCandidate *c = (void *)1;
  bool actual = obs_gltf_cook_static_cpu(path, &c);
  if (actual != expected)
    fprintf(stderr, "fixture %s: actual=%d expected=%d\n", path, actual,
            expected);
  assert(actual == expected);
  if (!expected) {
    assert(c == NULL);
    return;
  }
  assert(c && c->mesh_count == 1);
  assert(c->graph && c->graph->node_count == 1 &&
         c->graph->nodes[0].mesh_idx == 0);
  assert(c->counts[OBS_GEOM_VERTEX] == 3);
  assert(c->counts[OBS_GEOM_INDEX] == 3);
  assert(c->counts[OBS_GEOM_MESHLET] == 1);
  assert(c->counts[OBS_GEOM_BOUNDS] == 1);
  assert(c->counts[OBS_GEOM_SKIN] == 1);
  assert(c->counts[OBS_GEOM_MESHLET_VERTEX] == 3);
  assert(c->counts[OBS_GEOM_MESHLET_TRIANGLE] == 4);
  assert(c->counts[OBS_GEOM_MORPH] == 0);
  Vertex *v = c->streams[OBS_GEOM_VERTEX];
  assert(v[1].pos[0] == 1.0f && v[2].pos[1] == 1.0f);
  Meshlet *ml = c->streams[OBS_GEOM_MESHLET];
  assert(ml[0].vertex_count == 3 && ml[0].triangle_count == 1);
  uint8_t *tri = c->streams[OBS_GEOM_MESHLET_TRIANGLE];
  assert(tri[0] == 0 && tri[1] == 1 && tri[2] == 2 && tri[3] == 0);
  assert(c->spans[OBS_GEOM_VERTEX].bytes == 3 * sizeof(Vertex));
  assert(c->spans[OBS_GEOM_MESHLET_TRIANGLE].bytes == 4);
  assert(c->meshes[0].megaBaseVertex == UINT32_MAX);
  assert(c->meshes[0].textureIndex == -1);
  assert(c->meshes[0].jointOffset == -1);
  assert(c->meshes[0].visible);
  assert(c->meshes[0].aoStrength == 1.0f);
  assert(c->metadata[0].aoStrength == 1.0f);
  assert(c->textures.mesh_count == 1);
  assert(c->textures.unique_count == 0);
  obs_gltf_cache_destroy(c);
}

static void chess_scene(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(obs_gltf_cook_static_cpu(
      "assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &c));
  assert(c && c->mesh_count == 49);
  assert(c->counts[OBS_GEOM_VERTEX] == 934309);
  assert(c->counts[OBS_GEOM_INDEX] == 4500672);
  assert(c->textures.mesh_count == c->mesh_count);
  assert(c->textures.unique_count == 33);
  assert(strstr(c->metadata[0].tex_albedo,
                "assets/gltf/ABeautifulGame/glTF/") != NULL);
  obs_gltf_cache_destroy(c);
}

static void textured_static_scene(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(obs_gltf_cook_static_cpu("tests/fixtures/async-static-textured.gltf",
                                  &c));
  assert(c && c->mesh_count > 0);
  assert(c->textures.mesh_count == c->mesh_count);
  assert(c->textures.unique_count == 1);
  assert(strcmp(c->textures.paths[0], "tests/fixtures/test.png") == 0);
  obs_gltf_cache_destroy(c);
}

static void multi_meshlet(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(obs_gltf_cook_static_cpu("tests/fixtures/async-static-meshlets.gltf",
                                  &c));
  assert(c && c->counts[OBS_GEOM_VERTEX] == 66);
  assert(c->counts[OBS_GEOM_INDEX] == 66);
  assert(c->counts[OBS_GEOM_MESHLET] == 2);
  assert(c->counts[OBS_GEOM_MESHLET_VERTEX] == 66);
  assert(c->counts[OBS_GEOM_MESHLET_TRIANGLE] == 68);
  assert(c->meshes[0].model[3][0] == 3.0f);
  assert(c->meshes[0].model[3][1] == 4.0f);
  assert(c->meshes[0].model[3][2] == 5.0f);
  assert(c->meshes[0].metallicFactor == 0.8f);
  assert(c->meshes[0].baseColorFactor[0] == 0.2f);
  Meshlet *ml = c->streams[OBS_GEOM_MESHLET];
  uint32_t *mv = c->streams[OBS_GEOM_MESHLET_VERTEX];
  uint8_t *mt = c->streams[OBS_GEOM_MESHLET_TRIANGLE];
  uint32_t *idx = c->streams[OBS_GEOM_INDEX];
  for (uint32_t t = 0, global_triangle = 0; t < c->counts[OBS_GEOM_MESHLET];
       ++t) {
    assert(ml[t].vertex_count <= 64 && ml[t].triangle_count <= 124);
    assert((ml[t].triangle_offset & 3u) == 0);
    for (uint32_t k = 0; k < ml[t].triangle_count * 3; ++k) {
      uint32_t local = mt[ml[t].triangle_offset + k];
      assert(local < ml[t].vertex_count);
      assert(mv[ml[t].vertex_offset + local] == idx[global_triangle * 3 + k]);
    }
    global_triangle += ml[t].triangle_count;
    if (t + 1 == c->counts[OBS_GEOM_MESHLET])
      assert(global_triangle == 22);
  }
  obs_gltf_cache_destroy(c);
}

static void multi_root(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(obs_gltf_cook_static_cpu(
      "tests/fixtures/async-static-unsupported.gltf", &c));
  assert(c->mesh_count == 1 && c->graph->node_count == 2);
  assert(c->graph->nodes[0].mesh_idx == 0);
  assert(c->graph->nodes[1].mesh_idx == -1);
  obs_gltf_cache_destroy(c);
}

static bool almost(float a, float b) { return fabsf(a - b) < 0.0001f; }

static void ao_strength_import(void) {
  cgltf_options options = {0};
  cgltf_data *data = NULL;
  assert(cgltf_parse_file(&options, "tests/fixtures/ao-strength.gltf", &data) ==
         cgltf_result_success);
  assert(data && data->materials_count == 2);
  assert(data->materials[0].occlusion_texture.texture);
  assert(obs_gltf_occlusion_strength(&data->materials[0]) == 0.25f);
  assert(obs_gltf_occlusion_strength(&data->materials[1]) == 1.0f);
  data->materials[0].occlusion_texture.scale = 0.0f;
  assert(obs_gltf_occlusion_strength(&data->materials[0]) == 0.0f);
  cgltf_free(data);
}

static void hierarchy(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(obs_gltf_cook_static_cpu("tests/fixtures/async-static-hierarchy.gltf",
                                  &c));
  assert(c->mesh_count == 6);
  assert(c->graph->node_count == 8);
  assert(c->textures.mesh_count == 6 && c->textures.unique_count == 0);
  assert(c->graph->nodes[0].mesh_idx == -1);
  assert(c->graph->nodes[1].mesh_idx == 0);
  assert(c->graph->nodes[2].mesh_idx == 2);
  assert(c->graph->nodes[3].mesh_idx == -1);
  assert(c->graph->nodes[4].mesh_idx == 3);
  assert(c->graph->nodes[5].mesh_idx == 5);
  assert(c->graph->nodes[6].mesh_idx == 1 && c->graph->nodes[6].parent == 1);
  assert(c->graph->nodes[7].mesh_idx == 4 && c->graph->nodes[7].parent == 4);
  uint32_t position[8] = {0};
  for (uint32_t i = 0; i < 8; ++i)
    position[c->graph->topology_order[i]] = i;
  for (uint32_t n = 0; n < 8; ++n)
    if (c->graph->nodes[n].parent >= 0)
      assert(position[c->graph->nodes[n].parent] < position[n]);
  /* Parent rotation turns child translation (+1,0,0) into (+0,+1,0). */
  assert(almost(c->meshes[0].model[3][0], 2));
  assert(almost(c->meshes[0].model[3][1], 1));
  assert(almost(c->meshes[1].model[3][0], 2));
  assert(almost(c->meshes[1].model[3][1], 1));
  assert(almost(c->meshes[3].model[3][2], 5));
  assert(almost(c->meshes[5].model[3][0], -5));
  assert(c->meshes[0].node == (void *)(uintptr_t)1);
  assert(c->meshes[1].node == (void *)(uintptr_t)6);
  assert(c->meshes[4].node == (void *)(uintptr_t)7);
  assert(c->counts[OBS_GEOM_VERTEX] == 18);
  assert(c->counts[OBS_GEOM_INDEX] == 18);
  assert(c->counts[OBS_GEOM_MESHLET] == 6);
  assert(c->counts[OBS_GEOM_MESHLET_VERTEX] == 18);
  assert(c->counts[OBS_GEOM_MESHLET_TRIANGLE] == 24);
  const Vertex *vertices = c->streams[OBS_GEOM_VERTEX];
  const uint32_t *indices = c->streams[OBS_GEOM_INDEX];
  const Meshlet *ml = c->streams[OBS_GEOM_MESHLET];
  const uint32_t *mv = c->streams[OBS_GEOM_MESHLET_VERTEX];
  const uint8_t *mt = c->streams[OBS_GEOM_MESHLET_TRIANGLE];
  for (uint32_t i = 0; i < 6; ++i) {
    const OmdlMeshMeta *m = &c->metadata[i];
    assert(m->vertexOffset == i * 3 && m->indexOffset == i * 3);
    assert(m->meshletCount == 1 && m->meshletOffset == i);
    assert(m->meshletVertexOffset == i * 3);
    assert(m->meshletTriangleOffset == i * 4);
    assert(ml[m->meshletOffset].vertex_offset == 0);
    assert(ml[m->meshletOffset].triangle_offset == 0);
    assert(c->meshes[i].meshletCount == 1);
    assert(c->meshes[i].megaBaseVertex == UINT32_MAX);
    for (uint32_t k = 0; k < 3; ++k) {
      uint32_t local = mt[m->meshletTriangleOffset + k];
      assert(local < 3);
      assert(mv[m->meshletVertexOffset + local] == indices[m->indexOffset + k]);
    }
    assert(mt[m->meshletTriangleOffset + 3] == 0);
    assert(almost(vertices[m->vertexOffset].color[0], 1.0f));
  }
  assert(indices[0] == 0 && indices[1] == 2 && indices[2] == 1);
  assert(almost(c->meshes[0].baseColorFactor[0], 0.2));
  assert(almost(c->meshes[1].baseColorFactor[0], 0.7));
  assert(almost(c->meshes[2].metallicFactor, 1.0)); /* glTF default */
  for (uint32_t i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    if (c->counts[i])
      assert(c->spans[i].data == c->streams[i]);
  }
  obs_gltf_cache_destroy(c);
}

static void meshlet_growth_fixture(void) {
  ObsGltfCacheCandidate *c = NULL;
  assert(
      obs_gltf_cook_static_cpu("tests/fixtures/async-meshlet-growth.gltf", &c));
  assert(c && c->mesh_count == 1);
  assert(c->counts[OBS_GEOM_VERTEX] == 9000);
  assert(c->counts[OBS_GEOM_INDEX] == 9000);
  assert(c->counts[OBS_GEOM_MESHLET] > 128);
  assert(c->spans[OBS_GEOM_MESHLET_VERTEX].bytes > 4096);
  assert(c->spans[OBS_GEOM_MESHLET_TRIANGLE].bytes > 4096);
  obs_gltf_cache_destroy(c);
}

static void *parallel(void *ignored) {
  (void)ignored;
  for (int i = 0; i < 20; ++i) {
    sample("tests/fixtures/async-triangle.gltf", true);
    hierarchy();
  }
  return NULL;
}

int main(void) {
  ao_strength_import();
  sample("tests/fixtures/async-triangle.gltf", true);
  sample("tests/fixtures/async-material.gltf", true);
  chess_scene();
  /* Former multi-root rejection is intentionally supported in Cycle 45. */
  multi_root();
  meshlet_growth_fixture();
  hierarchy();
  sample("tests/fixtures/async-static-orphan.gltf", false);
  sample("tests/fixtures/async-static-cycle.gltf", false);
  textured_static_scene();
  sample("tests/fixtures/async-static-instancing.gltf", false);
  sample("tests/fixtures/async-static-variant.gltf", false);
  sample("tests/fixtures/async-static-required-extension.gltf", false);
  sample("tests/fixtures/async-static-meshopt.gltf", false);
  multi_meshlet();
  sample("tests/fixtures/async-lines.gltf", false);
  sample("tests/fixtures/async-bad-index.gltf", false);
  sample("tests/fixtures/does-not-exist.gltf", false);
  pthread_t ts[4];
  for (int i = 0; i < 4; ++i)
    assert(!pthread_create(&ts[i], NULL, parallel, NULL));
  for (int i = 0; i < 4; ++i)
    assert(!pthread_join(ts[i], NULL));
  puts("cold static glTF cooker: owned eight-stream triangle candidates OK");
  return 0;
}
