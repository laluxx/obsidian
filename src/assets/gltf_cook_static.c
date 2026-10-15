#define _POSIX_C_SOURCE 200809L
/// gltf_cook_static — worker-owned static scene preparation, without Vulkan
/* A deliberately non-textured, unskinned, non-animated glTF subset. A primitive
 * is one GPU mesh. Additional primitives attached to one source node become
 * identity child nodes: this preserves independent materials while respecting
 * OmdlNode's ONE mesh_idx field and its runtime animation/update semantics.
 * Unsupported features return no candidate (the controller uses NEEDS_COOK).
 * All sizes and topology are validated before publishing CPU ownership. */
#ifdef OBS_GLTF_STATIC_COOK_TEST
#include "static_cook_contract.h"
#else
#include "gltf_async_cache.h"
#endif
#include "cgltf.h"
#include "gltf_material.h"
#include "node_topology.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STATIC_MAX_VERTICES 2097152u
#define STATIC_MAX_NODES 1024u
#define STATIC_MAX_MESHES 256u
#define STATIC_MAX_TRIANGLES 2097152u
#define STATIC_MAX_MESHLET_VERTICES 64u
#define STATIC_MAX_MESHLET_TRIANGLES 124u

static const cgltf_accessor *find_attribute(const cgltf_primitive *p,
                                            cgltf_attribute_type type) {
  const cgltf_accessor *found = NULL;
  for (size_t i = 0; i < p->attributes_count; ++i) {
    const cgltf_attribute *a = &p->attributes[i];
    if (a->type != type)
      continue;
    /* Multiple sets (TEXCOORD_1, JOINTS_1, etc.) need full support. */
    if (a->index || found)
      return NULL;
    found = a->data;
  }
  return found;
}

static bool supported_attributes(const cgltf_primitive *p,
                                 const cgltf_accessor *pos) {
  if (!pos || pos->type != cgltf_type_vec3)
    return false;
  unsigned seen = 0;
  for (size_t i = 0; i < p->attributes_count; ++i) {
    const cgltf_attribute *a = &p->attributes[i];
    if (a->index || !a->data || a->data->count != pos->count)
      return false;
    unsigned bit;
    switch (a->type) {
    case cgltf_attribute_type_position:
      bit = 1u;
      break;
    case cgltf_attribute_type_normal:
      bit = 2u;
      break;
    case cgltf_attribute_type_texcoord:
      bit = 4u;
      break;
    case cgltf_attribute_type_tangent:
      bit = 8u;
      break;
    case cgltf_attribute_type_color:
      bit = 16u;
      break;
    default:
      return false; /* No silent loss of skin/extra streams. */
    }
    if (seen & bit)
      return false;
    seen |= bit;
    if ((a->type == cgltf_attribute_type_normal &&
         a->data->type != cgltf_type_vec3) ||
        (a->type == cgltf_attribute_type_texcoord &&
         a->data->type != cgltf_type_vec2) ||
        (a->type == cgltf_attribute_type_tangent &&
         a->data->type != cgltf_type_vec4) ||
        (a->type == cgltf_attribute_type_color &&
         a->data->type != cgltf_type_vec3 && a->data->type != cgltf_type_vec4))
      return false;
  }
  return (seen & 1u) != 0;
}

static bool finite_vec(const float *a, size_t n) {
  for (size_t k = 0; k < n; ++k)
    if (!isfinite(a[k]))
      return false;
  return true;
}

static bool decode_vertices(const cgltf_primitive *p, Vertex *dst,
                            OmdlMeshMeta *meta) {
  const cgltf_accessor *pos = find_attribute(p, cgltf_attribute_type_position);
  const cgltf_accessor *normal = find_attribute(p, cgltf_attribute_type_normal);
  const cgltf_accessor *uv = find_attribute(p, cgltf_attribute_type_texcoord);
  const cgltf_accessor *tangent =
      find_attribute(p, cgltf_attribute_type_tangent);
  const cgltf_accessor *color = find_attribute(p, cgltf_attribute_type_color);
  if (!supported_attributes(p, pos))
    return false;
  for (size_t i = 0; i < pos->count; ++i) {
    Vertex *v = &dst[i];
    if (!cgltf_accessor_read_float(pos, i, v->pos, 3))
      return false;
    v->normal[0] = 0.0f;
    v->normal[1] = 1.0f;
    v->tangent[0] = 1.0f;
    v->tangent[3] = 1.0f;
    for (unsigned j = 0; j < 4; ++j)
      v->color[j] = 1.0f;
    if ((normal && !cgltf_accessor_read_float(normal, i, v->normal, 3)) ||
        (uv && !cgltf_accessor_read_float(uv, i, v->texCoord, 2)) ||
        (tangent && !cgltf_accessor_read_float(tangent, i, v->tangent, 4)))
      return false;
    if (color) {
      if (!cgltf_accessor_read_float(color, i, v->color,
                                     color->type == cgltf_type_vec3 ? 3 : 4))
        return false;
    }
    /* No COLOR_0 means white. The fragment shader multiplies the
     * material baseColorFactor independently (never square it). */
    if (!finite_vec(v->pos, 3) || !finite_vec(v->normal, 3) ||
        !finite_vec(v->texCoord, 2) || !finite_vec(v->tangent, 4) ||
        !finite_vec(v->color, 4))
      return false;
    for (unsigned k = 0; k < 3; ++k) {
      if (i == 0 || v->pos[k] < meta->aabbMin[k])
        meta->aabbMin[k] = v->pos[k];
      if (i == 0 || v->pos[k] > meta->aabbMax[k])
        meta->aabbMax[k] = v->pos[k];
    }
  }
  return true;
}

static void material_values(const cgltf_material *mat, OmdlMeshMeta *m) {
  const float white[4] = {1, 1, 1, 1};
  const float clear[3] = {0, 0, 0};
  const float neutral[3] = {1, 1, 1};
  memcpy(m->baseColorFactor, white, sizeof(white));
  memcpy(m->emissiveFactor, clear, sizeof(clear));
  memcpy(m->attenuationColor, neutral, sizeof(neutral));
  m->metallicFactor = 1.0f;
  m->roughnessFactor = 1.0f;
  m->emissiveStrength = 1.0f;
  m->alpha_cutoff = 0.5f;
  m->aoStrength = 1.0f;
  m->ior = 1.5f;
  m->attenuationDistance = 100000.0f;
  m->visible = true;
  if (!mat)
    return;
  m->aoStrength = obs_gltf_occlusion_strength(mat);
  m->alpha_mode = mat->alpha_mode == cgltf_alpha_mode_mask    ? 1
                  : mat->alpha_mode == cgltf_alpha_mode_blend ? 2
                                                              : 0;
  m->alpha_cutoff = mat->alpha_cutoff;
  m->is_unlit = mat->unlit;
  if (mat->has_pbr_metallic_roughness) {
    memcpy(m->baseColorFactor, mat->pbr_metallic_roughness.base_color_factor,
           sizeof(m->baseColorFactor));
    m->metallicFactor = mat->pbr_metallic_roughness.metallic_factor;
    m->roughnessFactor = mat->pbr_metallic_roughness.roughness_factor;
  }
  memcpy(m->emissiveFactor, mat->emissive_factor, sizeof(m->emissiveFactor));
  if (mat->has_emissive_strength)
    m->emissiveStrength = mat->emissive_strength.emissive_strength;
  if (mat->has_ior)
    m->ior = mat->ior.ior;
  if (mat->has_transmission)
    m->transmissionFactor = mat->transmission.transmission_factor;
  if (mat->has_volume) {
    m->thicknessFactor = mat->volume.thickness_factor;
    m->attenuationDistance = mat->volume.attenuation_distance;
    memcpy(m->attenuationColor, mat->volume.attenuation_color,
           sizeof(m->attenuationColor));
  }
  if (mat->has_dispersion)
    m->dispersion = mat->dispersion.dispersion;
}

/* Meshlet offsets INSIDE each Meshlet are relative to its own mesh's section.
 * The GPU adds the per-mesh megaBaseMeshletVertex / Triangle once; storing
 * absolute pool offsets here would double-offset every mesh after mesh zero. */
static bool cook_meshlets(const Vertex *vertices, const uint32_t *indices,
                          uint32_t triangle_count, ObsGltfCacheCandidate *c,
                          OmdlMeshMeta *meta) {
  uint32_t base_ml = c->counts[OBS_GEOM_MESHLET];
  uint32_t base_mv = c->counts[OBS_GEOM_MESHLET_VERTEX];
  uint32_t base_mt = c->counts[OBS_GEOM_MESHLET_TRIANGLE];
  Meshlet *ml = (Meshlet *)c->streams[OBS_GEOM_MESHLET] + base_ml;
  MeshletBounds *bounds =
      (MeshletBounds *)c->streams[OBS_GEOM_BOUNDS] + base_ml;
  MeshletSkinData *skins =
      (MeshletSkinData *)c->streams[OBS_GEOM_SKIN] + base_ml;
  uint32_t *mv = (uint32_t *)c->streams[OBS_GEOM_MESHLET_VERTEX] + base_mv;
  uint8_t *mt = (uint8_t *)c->streams[OBS_GEOM_MESHLET_TRIANGLE] + base_mt;
  uint32_t ml_count = 0, vcursor = 0, tcursor = 0, tri = 0;
  meta->meshletOffset = base_ml;
  meta->meshletVertexOffset = base_mv;
  meta->meshletTriangleOffset = base_mt;
  while (tri < triangle_count) {
    uint32_t local[STATIC_MAX_MESHLET_VERTICES] = {0};
    uint32_t nv = 0, nt = 0;
    Meshlet *m = &ml[ml_count];
    m->vertex_offset = vcursor;
    m->triangle_offset = tcursor;
    while (tri < triangle_count && nt < STATIC_MAX_MESHLET_TRIANGLES) {
      uint8_t encoded[3];
      uint32_t novel[3], novel_count = 0;
      bool fits = true;
      for (uint32_t k = 0; k < 3; ++k) {
        uint32_t vertex = indices[3 * tri + k];
        uint32_t j = 0;
        while (j < nv && local[j] != vertex)
          ++j;
        if (j == nv) {
          uint32_t q = 0;
          while (q < novel_count && novel[q] != vertex)
            ++q;
          j += q;
          if (q == novel_count)
            novel[novel_count++] = vertex;
        }
        if (j >= STATIC_MAX_MESHLET_VERTICES) {
          fits = false;
          break;
        }
        encoded[k] = (uint8_t)j;
      }
      if (!fits || nv + novel_count > STATIC_MAX_MESHLET_VERTICES)
        break;
      for (uint32_t j = 0; j < novel_count; ++j)
        local[nv + j] = novel[j];
      nv += novel_count;
      for (uint32_t k = 0; k < 3; ++k)
        mt[tcursor + 3 * nt + k] = encoded[k];
      ++nt;
      ++tri;
    }
    if (!nt)
      return false;
    m->vertex_count = nv;
    m->triangle_count = nt;
    float center[3] = {0};
    for (uint32_t j = 0; j < nv; ++j) {
      mv[vcursor + j] = local[j];
      for (unsigned k = 0; k < 3; ++k)
        center[k] += vertices[local[j]].pos[k];
    }
    for (unsigned k = 0; k < 3; ++k)
      center[k] /= (float)nv;
    MeshletBounds *b = &bounds[ml_count];
    memcpy(b->center, center, sizeof(center));
    for (uint32_t j = 0; j < nv; ++j) {
      const Vertex *v = &vertices[local[j]];
      float d2 = 0;
      for (unsigned k = 0; k < 3; ++k) {
        float d = v->pos[k] - center[k];
        d2 += d * d;
      }
      float radius = sqrtf(d2);
      if (!isfinite(radius))
        return false;
      if (radius > b->radius)
        b->radius = radius;
    }
    b->cone_axis[2] = 1.0f;
    b->cone_cutoff = 2.0f; /* no speculative backface culling */
    skins[ml_count] = (MeshletSkinData){0};
    vcursor += nv;
    tcursor += (nt * 3 + 3u) & ~3u;
    ++ml_count;
  }
  c->counts[OBS_GEOM_MESHLET] += ml_count;
  c->counts[OBS_GEOM_BOUNDS] += ml_count;
  c->counts[OBS_GEOM_SKIN] += ml_count;
  c->counts[OBS_GEOM_MESHLET_VERTEX] += vcursor;
  c->counts[OBS_GEOM_MESHLET_TRIANGLE] += tcursor;
  meta->meshletCount = ml_count;
  return true;
}

static bool allocate_geometry(ObsGltfCacheCandidate *c, uint32_t nv,
                              uint32_t ni) {
  const uint32_t triangle_count = ni / 3u;
  const size_t sizes[OBS_GEOM_POOL_COUNT] = {
      [OBS_GEOM_VERTEX] = sizeof(Vertex),
      [OBS_GEOM_INDEX] = sizeof(uint32_t),
      [OBS_GEOM_MORPH] = sizeof(MorphDelta),
      [OBS_GEOM_MESHLET] = sizeof(Meshlet),
      [OBS_GEOM_BOUNDS] = sizeof(MeshletBounds),
      [OBS_GEOM_SKIN] = sizeof(MeshletSkinData),
      [OBS_GEOM_MESHLET_VERTEX] = sizeof(uint32_t),
      [OBS_GEOM_MESHLET_TRIANGLE] = sizeof(uint8_t)};
  const uint32_t capacities[OBS_GEOM_POOL_COUNT] = {
      [OBS_GEOM_VERTEX] = nv,
      [OBS_GEOM_INDEX] = ni,
      [OBS_GEOM_MESHLET] = triangle_count,
      [OBS_GEOM_BOUNDS] = triangle_count,
      [OBS_GEOM_SKIN] = triangle_count,
      [OBS_GEOM_MESHLET_VERTEX] = ni,
      [OBS_GEOM_MESHLET_TRIANGLE] = ni + 3u * triangle_count};
  for (size_t i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
    if (!capacities[i])
      continue;
    c->streams[i] = calloc((size_t)capacities[i], sizes[i]);
    if (!c->streams[i])
      return false;
  }
  c->counts[OBS_GEOM_VERTEX] = nv;
  c->counts[OBS_GEOM_INDEX] = ni;
  return true;
}

static bool copy_texture_path(char dst[OBS_CACHE_TEXTURE_PATH_MAX],
                              const char *source,
                              const cgltf_texture_view *view) {
  if (!view || !view->texture)
    return true;
  if (view->texcoord != 0 || view->has_transform)
    return false;
  if (!view->texture->image || !view->texture->image->uri)
    return false;
  const char *uri = view->texture->image->uri;
  if (!*uri || strncmp(uri, "data:", 5) == 0 || uri[0] == '/')
    return false;
  const char *slash = strrchr(source, '/');
  size_t directory = slash ? (size_t)(slash - source + 1) : 0;
  size_t uri_length = strlen(uri);
  if (directory + uri_length >= OBS_CACHE_TEXTURE_PATH_MAX)
    return false;
  memcpy(dst, source, directory);
  memcpy(dst + directory, uri, uri_length + 1);
  return true;
}

static bool prepare_material_texture_paths(const char *source,
                                           const cgltf_material *material,
                                           OmdlMeshMeta *meta) {
  if (!material)
    return true;
  const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
  return copy_texture_path(meta->tex_albedo, source,
                           &pbr->base_color_texture) &&
         copy_texture_path(meta->tex_metallicRoughness, source,
                           &pbr->metallic_roughness_texture) &&
         copy_texture_path(meta->tex_normal, source,
                           &material->normal_texture) &&
         copy_texture_path(meta->tex_occlusion, source,
                           &material->occlusion_texture) &&
         copy_texture_path(meta->tex_emissive, source,
                           &material->emissive_texture) &&
         (!material->has_transmission ||
          copy_texture_path(meta->tex_transmission, source,
                            &material->transmission.transmission_texture)) &&
         (!material->has_volume ||
          copy_texture_path(meta->tex_thickness, source,
                            &material->volume.thickness_texture));
}

static const char *cooked_texture_path(const void *source, size_t mesh,
                                       ObsCacheTextureChannel channel) {
  const OmdlMeshMeta *m = &((const OmdlMeshMeta *)source)[mesh];
  switch (channel) {
  case OBS_CACHE_ALBEDO:
    return m->tex_albedo;
  case OBS_CACHE_NORMAL:
    return m->tex_normal;
  case OBS_CACHE_METALLIC_ROUGHNESS:
    return m->tex_metallicRoughness;
  case OBS_CACHE_OCCLUSION:
    return m->tex_occlusion;
  case OBS_CACHE_EMISSIVE:
    return m->tex_emissive;
  case OBS_CACHE_TRANSMISSION:
    return m->tex_transmission;
  case OBS_CACHE_THICKNESS:
    return m->tex_thickness;
  default:
    return NULL;
  }
}

/* One work item per mesh *instance* primitive. Two glTF nodes may reference
 * the same mesh; they still get independent CPU/renderer Mesh records. */
typedef struct {
  const cgltf_primitive *primitive;
  const cgltf_mesh *source_mesh;
  uint32_t node, graph_node, vertices, indices, primitive_index;
} StaticWork;

static void mat_identity(float *m) {
  memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Column-major glTF/CGLM matrix multiplication, including rotated/scaled
 * translations. The cgltf_node_transform_world helper does not implement a
 * complete affine translation chain, so compute world matrices explicitly. */
static void mat_mul(const float *a, const float *b, float *out) {
  for (uint32_t column = 0; column < 4; ++column)
    for (uint32_t row = 0; row < 4; ++row) {
      float value = 0.0f;
      for (uint32_t k = 0; k < 4; ++k)
        value += a[k * 4 + row] * b[column * 4 + k];
      out[column * 4 + row] = value;
    }
}

/* Equality comparisons are defined even if a malformed cgltf reference
 * does not point into its declared node array. Do not subtract or order
 * unrelated pointers until source indices have been verified. */
static int32_t node_index(const cgltf_data *data, const cgltf_node *node) {
  for (size_t i = 0; i < data->nodes_count; ++i)
    if (&data->nodes[i] == node)
      return (int32_t)i;
  return -1;
}

static bool known_required_extensions(const cgltf_data *data) {
  static const char *const handled[] = {
      "KHR_materials_unlit",  "KHR_materials_emissive_strength",
      "KHR_materials_ior",    "KHR_materials_transmission",
      "KHR_materials_volume", "KHR_materials_dispersion",
  };
  for (size_t i = 0; i < data->extensions_required_count; ++i) {
    bool handled_extension = false;
    for (size_t j = 0; j < sizeof(handled) / sizeof(handled[0]); ++j)
      if (strcmp(data->extensions_required[i], handled[j]) == 0) {
        handled_extension = true;
        break;
      }
    if (!handled_extension)
      return false;
  }
  return true;
}

static bool prepare_hierarchy(const cgltf_data *data, const cgltf_scene *scene,
                              const StaticWork *work, size_t mesh_count,
                              OmdlSceneGraph *g, uint32_t total_nodes) {
  const uint32_t source_nodes = (uint32_t)data->nodes_count;
  uint8_t roots[STATIC_MAX_NODES] = {0};
  uint8_t reached[STATIC_MAX_NODES] = {0};
  uint32_t *stack = calloc(total_nodes, sizeof(*stack));
  uint8_t *states = calloc(total_nodes, sizeof(*states));
  if (!stack || !states) {
    free(stack);
    free(states);
    return false;
  }
  bool ok = false;
  do {
    for (size_t i = 0; i < scene->nodes_count; ++i) {
      const cgltf_node *node = scene->nodes[i];
      int32_t index = node_index(data, node);
      if (index < 0 || roots[index] || node->parent)
        break;
      roots[index] = 1;
      if (i + 1 == scene->nodes_count)
        ok = true;
    }
    if (!ok)
      break;
    for (uint32_t i = 0; i < source_nodes; ++i) {
      const cgltf_node *n = &data->nodes[i];
      OmdlNode *dest = &g->nodes[i];
      snprintf(dest->name, sizeof(dest->name), "%s",
               n->name ? n->name : "node");
      dest->parent = -1;
      dest->mesh_idx = -1;
      dest->skin_idx = -1;
      if (n->parent) {
        dest->parent = node_index(data, n->parent);
        if (dest->parent < 0) {
          ok = false;
          break;
        }
      }
      if (n->skin || n->camera || n->light || n->weights_count ||
          n->has_mesh_gpu_instancing || n->extensions_count ||
          (n->has_matrix &&
           (n->has_translation || n->has_rotation || n->has_scale))) {
        ok = false;
        break;
      }
      for (size_t j = 0; j < n->children_count; ++j) {
        int32_t child = node_index(data, n->children[j]);
        if (child < 0 || data->nodes[child].parent != n) {
          ok = false;
          break;
        }
      }
      if (!ok)
        break;
      dest->rotation[3] = 1.0f;
      dest->scale[0] = dest->scale[1] = dest->scale[2] = 1.0f;
      if (n->has_translation)
        memcpy(dest->translation, n->translation, sizeof(dest->translation));
      if (n->has_rotation)
        memcpy(dest->rotation, n->rotation, sizeof(dest->rotation));
      if (n->has_scale)
        memcpy(dest->scale, n->scale, sizeof(dest->scale));
      cgltf_node_transform_local(n, (float *)dest->base_matrix);
      if (!finite_vec((const float *)dest->base_matrix, 16) ||
          !finite_vec(dest->translation, 3) || !finite_vec(dest->rotation, 4) ||
          !finite_vec(dest->scale, 3)) {
        ok = false;
        break;
      }
    }
    if (!ok)
      break;
    for (size_t i = 0; i < mesh_count; ++i) {
      const StaticWork *w = &work[i];
      if (w->graph_node == w->node) {
        g->nodes[w->node].mesh_idx = (int32_t)i;
        continue;
      }
      OmdlNode *extra = &g->nodes[w->graph_node];
      snprintf(extra->name, sizeof(extra->name), "primitive-%u",
               w->primitive_index);
      extra->parent = (int32_t)w->node;
      extra->mesh_idx = (int32_t)i;
      extra->skin_idx = -1;
      extra->rotation[3] = 1.0f;
      extra->scale[0] = extra->scale[1] = extra->scale[2] = 1.0f;
      mat_identity((float *)extra->base_matrix);
    }
    ok = obs_node_topological_order(g->nodes, total_nodes, sizeof(OmdlNode),
                                    offsetof(OmdlNode, parent),
                                    g->topology_order, stack, states);
    if (!ok)
      break;
    for (uint32_t i = 0; i < total_nodes; ++i) {
      uint32_t index = g->topology_order[i];
      OmdlNode *n = &g->nodes[index];
      int32_t parent = n->parent;
      if (index < source_nodes) {
        reached[index] = parent < 0 ? roots[index] : reached[parent];
        if (!reached[index]) {
          ok = false;
          break;
        }
      }
      memcpy(g->local_transforms[index], n->base_matrix, sizeof(mat4));
      if (parent >= 0)
        mat_mul((const float *)g->world_transforms[parent],
                (const float *)g->local_transforms[index],
                (float *)g->world_transforms[index]);
      else
        memcpy(g->world_transforms[index], g->local_transforms[index],
               sizeof(mat4));
      if (!finite_vec((float *)g->world_transforms[index], 16)) {
        ok = false;
        break;
      }
    }
  } while (0);
  free(stack);
  free(states);
  return ok;
}

static bool plan_primitives(const cgltf_data *data, StaticWork *work,
                            size_t *mesh_count, uint32_t *node_count,
                            uint32_t *nv, uint32_t *ni) {
  uint32_t synthetic = 0;
  *mesh_count = 0;
  *nv = 0;
  *ni = 0;
  for (uint32_t n = 0; n < data->nodes_count; ++n) {
    const cgltf_node *node = &data->nodes[n];
    if (!node->mesh)
      continue;
    const cgltf_mesh *mesh = node->mesh;
    bool known_mesh = false;
    for (size_t i = 0; i < data->meshes_count; ++i)
      if (&data->meshes[i] == mesh) {
        known_mesh = true;
        break;
      }
    if (!known_mesh || node->skin || mesh->weights_count ||
        mesh->extensions_count || !mesh->primitives_count)
      return false;
    for (uint32_t p = 0; p < mesh->primitives_count; ++p) {
      const cgltf_primitive *prim = &mesh->primitives[p];
      const cgltf_accessor *pos =
          find_attribute(prim, cgltf_attribute_type_position);
      if (prim->type != cgltf_primitive_type_triangles || prim->targets_count ||
          prim->has_draco_mesh_compression || prim->mappings_count ||
          prim->extensions_count || !pos || !pos->count ||
          !supported_attributes(prim, pos))
        return false;
      for (size_t a = 0; a < prim->attributes_count; ++a)
        if (prim->attributes[a].data->is_sparse)
          return false;
      const cgltf_accessor *idx = prim->indices;
      if (idx && (idx->type != cgltf_type_scalar || idx->is_sparse ||
                  (idx->component_type != cgltf_component_type_r_8u &&
                   idx->component_type != cgltf_component_type_r_16u &&
                   idx->component_type != cgltf_component_type_r_32u)))
        return false;
      size_t count = idx ? idx->count : pos->count;
      if (!count || count % 3 || count / 3 > STATIC_MAX_TRIANGLES ||
          pos->count > STATIC_MAX_VERTICES ||
          count > STATIC_MAX_TRIANGLES * 3u ||
          *nv > STATIC_MAX_VERTICES - pos->count ||
          *ni > STATIC_MAX_TRIANGLES * 3u - count ||
          *mesh_count == STATIC_MAX_MESHES)
        return false;
      uint32_t graph_node = n;
      if (p)
        graph_node = (uint32_t)data->nodes_count + synthetic++;
      work[*mesh_count] = (StaticWork){.primitive = prim,
                                       .source_mesh = mesh,
                                       .node = n,
                                       .graph_node = graph_node,
                                       .primitive_index = p,
                                       .vertices = (uint32_t)pos->count,
                                       .indices = (uint32_t)count};
      ++*mesh_count;
      *nv += (uint32_t)pos->count;
      *ni += (uint32_t)count;
    }
  }
  *node_count = (uint32_t)data->nodes_count + synthetic;
  return *mesh_count != 0;
}

static bool cook_primitive(const StaticWork *work, ObsGltfCacheCandidate *c,
                           const char *source, uint32_t mesh_index,
                           uint32_t vertex_offset, uint32_t index_offset) {
  OmdlMeshMeta *meta = &c->metadata[mesh_index];
  Mesh *m = &c->meshes[mesh_index];
  const cgltf_primitive *p = work->primitive;
  const char *name =
      work->source_mesh->name ? work->source_mesh->name : "glTF mesh";
  if (work->source_mesh->primitives_count > 1)
    snprintf(meta->name, sizeof(meta->name), "%s/primitive-%u", name,
             work->primitive_index);
  else
    snprintf(meta->name, sizeof(meta->name), "%s", name);
  m->name = strdup(meta->name);
  if (!m->name)
    return false;
  meta->vertexOffset = vertex_offset;
  meta->indexOffset = index_offset;
  meta->vertexCount = work->vertices;
  meta->indexCount = work->indices;
  material_values(p->material, meta);
  if (!prepare_material_texture_paths(source, p->material, meta))
    return false;
  if (!finite_vec(meta->baseColorFactor, 4) ||
      !finite_vec(meta->emissiveFactor, 3) ||
      !finite_vec(meta->attenuationColor, 3) ||
      !isfinite(meta->metallicFactor) || !isfinite(meta->roughnessFactor) ||
      !isfinite(meta->emissiveStrength) || !isfinite(meta->alpha_cutoff) ||
      !isfinite(meta->aoStrength) || meta->aoStrength < 0.0f ||
      meta->aoStrength > 1.0f || !isfinite(meta->transmissionFactor) ||
      !isfinite(meta->ior) || !isfinite(meta->thicknessFactor) ||
      !isfinite(meta->attenuationDistance) || !isfinite(meta->dispersion))
    return false;
  Vertex *verts = (Vertex *)c->streams[OBS_GEOM_VERTEX] + vertex_offset;
  uint32_t *indices = (uint32_t *)c->streams[OBS_GEOM_INDEX] + index_offset;
  if (!decode_vertices(p, verts, meta))
    return false;
  for (uint32_t i = 0; i < work->indices; ++i) {
    size_t vertex = p->indices ? cgltf_accessor_read_index(p->indices, i) : i;
    if (vertex >= work->vertices)
      return false;
    indices[i] = (uint32_t)vertex;
  }
  if (!cook_meshlets(verts, indices, work->indices / 3, c, meta))
    return false;
  memcpy(m->aabbMin, meta->aabbMin, sizeof(meta->aabbMin));
  memcpy(m->aabbMax, meta->aabbMax, sizeof(meta->aabbMax));
  memcpy(m->baseColorFactor, meta->baseColorFactor,
         sizeof(meta->baseColorFactor));
  memcpy(m->emissiveFactor, meta->emissiveFactor, sizeof(meta->emissiveFactor));
  memcpy(m->attenuationColor, meta->attenuationColor,
         sizeof(meta->attenuationColor));
  m->metallicFactor = meta->metallicFactor;
  m->roughnessFactor = meta->roughnessFactor;
  m->emissiveStrength = meta->emissiveStrength;
  m->aoStrength = meta->aoStrength;
  m->alpha_mode = meta->alpha_mode;
  m->alpha_cutoff = meta->alpha_cutoff;
  m->is_unlit = meta->is_unlit;
  m->transmissionFactor = meta->transmissionFactor;
  m->ior = meta->ior;
  m->thicknessFactor = meta->thicknessFactor;
  m->attenuationDistance = meta->attenuationDistance;
  m->dispersion = meta->dispersion;
  m->visible = true;
  m->vertexCount = meta->vertexCount;
  m->indexCount = meta->indexCount;
  m->meshletCount = meta->meshletCount;
  m->megaBaseVertex = m->megaBaseIndex = m->megaBaseMeshlet = UINT32_MAX;
  m->megaBaseMeshletVertex = m->megaBaseMeshletTriangle = UINT32_MAX;
  m->morphDeltaOffset = m->dynamicBaseVertex = UINT32_MAX;
  m->textureIndex = m->normalMapIndex = m->metallicRoughIndex = -1;
  m->aoIndex = m->emissiveIndex = m->transmissionIndex = m->thicknessIndex = -1;
  m->jointOffset = -1;
  m->materialIndex = -1;
  uint32_t n = work->graph_node;
  memcpy(m->model, c->graph->world_transforms[n], sizeof(mat4));
  memcpy(m->local_transform, c->graph->local_transforms[n], sizeof(mat4));
  m->node = (void *)(uintptr_t)n; /* zero is legacy node-0 token */
  return true;
}

/// Closed subset: a complete, GPU-admissible candidate or no ownership at all
bool obs_gltf_cook_static_cpu(const char *path, ObsGltfCacheCandidate **out) {
  if (!out)
    return false;
  *out = NULL;
  if (!path || !*path || strnlen(path, 4096) >= 4096)
    return false;
  struct stat st;
  if (stat(path, &st) || st.st_size <= 0 ||
      (uint64_t)st.st_size > 16u * 1024u * 1024u)
    return false;
  cgltf_options options = {0};
  cgltf_data *data = NULL;
  if (cgltf_parse_file(&options, path, &data) != cgltf_result_success)
    return false;
  ObsGltfCacheCandidate *c = NULL;
  StaticWork *work = NULL;
  bool ok = false;
  do {
    if (!data->scenes_count || !data->nodes_count ||
        data->nodes_count > STATIC_MAX_NODES || data->skins_count ||
        data->animations_count || !known_required_extensions(data))
      break;
    bool supported_buffers = true;
    for (size_t i = 0; i < data->buffer_views_count; ++i)
      if (data->buffer_views[i].has_meshopt_compression) {
        supported_buffers = false;
        break;
      }
    if (!supported_buffers)
      break;
    const cgltf_scene *scene = data->scene ? data->scene : &data->scenes[0];
    if (!scene->nodes_count || scene->nodes_count > data->nodes_count)
      break;
    /* cgltf_validate subtracts one from accessor counts; reject zero
     * before invoking it on potentially hostile source JSON. */
    bool valid_counts = true;
    for (size_t i = 0; i < data->accessors_count; ++i)
      if (!data->accessors[i].count) {
        valid_counts = false;
        break;
      }
    if (!valid_counts)
      break;
    work = calloc(STATIC_MAX_MESHES, sizeof(*work));
    if (!work)
      break;
    size_t mesh_count = 0;
    uint32_t nodes = 0, nv = 0, ni = 0;
    if (!plan_primitives(data, work, &mesh_count, &nodes, &nv, &ni))
      break;
    uint64_t decoded_size = 0;
    for (size_t i = 0; i < data->buffers_count; ++i) {
      if (data->buffers[i].size > (128u * 1024u * 1024u) - decoded_size) {
        decoded_size = UINT64_MAX;
        break;
      }
      decoded_size += data->buffers[i].size;
    }
    if (decoded_size == UINT64_MAX)
      break;
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success)
      break;
    c = calloc(1, sizeof(*c));
    if (!c)
      break;
    c->mesh_count = mesh_count; /* destruction always sees partial names */
    c->metadata = calloc(mesh_count, sizeof(*c->metadata));
    c->meshes = calloc(mesh_count, sizeof(*c->meshes));
    c->graph = calloc(1, sizeof(*c->graph));
    c->source_path = strdup(path);
    if (!c->metadata || !c->meshes || !c->graph || !c->source_path ||
        !allocate_geometry(c, nv, ni))
      break;
    OmdlSceneGraph *g = c->graph;
    g->node_count = nodes;
    g->nodes = calloc(nodes, sizeof(*g->nodes));
    g->topology_order = calloc(nodes, sizeof(*g->topology_order));
    g->local_transforms = calloc(nodes, sizeof(*g->local_transforms));
    g->world_transforms = calloc(nodes, sizeof(*g->world_transforms));
    g->script_translations = calloc(nodes, sizeof(*g->script_translations));
    if (!g->nodes || !g->topology_order || !g->local_transforms ||
        !g->world_transforms || !g->script_translations ||
        !prepare_hierarchy(data, scene, work, mesh_count, g, nodes))
      break;
    uint32_t vo = 0, io = 0;
    for (size_t i = 0; i < mesh_count; ++i) {
      if (!cook_primitive(&work[i], c, path, (uint32_t)i, vo, io))
        break;
      vo += work[i].vertices;
      io += work[i].indices;
      if (i + 1 == mesh_count)
        ok = true;
    }
    if (!ok || vo != nv || io != ni)
      break;
    if (!obs_cache_textures_prepare(&c->textures, mesh_count,
                                    cooked_texture_path, c->metadata)) {
      ok = false;
      break;
    }
    const size_t sizes[OBS_GEOM_POOL_COUNT] = {
        sizeof(Vertex),   sizeof(uint32_t),      sizeof(MorphDelta),
        sizeof(Meshlet),  sizeof(MeshletBounds), sizeof(MeshletSkinData),
        sizeof(uint32_t), sizeof(uint8_t)};
    for (size_t i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
      c->spans[i] = (ObsGeometrySourceSpan){c->streams[i],
                                            (size_t)c->counts[i] * sizes[i]};
  } while (0);
  free(work);
  cgltf_free(data);
  if (!ok) {
    obs_gltf_cache_destroy(c);
    return false;
  }
  *out = c;
  return true;
}
