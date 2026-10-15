#define _POSIX_C_SOURCE 200809L
#include "gltf_material.h"
#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include "node_overrides.h"
#include "node_topology.h"
#include "cache_textures.h"
#include "omdl_cache.h"
#include "omdl_mesh_meta.h"
#include "gltf_async_cache.h"
#include <string.h>
#include "context.h"
#include "vulkan_setup.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>
#include "animation_editor.h"
#include <meshoptimizer.h>

/// Implementation

static bool prepare_bone_override_index(Scene *scene,
                                        const GLTFInstance *instance,
                                        OmdlSceneGraph *osg);

static int32_t gltf_texture_indices[MAX_TEXTURES];
static size_t gltf_texture_count = 0;

// Bumped magic to force rebuild of OMDL files for Meshlet Skinning
#define OMDL_MAGIC 0x4C444D54 // 'OMDT'

/* Native OMDL metadata is shared with the isolated cache worker. */

// Cache-hit texture references belong to the staged scene, not the renderer.
static ObsCacheTexturePlan s_cache_texture_plan = {0};

/// OMDL texture metadata accessor — never mutates the texture pool
static const char *omdl_texture_path(const void *source, size_t mesh,
                                      ObsCacheTextureChannel channel) {
    const OmdlMeshMeta *m = &((const OmdlMeshMeta *)source)[mesh];
    switch (channel) {
    case OBS_CACHE_ALBEDO: return m->tex_albedo;
    case OBS_CACHE_NORMAL: return m->tex_normal;
    case OBS_CACHE_METALLIC_ROUGHNESS: return m->tex_metallicRoughness;
    case OBS_CACHE_OCCLUSION: return m->tex_occlusion;
    case OBS_CACHE_EMISSIVE: return m->tex_emissive;
    case OBS_CACHE_TRANSMISSION: return m->tex_transmission;
    case OBS_CACHE_THICKNESS: return m->tex_thickness;
    default: return NULL;
    }
}

static char gltf_texture_paths[MAX_TEXTURES][512];

// Staging array for cache-miss mesh loading.
// Meshes are held here until GPU addresses are patched, then committed to the live scene.
static Mesh*  s_staged_meshes = NULL;
static size_t s_staged_mesh_count = 0;
static size_t s_staged_mesh_capacity = 0;
static bool s_staged_mesh_failed = false;

/// Owned mesh staging — never expose incomplete geometry
static void staged_mesh_reset(void) {
    obs_cache_textures_dispose(&s_cache_texture_plan);
    s_staged_mesh_count = 0;
    s_staged_mesh_failed = false;
}
static void staged_mesh_discard(void) {
    for (size_t i = 0; i < s_staged_mesh_count; ++i)
        mesh_destroy(VK_NULL_HANDLE, &s_staged_meshes[i]);
    staged_mesh_reset();
}
static bool staged_mesh_reserve(size_t needed) {
    if (needed <= s_staged_mesh_capacity)
        return true;
    if (needed > SIZE_MAX / sizeof(Mesh)) {
        s_staged_mesh_failed = true;
        return false;
    }

    size_t new_capacity = s_staged_mesh_capacity ? s_staged_mesh_capacity : 1;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    Mesh *next = realloc(s_staged_meshes, new_capacity * sizeof(*next));
    if (!next) {
        s_staged_mesh_failed = true;
        return false;
    }
    s_staged_meshes = next;
    s_staged_mesh_capacity = new_capacity;
    return true;
}
static bool staged_mesh_push(Mesh m) {
    if (s_staged_mesh_failed || !m.name) {
        s_staged_mesh_failed = true;
        mesh_destroy(VK_NULL_HANDLE, &m);
        return false;
    }
    if (s_staged_mesh_count == SIZE_MAX ||
        !staged_mesh_reserve(s_staged_mesh_count + 1)) {
        mesh_destroy(VK_NULL_HANDLE, &m);
        return false;
    }
    s_staged_meshes[s_staged_mesh_count++] = m;
    return true;
}

// OMDL Cooker Arrays
static Vertex* omdl_vertices = NULL;
static uint32_t* omdl_indices = NULL;
static MorphDelta* omdl_morphs = NULL;
static Meshlet* omdl_meshlets = NULL;
static MeshletBounds* omdl_meshlet_bounds = NULL;
static MeshletSkinData* omdl_meshlet_skins = NULL;
static uint32_t* omdl_meshlet_vertices = NULL;
static uint8_t* omdl_meshlet_triangles = NULL;
static OmdlMeshMeta* omdl_metas = NULL;
static uint32_t omdl_vertex_count = 0;
static uint32_t omdl_index_count = 0;
static uint32_t omdl_morph_count = 0;
static uint32_t omdl_meshlet_count = 0;
static uint32_t omdl_meshlet_vertex_count = 0;
static uint32_t omdl_meshlet_triangle_count = 0;
static uint32_t omdl_mesh_count = 0;

/// Failed cache cook cleanup — no scene publication
static void omdl_discard_cook_arrays(void) {
    free(omdl_vertices); omdl_vertices = NULL;
    free(omdl_indices); omdl_indices = NULL;
    free(omdl_morphs); omdl_morphs = NULL;
    free(omdl_meshlets); omdl_meshlets = NULL;
    free(omdl_meshlet_bounds); omdl_meshlet_bounds = NULL;
    free(omdl_meshlet_skins); omdl_meshlet_skins = NULL;
    free(omdl_meshlet_vertices); omdl_meshlet_vertices = NULL;
    free(omdl_meshlet_triangles); omdl_meshlet_triangles = NULL;
    free(omdl_metas); omdl_metas = NULL;
    staged_mesh_discard();
}

// OMDL Loader State
static bool is_omdl_cache_hit = false;
static OmdlMeshMeta* omdl_cache_metas = NULL;
static uint32_t omdl_cache_idx = 0;
static uint32_t omdl_cache_expected_meshes = 0;
static uint32_t omdl_base_v = 0;
static uint32_t omdl_base_i = 0;
static uint32_t omdl_base_m = 0;
static uint32_t omdl_base_ml = 0;
static uint32_t omdl_base_ml_v = 0;
static uint32_t omdl_base_ml_t = 0;
static OmdlSceneGraph* omdl_cache_osg = NULL;

/// OMDL cache preflight — no GPU work before these checks
static const size_t omdl_item_sizes[OMDL_SECTION_COUNT] = {
    [OMDL_MESHES] = sizeof(OmdlMeshMeta),
    [OMDL_VERTICES] = sizeof(Vertex),
    [OMDL_INDICES] = sizeof(uint32_t),
    [OMDL_MORPHS] = sizeof(MorphDelta),
    [OMDL_MESHLETS] = sizeof(Meshlet),
    [OMDL_MESHLET_BOUNDS] = sizeof(MeshletBounds),
    [OMDL_MESHLET_SKINS] = sizeof(MeshletSkinData),
    [OMDL_MESHLET_VERTICES] = sizeof(uint32_t),
    [OMDL_MESHLET_TRIANGLES] = sizeof(uint8_t),
    [OMDL_NODES] = sizeof(OmdlNode),
    [OMDL_SKINS] = sizeof(OmdlSkin),
    [OMDL_JOINTS] = sizeof(uint32_t),
    [OMDL_IBMS] = sizeof(mat4),
    [OMDL_ANIMATIONS] = sizeof(OmdlAnimation),
    [OMDL_CHANNELS] = sizeof(OmdlChannel),
    [OMDL_FLOATS] = sizeof(float),
};

static bool omdl_metas_valid(const OmdlMeshMeta *metas, const OmdlHeader *h) {
    for (uint32_t i = 0; i < h->mesh_count; ++i) {
        const OmdlMeshMeta *m = &metas[i];
        if (!omdl_cache_range(m->vertexOffset, m->vertexCount, h->total_vertices) ||
            !omdl_cache_range(m->indexOffset, m->indexCount, h->total_indices) ||
            ((uint64_t)m->morphCount * m->vertexCount > UINT32_MAX ||
             !omdl_cache_range(m->morphOffset, m->morphCount * m->vertexCount, h->total_morph_deltas)) ||
            !omdl_cache_range(m->meshletOffset, m->meshletCount, h->total_meshlets) ||
            m->meshletVertexOffset > h->total_meshlet_vertices ||
            m->meshletTriangleOffset > h->total_meshlet_triangles ||
            !memchr(m->name, 0, sizeof(m->name)) ||
            !memchr(m->tex_albedo, 0, sizeof(m->tex_albedo)) ||
            !memchr(m->tex_normal, 0, sizeof(m->tex_normal)) ||
            !memchr(m->tex_metallicRoughness, 0, sizeof(m->tex_metallicRoughness)) ||
            !memchr(m->tex_occlusion, 0, sizeof(m->tex_occlusion)) ||
            !memchr(m->tex_emissive, 0, sizeof(m->tex_emissive)) ||
            !memchr(m->tex_transmission, 0, sizeof(m->tex_transmission)) ||
            !memchr(m->tex_thickness, 0, sizeof(m->tex_thickness))) return false;
    }
    return true;
}

static void *omdl_cache_alloc(const OmdlLayout *layout, OmdlSection section) {
    uint64_t bytes = layout->bytes[section];
    return bytes && bytes <= SIZE_MAX ? malloc((size_t)bytes) : NULL;
}

static bool omdl_read_array(FILE *fp, const OmdlLayout *layout, OmdlSection section,
                            void **out) {
    *out = omdl_cache_alloc(layout, section);
    return (!layout->bytes[section] || *out) &&
           omdl_cache_read(fp, layout, section, *out);
}

static bool omdl_preflight_graph(FILE *fp, const OmdlLayout *layout,
                                OmdlSceneGraph **out) {
    const OmdlHeader *h = &layout->header;
    OmdlSceneGraph *g = calloc(1, sizeof(*g));
    if (!g) return false;
    g->node_count = h->node_count;
    g->skin_count = h->skin_count;
    g->anim_count = h->anim_count;
    bool ok = omdl_read_array(fp, layout, OMDL_NODES, (void**)&g->nodes) &&
              omdl_read_array(fp, layout, OMDL_SKINS, (void**)&g->skins) &&
              omdl_read_array(fp, layout, OMDL_JOINTS, (void**)&g->skin_joints) &&
              omdl_read_array(fp, layout, OMDL_IBMS, (void**)&g->skin_ibms) &&
              omdl_read_array(fp, layout, OMDL_ANIMATIONS, (void**)&g->anims) &&
              omdl_read_array(fp, layout, OMDL_CHANNELS, (void**)&g->channels) &&
              omdl_read_array(fp, layout, OMDL_FLOATS, (void**)&g->anim_floats);
    if (ok && h->node_count) {
        g->world_transforms = malloc((size_t)h->node_count * sizeof(mat4));
        g->local_transforms = malloc((size_t)h->node_count * sizeof(mat4));
        g->topology_order = malloc((size_t)h->node_count * sizeof(uint32_t));
        uint32_t *stack = malloc((size_t)h->node_count * sizeof(uint32_t));
        uint8_t *states = malloc(h->node_count);
        ok = g->world_transforms && g->local_transforms && g->topology_order &&
             stack && states && obs_node_topological_order(g->nodes, h->node_count,
                sizeof(OmdlNode), offsetof(OmdlNode, parent),
                g->topology_order, stack, states);
        free(stack);
        free(states);
    }
    for (uint32_t i=0; ok && i<h->node_count; ++i) {
        const OmdlNode *n=&g->nodes[i];
        if (!memchr(n->name, 0, sizeof(n->name)) ||
            (n->mesh_idx < -1 || (n->mesh_idx >= 0 && (uint32_t)n->mesh_idx >= h->mesh_count)) ||
            (n->skin_idx < -1 || (n->skin_idx >= 0 && (uint32_t)n->skin_idx >= h->skin_count))) ok=false;
    }
    for (uint32_t i=0; ok && i<h->skin_count; ++i) {
        const OmdlSkin *skin = &g->skins[i];
        if (!omdl_cache_range(skin->joints_offset, skin->joints_count, h->total_joints) ||
            !omdl_cache_range(skin->ibm_offset, skin->joints_count, h->total_joints)) ok=false;
    }
    for (uint32_t i=0; ok && i<h->total_joints; ++i)
        if (g->skin_joints[i] >= h->node_count) ok=false;
    for (uint32_t i=0; ok && i<h->anim_count; ++i) {
        const OmdlAnimation *anim=&g->anims[i];
        if (!memchr(anim->name, 0, sizeof(anim->name)) ||
            !isfinite(anim->duration) || anim->duration < 0.0f ||
            !omdl_cache_range(anim->channel_offset, anim->channel_count, h->total_channels)) ok=false;
    }
    for (uint32_t i=0; ok && i<h->total_channels; ++i) {
        const OmdlChannel *chan=&g->channels[i];
        uint32_t stride = chan->path_type == cgltf_animation_path_type_rotation ? 4 : 3;
        uint64_t values = (uint64_t)chan->keyframe_count * stride;
        /* Morph channel values are variable-width; bound the first sample here.
         * Complete morph stride needs the target mesh's morph count at runtime. */
        if (chan->target_node != UINT32_MAX && chan->target_node >= h->node_count) ok=false;
        if (chan->path_type <= cgltf_animation_path_type_invalid ||
            chan->path_type >= cgltf_animation_path_type_max_enum ||
            !chan->keyframe_count ||
            !omdl_cache_range(chan->times_offset, chan->keyframe_count, h->total_floats) ||
            (chan->path_type == cgltf_animation_path_type_weights ?
                !omdl_cache_range(chan->values_offset, 1, h->total_floats) :
                (values > UINT32_MAX || !omdl_cache_range(chan->values_offset, (uint32_t)values, h->total_floats)))) ok=false;
    }
    if (!ok) {
        omdl_scene_graph_destroy(g);
        return false;
    }
    *out=g;
    return true;
}

/// V2 provenance — source document plus decoded glTF buffer contents
/* Source identity is based on the JSON/GLB source file and every decoded
 * buffer as cgltf sees it. It deliberately does not hash external image
 * resources: they are managed by the separate texture cache. CRC64 protects
 * against accidental stale/corrupt data, not deliberate modification. */
static bool omdl_source_digest(const cgltf_data *data, uint64_t *out) {
    if (!data || !out || (!data->file_data && data->file_size)) return false;
    uint64_t digest =
        omdl_cache_digest_bytes(0, data->file_data, data->file_size);
    digest = omdl_cache_digest_size(digest, data->file_size);
    digest = omdl_cache_digest_size(digest, data->buffers_count);
    for (size_t i=0; i<data->buffers_count; ++i) {
        const cgltf_buffer *buffer = &data->buffers[i];
        if (buffer->size && !buffer->data) return false;
        digest = omdl_cache_digest_size(digest, buffer->size);
        digest = omdl_cache_digest_bytes(digest, buffer->data, buffer->size);
    }
    *out = digest;
    return true;
}

/* Conservative build-ABI identity for the still-native OMDL payload.
 * Full cross-architecture portability requires field-wise serialization. */
static uint64_t omdl_native_abi_tag(void) {
    const size_t layout[] = {
        sizeof(bool), sizeof(int), sizeof(void*),
        _Alignof(OmdlHeader), _Alignof(OmdlMeshMeta),
        _Alignof(Vertex), _Alignof(OmdlNode),
        offsetof(OmdlMeshMeta, vertexOffset),
        offsetof(OmdlMeshMeta, baseColorFactor),
        offsetof(OmdlMeshMeta, aoStrength),
        offsetof(OmdlMeshMeta, tex_albedo),
        offsetof(OmdlNode, parent),
        offsetof(OmdlNode, base_matrix),
        sizeof(cgltf_animation_path_type),
    };
    uint64_t tag = omdl_cache_abi_tag(omdl_item_sizes, layout,
                                       sizeof(layout)/sizeof(layout[0]));
    /* Same sizeof/offsetof on opposite-endian targets is not compatible
     * with the current native payload, even though the envelope is LE. */
    const uint32_t endian_probe = UINT32_C(0x01020304);
    return omdl_cache_digest_bytes(tag, &endian_probe, sizeof endian_probe);
}

/// Source/cache identity sanity (conservative: never guess on ambiguous glTF)
static bool omdl_source_mesh_count(const cgltf_data *data, uint32_t *count) {
    if (!data || !count) return false;
    if (!data->scenes_count) { *count = 0; return true; }
    const cgltf_scene *root = data->scene ? data->scene : &data->scenes[0];
    uint64_t total = 0;
    for (size_t i=0; i<data->nodes_count; ++i) {
        const cgltf_node *node = &data->nodes[i];
        const cgltf_node *top = node;
        size_t remaining = data->nodes_count;
        while (top->parent && remaining) { top = top->parent; --remaining; }
        if (top->parent) return false; // malformed/cyclic hierarchy
        bool in_scene=false;
        for (size_t r=0; r<root->nodes_count; ++r)
            if (root->nodes[r] == top) { in_scene=true; break; }
        if (!in_scene || !node->mesh) continue;
        for (size_t pi=0; pi<node->mesh->primitives_count; ++pi) {
            const cgltf_primitive *prim=&node->mesh->primitives[pi];
            bool has_vertices=false;
            for (size_t a=0; a<prim->attributes_count; ++a) {
                if (prim->attributes[a].type == cgltf_attribute_type_position &&
                    prim->attributes[a].data && prim->attributes[a].data->count)
                    has_vertices=true;
            }
            /* The old cached fast path assumes one metadata entry per
             * primitive; bypass it for empty or unsupported primitives. */
            if (!has_vertices || ++total > UINT32_MAX) return false;
        }
    }
    *count=(uint32_t)total;
    return true;
}

static bool omdl_cache_preflight(FILE *fp, OmdlLayout *layout,
                                 uint64_t source_digest, uint64_t abi_tag,
                                 OmdlMeshMeta **metas, OmdlSceneGraph **graph) {
    if (!omdl_cache_layout_v2(fp, OMDL_MAGIC, omdl_item_sizes,
                              source_digest, abi_tag, layout)) return false;
    OmdlMeshMeta *m=NULL;
    OmdlSceneGraph *g=NULL;
    bool ok=omdl_read_array(fp, layout, OMDL_MESHES, (void**)&m) &&
            omdl_metas_valid(m, &layout->header) &&
            omdl_preflight_graph(fp, layout, &g);
    /* Weight channels have per-mesh widths rather than a fixed vec3/quat
     * stride. Validate every referenced mesh's complete sample range. */
    for (uint32_t c=0; ok && c<layout->header.total_channels; ++c) {
        const OmdlChannel *ch=&g->channels[c];
        if (ch->path_type != cgltf_animation_path_type_weights) continue;
        for (uint32_t n=0; ok && n<g->node_count; ++n) {
            if (n != ch->target_node || g->nodes[n].mesh_idx < 0) continue;
            const OmdlMeshMeta *meta=&m[g->nodes[n].mesh_idx];
            uint64_t values=(uint64_t)meta->morphCount * ch->keyframe_count;
            if (values > UINT32_MAX ||
                !omdl_cache_range(ch->values_offset, (uint32_t)values,
                                  layout->header.total_floats)) ok=false;
        }
    }
    if (!ok) {
        free(m);
        omdl_scene_graph_destroy(g);
        return false;
    }
    *metas=m;
    *graph=g;
    return true;
}


typedef struct {
    cgltf_node* node;
    size_t mesh_start_index;
    size_t mesh_count;
} NodeMeshMapping;

static NodeMeshMapping node_mappings[256];
static size_t node_mapping_count = 0;

static int global_joint_counter = 0;
extern mat4* jointSSBOMapped[MAX_FRAMES_IN_FLIGHT];
extern bool scene_topology_dirty;

extern uint32_t megaBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t vertexCount);
extern uint32_t megaIndexBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t indexCount);
extern uint32_t megaMorphBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t deltaCount);
extern uint32_t megaMeshletBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t count);
extern uint32_t megaMeshletVertexBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t count);
extern uint32_t megaMeshletTriangleBufferAllocateFromFile(VulkanContext* ctx, FILE* f, uint32_t count);
extern uint32_t megaMeshletBufferAllocate(VulkanContext* ctx, Meshlet* meshlets, MeshletBounds* bounds, MeshletSkinData* skins, uint32_t count);
extern uint32_t megaMeshletVertexBufferAllocate(VulkanContext* ctx, uint32_t* vertices, uint32_t count);
extern uint32_t megaMeshletTriangleBufferAllocate(VulkanContext* ctx, uint8_t* triangles, uint32_t count);

void get_directory(const char* filepath, char* dir, size_t dir_size) {
    const char* last_slash = strrchr(filepath, '/');
    if (last_slash) {
        size_t len = last_slash - filepath + 1;
        if (len < dir_size) {
            memcpy(dir, filepath, len);
            dir[len] = '\0';
        }
    } else {
        dir[0] = '\0';
    }
}

bool load_gltf_textures(cgltf_data* data, const char* base_path) {
    if (data->textures_count == 0) return true;

    char dir[512];
    get_directory(base_path, dir, sizeof(dir));
    gltf_texture_count = 0;
    memset(gltf_texture_indices, -1, sizeof(gltf_texture_indices));
    memset(gltf_texture_paths, 0, sizeof(gltf_texture_paths));

    for (size_t i = 0; i < data->textures_count; i++) {
        cgltf_texture* tex = &data->textures[i];
        if (!tex->image) { gltf_texture_indices[i] = -1; continue; }
        cgltf_image* img = tex->image;
        int32_t tex_id = -1;

        if (img->uri && !strstr(img->uri, "data:")) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s%s", dir, img->uri);
            strcpy(gltf_texture_paths[i], full_path);
            tex_id = texture_pool_add(&context, full_path);
        }
        else if (img->buffer_view) {
            char virtual_filename[1024];
            const char* home = getenv("HOME");
            if (!home) home = ".";

            char tdir[512];
            snprintf(tdir, sizeof(tdir), "%s/.cache", home); mkdir(tdir, 0777);
            snprintf(tdir, sizeof(tdir), "%s/.cache/obsidian", home); mkdir(tdir, 0777);
            snprintf(tdir, sizeof(tdir), "%s/.cache/obsidian/textures", home); mkdir(tdir, 0777);

            char safe_name[256];
            strncpy(safe_name, base_path, sizeof(safe_name) - 1);
            safe_name[sizeof(safe_name) - 1] = '\0';
            for (int k = 0; safe_name[k]; k++) {
                if (safe_name[k] == '/' || safe_name[k] == '\\' || safe_name[k] == '.') safe_name[k] = '_';
            }

            snprintf(virtual_filename, sizeof(virtual_filename), "%s/.cache/obsidian/textures/%s_embedded_%zu.png", home, safe_name, i);
            strcpy(gltf_texture_paths[i], virtual_filename);

            cgltf_buffer_view* view = img->buffer_view;
            unsigned char* buffer_data = (unsigned char*)view->buffer->data + view->offset;

            FILE* f = fopen(virtual_filename, "rb");
            if (f) {
                fclose(f);
            } else {
                f = fopen(virtual_filename, "wb");
                if (f) {
                    fwrite(buffer_data, 1, view->size, f);
                    fclose(f);
                }
            }

            tex_id = texture_pool_add(&context, virtual_filename);
        }
        else {
            gltf_texture_indices[i] = -1; continue;
        }

        if (tex_id >= 0) {
            gltf_texture_indices[i] = tex_id;
            gltf_texture_count = i + 1;
        }
    }
    return true;
}

// CPU Morph Extraction: pack all target deltas into a staging array.
static MorphDelta* extract_morph_targets(cgltf_primitive* prim, uint32_t vertex_count) {
    if (prim->targets_count == 0) return NULL;
    size_t target_count = prim->targets_count;

    cgltf_accessor** pos_accessors = calloc(target_count, sizeof(cgltf_accessor*));
    cgltf_accessor** nrm_accessors = calloc(target_count, sizeof(cgltf_accessor*));

    for (size_t t = 0; t < target_count; t++) {
        cgltf_morph_target* target = &prim->targets[t];
        for (size_t a = 0; a < target->attributes_count; a++) {
            cgltf_attribute* attr = &target->attributes[a];
            if (attr->type == cgltf_attribute_type_position) pos_accessors[t] = attr->data;
            else if (attr->type == cgltf_attribute_type_normal) nrm_accessors[t] = attr->data;
        }
    }

    size_t total_deltas = (size_t)vertex_count * target_count;
    MorphDelta* staging = calloc(total_deltas, sizeof(MorphDelta));

    for (uint32_t v = 0; v < vertex_count; v++) {
        for (size_t t = 0; t < target_count; t++) {
            uint32_t idx = v * (uint32_t)target_count + (uint32_t)t;
            if (pos_accessors[t] && v < pos_accessors[t]->count) {
                float p[3];
                cgltf_accessor_read_float(pos_accessors[t], v, p, 3);
                staging[idx].pos_delta[0] = p[0];
                staging[idx].pos_delta[1] = p[1];
                staging[idx].pos_delta[2] = p[2];
                staging[idx].pos_delta[3] = 0.0f;
            }
            if (nrm_accessors[t] && v < nrm_accessors[t]->count) {
                float n[3];
                cgltf_accessor_read_float(nrm_accessors[t], v, n, 3);
                staging[idx].normal_delta[0] = n[0];
                staging[idx].normal_delta[1] = n[1];
                staging[idx].normal_delta[2] = n[2];
                staging[idx].normal_delta[3] = 0.0f;
            }
        }
    }

    free(pos_accessors);
    free(nrm_accessors);
    return staging;
}

/// create_mesh_from_primitive — implementation group
static Mesh create_mesh_from_primitive(cgltf_primitive* prim, cgltf_data* data, const char* name) {
    Mesh mesh = {0};
    mesh.name = strdup(name);
    mesh.textureIndex = -1;
    mesh.normalMapIndex = -1;
    mesh.metallicRoughIndex = -1;
    mesh.aoIndex = -1;
    mesh.aoStrength = 1.0f;
    mesh.emissiveIndex = -1;
    mesh.transmissionIndex = -1;
    mesh.thicknessIndex = -1;
    mesh.node = NULL;
    mesh.morph_data = NULL;
    mesh.is_unlit = false;
    mesh.alpha_mode = 0;
    mesh.alpha_cutoff = 0.5f;
    mesh.jointOffset = -1;
    mesh.jointCount = 0;
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);

    // OMDL CACHE HIT PATH: INSTANT RECONSTRUCTION (ZERO CPU MATH)
    if (is_omdl_cache_hit) {
        if (!omdl_cache_metas || omdl_cache_idx >= omdl_cache_expected_meshes) {
            s_staged_mesh_failed = true;
            return mesh;
        }
        OmdlMeshMeta meta = omdl_cache_metas[omdl_cache_idx++];
        glm_vec3_copy(meta.aabbMin, mesh.aabbMin);
        glm_vec3_copy(meta.aabbMax, mesh.aabbMax);
        mesh.vertexCount = meta.vertexCount;
        mesh.indexCount = meta.indexCount;
        mesh.meshletCount = meta.meshletCount;

        mesh.megaBaseVertex = UINT32_MAX; // GPU base not yet published
        mesh.megaBaseIndex = UINT32_MAX; // GPU base not yet published
        mesh.megaBaseMeshlet = UINT32_MAX; // GPU base not yet published
        mesh.megaBaseMeshletVertex = UINT32_MAX; // GPU base not yet published
        mesh.megaBaseMeshletTriangle = UINT32_MAX; // GPU base not yet published
        mesh.dynamicBaseVertex = UINT32_MAX;

        glm_vec4_copy(meta.baseColorFactor, mesh.baseColorFactor);
        mesh.metallicFactor = meta.metallicFactor;
        mesh.roughnessFactor = meta.roughnessFactor;
        mesh.emissiveStrength = meta.emissiveStrength;
        mesh.aoStrength = meta.aoStrength;
        glm_vec3_copy(meta.emissiveFactor, mesh.emissiveFactor);
        mesh.alpha_mode = meta.alpha_mode;
        mesh.alpha_cutoff = meta.alpha_cutoff;
        mesh.is_unlit = meta.is_unlit;
        mesh.visible = meta.visible;
        mesh.transmissionFactor = meta.transmissionFactor;
        mesh.ior = meta.ior;
        mesh.thicknessFactor = meta.thicknessFactor;
        glm_vec3_copy(meta.attenuationColor, mesh.attenuationColor);
        mesh.attenuationDistance = meta.attenuationDistance;
        mesh.dispersion = meta.dispersion;

        // Texture-pool mutations are deferred until the GPU upload is confirmed.
        if (meta.morphCount > 0) {
            mesh.morphCount = meta.morphCount;
            mesh.morphDeltaOffset = UINT32_MAX; // patched after GPU completion
            mesh.morph_data = calloc(1, sizeof(MorphData));
            if (!mesh.morph_data) s_staged_mesh_failed = true;
            else {
                mesh.morph_data->target_count = meta.morphCount;
                mesh.morph_data->weights = calloc(meta.morphCount, sizeof(float));
                if (!mesh.morph_data->weights) s_staged_mesh_failed = true;
            }
        }

        return mesh;
    }
    // ====================================================================

    // Find all attribute accessors
    cgltf_accessor* pos_accessor = NULL;
    cgltf_accessor* normal_accessor = NULL;
    cgltf_accessor* texcoord_accessor = NULL;
    cgltf_accessor* color_accessor = NULL;
    cgltf_accessor* tangent_accessor = NULL;
    cgltf_accessor* joints_accessor = NULL;
    cgltf_accessor* weights_accessor = NULL;

    for (size_t j = 0; j < prim->attributes_count; ++j) {
        cgltf_attribute* attr = &prim->attributes[j];
        switch (attr->type) {
        case cgltf_attribute_type_position:
            pos_accessor = attr->data;
            break;
        case cgltf_attribute_type_normal:
            normal_accessor = attr->data;
            break;
        case cgltf_attribute_type_texcoord:
            texcoord_accessor = attr->data;
            break;
        case cgltf_attribute_type_color:
            color_accessor = attr->data;
            break;
        case cgltf_attribute_type_tangent:
            tangent_accessor = attr->data;
            break;
        case cgltf_attribute_type_joints:
            joints_accessor = attr->data;
            break;
        case cgltf_attribute_type_weights:
            weights_accessor = attr->data;
            break;
        default:
            break;
        }
    }

    if (!pos_accessor) {
        printf("  Primitive has no position data\n");
        return mesh;
    }

    size_t vertex_count = pos_accessor->count;

    // Handle indices
    cgltf_accessor* indices_accessor = prim->indices;
    size_t index_count = 0;
    uint32_t* indices = NULL;

    if (indices_accessor) {
        index_count = indices_accessor->count;
        indices = malloc(index_count * sizeof(uint32_t));

        for (size_t i = 0; i < index_count; i++) {
            indices[i] = (uint32_t)cgltf_accessor_read_index(indices_accessor, i);
        }

        printf("  -> Has %zu indices\n", index_count);
    } else {
        printf("  -> No indices (drawing sequential)\n");
    }

    // Get material base color and check for unlit extension
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool is_unlit = false;

    if (prim->material) {
        // Get base color from material
        if (prim->material->has_pbr_metallic_roughness) {
            cgltf_pbr_metallic_roughness* pbr = &prim->material->pbr_metallic_roughness;
            memcpy(base_color, pbr->base_color_factor, sizeof(base_color));
            printf("  -> Material base color: (%.2f, %.2f, %.2f, %.2f)\n",
                   base_color[0], base_color[1], base_color[2], base_color[3]);
        }

        // Check for unlit extension using cgltf's built-in flag
        is_unlit = prim->material->unlit;

        // Load alpha mode from glTF material
        switch (prim->material->alpha_mode) {
        case cgltf_alpha_mode_opaque:
            mesh.alpha_mode = 0;
            printf("  -> Alpha mode: OPAQUE\n");
            break;
        case cgltf_alpha_mode_mask:
            mesh.alpha_mode = 1;
            mesh.alpha_cutoff = prim->material->alpha_cutoff;
            printf("  -> Alpha mode: MASK (cutoff: %.2f)\n", mesh.alpha_cutoff);
            break;
        case cgltf_alpha_mode_blend:
            mesh.alpha_mode = 2;
            printf("  -> Alpha mode: BLEND\n");
            break;
        default:
            mesh.alpha_mode = 0;
            break;
        }

        if (is_unlit) {
            printf("  -> Material is UNLIT (KHR_materials_unlit)\n");
        } else {
            printf("  -> Material is LIT (standard PBR)\n");
        }
    } else {
        printf("  -> No material, using default white color\n");
    }

    mesh.is_unlit = is_unlit;
    mesh.visible = true;

    /* PBR material defaults */
    glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, mesh.baseColorFactor);
    mesh.metallicFactor    = 0.0f; // Fix: Default to dielectric (plastic), not metal!
    mesh.roughnessFactor   = 0.5f; // Fix: Default to matte, not a perfect mirror!
    mesh.emissiveStrength  = 1.0f;
    mesh.aoStrength        = 1.0f;
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, mesh.emissiveFactor);
    mesh.normalMapIndex      = -1;
    mesh.metallicRoughIndex  = -1;
    mesh.aoIndex             = -1;
    mesh.emissiveIndex       = -1;

    mesh.transmissionFactor = 0.0f;
    mesh.ior                = 1.5f;
    mesh.thicknessFactor    = 0.0f;
    mesh.transmissionIndex  = -1;
    mesh.thicknessIndex     = -1;
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, mesh.attenuationColor);
    mesh.attenuationDistance = 100000.0f;
    mesh.dispersion = 0.0f;

    if (prim->material) {
        if (prim->material->has_dispersion) {
            mesh.dispersion = prim->material->dispersion.dispersion;
        }
        if (prim->material->has_ior) {
            mesh.ior = prim->material->ior.ior;
        }
        if (prim->material->has_transmission) {
            mesh.transmissionFactor = prim->material->transmission.transmission_factor;
            if (prim->material->transmission.transmission_texture.texture) {
                for (size_t t = 0; t < data->textures_count; t++) {
                    if (&data->textures[t] == prim->material->transmission.transmission_texture.texture) {
                        if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                            mesh.transmissionIndex = gltf_texture_indices[t];
                        break;
                    }
                }
            }
            // Transmission meshes are physically opaque — they compute their own
            // background via screen-space refraction. Do NOT set alpha_mode = 2.
            // They are drawn in a dedicated transmission pass after the screen copy.
        }
        if (prim->material->has_volume) {
            mesh.thicknessFactor = prim->material->volume.thickness_factor;
            memcpy(mesh.attenuationColor, prim->material->volume.attenuation_color, sizeof(vec3));
            if (prim->material->volume.attenuation_distance > 0.0f) {
                mesh.attenuationDistance = prim->material->volume.attenuation_distance;
            }
            if (prim->material->volume.thickness_texture.texture) {
                for (size_t t = 0; t < data->textures_count; t++) {
                    if (&data->textures[t] == prim->material->volume.thickness_texture.texture) {
                        if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                            mesh.thicknessIndex = gltf_texture_indices[t];
                        break;
                    }
                }
            }
        }
    }

    if (prim->material && prim->material->has_pbr_metallic_roughness) {
        cgltf_pbr_metallic_roughness* pbr = &prim->material->pbr_metallic_roughness;
        memcpy(mesh.baseColorFactor, pbr->base_color_factor, sizeof(vec4));
        mesh.metallicFactor   = pbr->metallic_factor;
        mesh.roughnessFactor  = pbr->roughness_factor;

        /* Normal map */
        if (prim->material->normal_texture.texture) {
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] == prim->material->normal_texture.texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                        mesh.normalMapIndex = gltf_texture_indices[t];
                    break;
                }
            }
        }

        /* Metallic-roughness combined map */
        if (pbr->metallic_roughness_texture.texture) {
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] == pbr->metallic_roughness_texture.texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                        mesh.metallicRoughIndex = gltf_texture_indices[t];
                    break;
                }
            }
        }

        /* Emissive map */
        if (prim->material->emissive_texture.texture) {
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] == prim->material->emissive_texture.texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                        mesh.emissiveIndex = gltf_texture_indices[t];
                    break;
                }
            }
        }

        /* Emissive factor and KHR_materials_emissive_strength */
        glm_vec3_copy(prim->material->emissive_factor, mesh.emissiveFactor);
        if (prim->material->has_emissive_strength)
            mesh.emissiveStrength = prim->material->emissive_strength.emissive_strength;
    }
    if (prim->material) {
        mesh.aoStrength = obs_gltf_occlusion_strength(prim->material);
        if (prim->material->occlusion_texture.texture) {
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] ==
                    prim->material->occlusion_texture.texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                        mesh.aoIndex = gltf_texture_indices[t];
                    break;
                }
            }
        }
    }

    uint32_t final_vertex_count = (uint32_t)vertex_count;
    uint32_t final_index_count = (uint32_t)index_count;
    uint32_t final_morph_count = (uint32_t)prim->targets_count;

    Vertex* final_vertices = malloc(final_vertex_count * sizeof(Vertex));
    uint32_t* final_indices = NULL;
    MorphDelta* final_morphs = NULL;

    for (size_t v = 0; v < vertex_count; v++) {
        if (pos_accessor) cgltf_accessor_read_float(pos_accessor, v, final_vertices[v].pos, 3);
        if (normal_accessor) {
            cgltf_accessor_read_float(normal_accessor, v, final_vertices[v].normal, 3);
        } else {
            final_vertices[v].normal[0] = 0.0f; final_vertices[v].normal[1] = 1.0f; final_vertices[v].normal[2] = 0.0f;
        }
        if (texcoord_accessor) {
            cgltf_accessor_read_float(texcoord_accessor, v, final_vertices[v].texCoord, 2);
        } else {
            final_vertices[v].texCoord[0] = 0.0f; final_vertices[v].texCoord[1] = 0.0f;
        }
        if (tangent_accessor) {
            cgltf_accessor_read_float(tangent_accessor, v, final_vertices[v].tangent, 4);
        } else {
            final_vertices[v].tangent[0] = 1.0f; final_vertices[v].tangent[1] = 0.0f; final_vertices[v].tangent[2] = 0.0f; final_vertices[v].tangent[3] = 1.0f;
        }
        if (color_accessor) {
            cgltf_accessor_read_float(color_accessor, v, final_vertices[v].color, 4);
        } else {
            final_vertices[v].color[0] = base_color[0]; final_vertices[v].color[1] = base_color[1]; final_vertices[v].color[2] = base_color[2]; final_vertices[v].color[3] = base_color[3];
        }
        final_vertices[v].textureIndex = 0;
        if (joints_accessor) {
            uint32_t j[4] = {0,0,0,0};
            cgltf_accessor_read_uint(joints_accessor, v, j, 4);
            final_vertices[v].joints[0] = j[0]; final_vertices[v].joints[1] = j[1]; final_vertices[v].joints[2] = j[2]; final_vertices[v].joints[3] = j[3];
        } else {
            final_vertices[v].joints[0] = 0; final_vertices[v].joints[1] = 0; final_vertices[v].joints[2] = 0; final_vertices[v].joints[3] = 0;
        }
        if (weights_accessor) {
            cgltf_accessor_read_float(weights_accessor, v, final_vertices[v].weights, 4);
        } else {
            final_vertices[v].weights[0] = 0.0f; final_vertices[v].weights[1] = 0.0f; final_vertices[v].weights[2] = 0.0f; final_vertices[v].weights[3] = 0.0f;
        }
    }

    if (index_count > 0 && indices != NULL) {
        // AAA Optimization: Spatially sort triangles before any other optimization.
        // This guarantees that triangles physically near each other in 3D space
        // are packed together in the index buffer, slashing meshlet bounding sphere radiuses.
        meshopt_spatialSortTriangles(indices, indices, index_count, &final_vertices[0].pos[0], final_vertex_count, sizeof(Vertex));

        meshopt_optimizeVertexCache(indices, indices, index_count, final_vertex_count);
        meshopt_optimizeOverdraw(indices, indices, index_count, &final_vertices[0].pos[0], final_vertex_count, sizeof(Vertex), 1.05f);
        meshopt_optimizeVertexFetch(final_vertices, indices, index_count, final_vertices, final_vertex_count, sizeof(Vertex));
    }

    final_indices = indices;
    indices = NULL;

    final_morphs = extract_morph_targets(prim, final_vertex_count);

    Meshlet* final_meshlets = NULL;
    MeshletBounds* final_bounds = NULL;
    MeshletSkinData* final_skins = NULL;
    uint32_t* final_meshlet_vertices = NULL;
    uint8_t* final_meshlet_triangles = NULL;
    size_t final_meshlet_count = 0;
    size_t final_ml_vertex_count = 0;
    size_t final_ml_triangle_count = 0;

    if (final_index_count > 0 && final_indices != NULL) {
        size_t max_meshlets = meshopt_buildMeshletsBound(final_index_count, 64, 124);
        final_meshlets = malloc(max_meshlets * sizeof(Meshlet));
        final_bounds = malloc(max_meshlets * sizeof(MeshletBounds));
        final_skins = malloc(max_meshlets * sizeof(MeshletSkinData));
        final_meshlet_vertices = malloc(max_meshlets * 64 * sizeof(uint32_t));
        final_meshlet_triangles = malloc(max_meshlets * 124 * 3 * sizeof(uint8_t));

        struct meshopt_Meshlet* mo_meshlets = malloc(max_meshlets * sizeof(struct meshopt_Meshlet));

        final_meshlet_count = meshopt_buildMeshlets(
            mo_meshlets, final_meshlet_vertices, final_meshlet_triangles,
            final_indices, final_index_count,
            &final_vertices[0].pos[0], final_vertex_count, sizeof(Vertex),
            64, 124, 0.7f // AAA Optimization: Cone Weight. Groups triangles with identical normals to weaponize Task Shader backface culling.
            );

        for (size_t i = 0; i < final_meshlet_count; i++) {
            final_meshlets[i].vertex_offset = mo_meshlets[i].vertex_offset;
            final_meshlets[i].triangle_offset = mo_meshlets[i].triangle_offset;
            final_meshlets[i].vertex_count = mo_meshlets[i].vertex_count;
            final_meshlets[i].triangle_count = mo_meshlets[i].triangle_count;

            struct meshopt_Bounds bounds = meshopt_computeMeshletBounds(
                &final_meshlet_vertices[mo_meshlets[i].vertex_offset],
                &final_meshlet_triangles[mo_meshlets[i].triangle_offset],
                mo_meshlets[i].triangle_count,
                &final_vertices[0].pos[0], final_vertex_count, sizeof(Vertex)
                );

            final_bounds[i].center[0] = bounds.center[0];
            final_bounds[i].center[1] = bounds.center[1];
            final_bounds[i].center[2] = bounds.center[2];
            final_bounds[i].radius = bounds.radius;
            final_bounds[i].cone_apex[0] = bounds.cone_apex[0];
            final_bounds[i].cone_apex[1] = bounds.cone_apex[1];
            final_bounds[i].cone_apex[2] = bounds.cone_apex[2];
            final_bounds[i].cone_axis[0] = bounds.cone_axis[0];
            final_bounds[i].cone_axis[1] = bounds.cone_axis[1];
            final_bounds[i].cone_axis[2] = bounds.cone_axis[2];
            final_bounds[i].cone_cutoff = bounds.cone_cutoff;

            // AAA Phase 1: Accumulate average bone influences for this meshlet
            float joint_sums[256] = {0};
            uint32_t active_joints[256];
            uint32_t active_joint_count = 0;

            for (uint32_t j = 0; j < mo_meshlets[i].vertex_count; j++) {
                uint32_t local_v_idx = final_meshlet_vertices[mo_meshlets[i].vertex_offset + j];
                Vertex* v = &final_vertices[local_v_idx];
                for (int w = 0; w < 4; w++) {
                    if (v->weights[w] > 0.0f) {
                        uint32_t joint = v->joints[w];
                        if (joint < 256) {
                            if (joint_sums[joint] == 0.0f) {
                                active_joints[active_joint_count++] = joint;
                            }
                            joint_sums[joint] += v->weights[w];
                        }
                    }
                }
            }

            MeshletSkinData skin = { {0,0,0,0}, {0.0f, 0.0f, 0.0f, 0.0f} };
            for (int t = 0; t < 4; t++) {
                float max_w = 0.0f;
                uint32_t best_j = 0;
                for (uint32_t k = 0; k < active_joint_count; k++) {
                    uint32_t joint = active_joints[k];
                    if (joint_sums[joint] > max_w) {
                        max_w = joint_sums[joint];
                        best_j = joint;
                    }
                }
                if (max_w > 0.0f) {
                    skin.joints[t] = best_j;
                    skin.weights[t] = max_w;
                    joint_sums[best_j] = 0.0f; // remove from next iteration
                }
            }

            float sum = skin.weights[0] + skin.weights[1] + skin.weights[2] + skin.weights[3];
            if (sum > 0.0f) {
                skin.weights[0] /= sum; skin.weights[1] /= sum;
                skin.weights[2] /= sum; skin.weights[3] /= sum;
            }
            final_skins[i] = skin;

            final_ml_vertex_count += mo_meshlets[i].vertex_count;
            final_ml_triangle_count += ((mo_meshlets[i].triangle_count * 3 + 3) & ~3);
        }

        // Recalculate exactly how much byte data we generated to avoid padding overflow reads
        final_ml_triangle_count = 0;
        for(size_t i = 0; i < final_meshlet_count; i++) {
            size_t end = mo_meshlets[i].triangle_offset + ((mo_meshlets[i].triangle_count * 3 + 3) & ~3);
            if (end > final_ml_triangle_count) final_ml_triangle_count = end;
        }

        free(mo_meshlets);
    }

    vec3 bmin = { 1e30f,  1e30f,  1e30f};
    vec3 bmax = {-1e30f, -1e30f, -1e30f};
    for (size_t v = 0; v < vertex_count; v++) {
        bmin[0] = fminf(bmin[0], final_vertices[v].pos[0]);
        bmin[1] = fminf(bmin[1], final_vertices[v].pos[1]);
        bmin[2] = fminf(bmin[2], final_vertices[v].pos[2]);
        bmax[0] = fmaxf(bmax[0], final_vertices[v].pos[0]);
        bmax[1] = fmaxf(bmax[1], final_vertices[v].pos[1]);
        bmax[2] = fmaxf(bmax[2], final_vertices[v].pos[2]);
    }
    glm_vec3_copy(bmin, mesh.aabbMin);
    glm_vec3_copy(bmax, mesh.aabbMax);

    // OMDL COOKER PATH: Append to massive flat arrays
    OmdlMeshMeta meta = {0};
    strncpy(meta.name, name, sizeof(meta.name)-1);
    memcpy(meta.aabbMin, mesh.aabbMin, sizeof(vec3));
    memcpy(meta.aabbMax, mesh.aabbMax, sizeof(vec3));
    meta.vertexCount = final_vertex_count;
    meta.indexCount = final_index_count;
    meta.morphCount = final_morph_count;
    meta.vertexOffset = omdl_vertex_count;
    meta.indexOffset = omdl_index_count;
    meta.morphOffset = omdl_morph_count;
    meta.meshletCount = (uint32_t)final_meshlet_count;
    meta.meshletOffset = omdl_meshlet_count;
    meta.meshletVertexOffset = omdl_meshlet_vertex_count;
    meta.meshletTriangleOffset = omdl_meshlet_triangle_count;

    memcpy(meta.baseColorFactor, mesh.baseColorFactor, sizeof(vec4));
    meta.metallicFactor = mesh.metallicFactor;
    meta.roughnessFactor = mesh.roughnessFactor;
    meta.emissiveStrength = mesh.emissiveStrength;
    meta.aoStrength = mesh.aoStrength;
    memcpy(meta.emissiveFactor, mesh.emissiveFactor, sizeof(vec3));
    meta.alpha_mode = mesh.alpha_mode;
    meta.alpha_cutoff = mesh.alpha_cutoff;
    meta.is_unlit = mesh.is_unlit;
    meta.visible = mesh.visible;
    meta.transmissionFactor = mesh.transmissionFactor;
    meta.ior = mesh.ior;
    meta.thicknessFactor = mesh.thicknessFactor;
    memcpy(meta.attenuationColor, mesh.attenuationColor, sizeof(vec3));
    meta.attenuationDistance = mesh.attenuationDistance;
    meta.dispersion = mesh.dispersion;

    // Safely extract paths
#define EXTRACT_TEX(field, ptr) if(ptr) { size_t t = ptr - data->textures; if(t < data->textures_count) strcpy(meta.field, gltf_texture_paths[t]); }
    if (prim->material && prim->material->has_pbr_metallic_roughness) {
        cgltf_pbr_metallic_roughness* pbr = &prim->material->pbr_metallic_roughness;
        EXTRACT_TEX(tex_albedo, pbr->base_color_texture.texture);
        EXTRACT_TEX(tex_metallicRoughness, pbr->metallic_roughness_texture.texture);
        if (meta.tex_albedo[0]) {
            size_t t = pbr->base_color_texture.texture - data->textures;
            mesh.textureIndex = gltf_texture_indices[t];
            if (mesh.textureIndex >= 0) mesh.texture = texture_pool_get(mesh.textureIndex);
        }
    }
    if (prim->material) {
        EXTRACT_TEX(tex_normal, prim->material->normal_texture.texture);
        EXTRACT_TEX(tex_occlusion, prim->material->occlusion_texture.texture);
        EXTRACT_TEX(tex_emissive, prim->material->emissive_texture.texture);
        if (prim->material->has_transmission) EXTRACT_TEX(tex_transmission, prim->material->transmission.transmission_texture.texture);
        if (prim->material->has_volume) EXTRACT_TEX(tex_thickness, prim->material->volume.thickness_texture.texture);
    }
#undef EXTRACT_TEX

    omdl_metas = realloc(omdl_metas, (omdl_mesh_count + 1) * sizeof(OmdlMeshMeta));
    omdl_metas[omdl_mesh_count++] = meta;

    if (final_vertex_count > 0) {
        omdl_vertices = realloc(omdl_vertices, (omdl_vertex_count + final_vertex_count) * sizeof(Vertex));
        memcpy(&omdl_vertices[omdl_vertex_count], final_vertices, final_vertex_count * sizeof(Vertex));
        omdl_vertex_count += final_vertex_count;
    }
    if (final_index_count > 0) {
        omdl_indices = realloc(omdl_indices, (omdl_index_count + final_index_count) * sizeof(uint32_t));
        memcpy(&omdl_indices[omdl_index_count], final_indices, final_index_count * sizeof(uint32_t));
        omdl_index_count += final_index_count;
    }
    if (final_morph_count > 0) {
        size_t total_deltas = final_vertex_count * final_morph_count;
        omdl_morphs = realloc(omdl_morphs, (omdl_morph_count + total_deltas) * sizeof(MorphDelta));
        memcpy(&omdl_morphs[omdl_morph_count], final_morphs, total_deltas * sizeof(MorphDelta));
        omdl_morph_count += total_deltas;

        mesh.morph_data = malloc(sizeof(MorphData));
        mesh.morph_data->target_count = final_morph_count;
        mesh.morph_data->weights = calloc(final_morph_count, sizeof(float));
    }

    if (final_meshlet_count > 0) {
        omdl_meshlets = realloc(omdl_meshlets, (omdl_meshlet_count + final_meshlet_count) * sizeof(Meshlet));
        memcpy(&omdl_meshlets[omdl_meshlet_count], final_meshlets, final_meshlet_count * sizeof(Meshlet));
        omdl_meshlet_bounds = realloc(omdl_meshlet_bounds, (omdl_meshlet_count + final_meshlet_count) * sizeof(MeshletBounds));
        memcpy(&omdl_meshlet_bounds[omdl_meshlet_count], final_bounds, final_meshlet_count * sizeof(MeshletBounds));
        omdl_meshlet_skins = realloc(omdl_meshlet_skins, (omdl_meshlet_count + final_meshlet_count) * sizeof(MeshletSkinData));
        memcpy(&omdl_meshlet_skins[omdl_meshlet_count], final_skins, final_meshlet_count * sizeof(MeshletSkinData));
        omdl_meshlet_count += final_meshlet_count;

        omdl_meshlet_vertices = realloc(omdl_meshlet_vertices, (omdl_meshlet_vertex_count + final_ml_vertex_count) * sizeof(uint32_t));
        memcpy(&omdl_meshlet_vertices[omdl_meshlet_vertex_count], final_meshlet_vertices, final_ml_vertex_count * sizeof(uint32_t));
        omdl_meshlet_vertex_count += final_ml_vertex_count;

        omdl_meshlet_triangles = realloc(omdl_meshlet_triangles, (omdl_meshlet_triangle_count + final_ml_triangle_count) * sizeof(uint8_t));
        memcpy(&omdl_meshlet_triangles[omdl_meshlet_triangle_count], final_meshlet_triangles, final_ml_triangle_count * sizeof(uint8_t));
        omdl_meshlet_triangle_count += final_ml_triangle_count;
    }

    if (final_vertices) free(final_vertices);
    if (final_indices) free(final_indices);
    if (final_morphs) free(final_morphs);
    if (final_meshlets) free(final_meshlets);
    if (final_bounds) free(final_bounds);
    if (final_skins) free(final_skins);
    if (final_meshlet_vertices) free(final_meshlet_vertices);
    if (final_meshlet_triangles) free(final_meshlet_triangles);

    mesh.vertexCount = final_vertex_count;
    mesh.indexCount  = final_index_count;
    mesh.meshletCount = final_meshlet_count;
    mesh.megaBaseVertex = UINT32_MAX;
    mesh.megaBaseIndex = UINT32_MAX;
    mesh.dynamicBaseVertex = UINT32_MAX;

    return mesh;
}


/// process_node — implementation group
/// Cached texture resolution — only after GPU completion
static bool cached_mesh_resolve_textures(Mesh *mesh, size_t mesh_index) {
    int32_t ids[OBS_CACHE_TEXTURE_CHANNELS];
    if (!obs_cache_textures_indices(&s_cache_texture_plan, mesh_index, ids))
        return false;
    mesh->textureIndex       = ids[OBS_CACHE_ALBEDO];
    mesh->normalMapIndex     = ids[OBS_CACHE_NORMAL];
    mesh->metallicRoughIndex = ids[OBS_CACHE_METALLIC_ROUGHNESS];
    mesh->aoIndex            = ids[OBS_CACHE_OCCLUSION];
    mesh->emissiveIndex      = ids[OBS_CACHE_EMISSIVE];
    mesh->transmissionIndex  = ids[OBS_CACHE_TRANSMISSION];
    mesh->thicknessIndex     = ids[OBS_CACHE_THICKNESS];
    if (mesh->textureIndex >= 0)
        mesh->texture = texture_pool_get(mesh->textureIndex);
    return true;
}

static void process_node(cgltf_node* node, cgltf_data* data, Meshes* meshes, mat4 parent_transform) {
    mat4 local_transform;
    mat4 world_transform;

    cgltf_node_transform_local(node, (float*)local_transform);
    glm_mat4_mul(parent_transform, local_transform, world_transform);

    if (s_staged_mesh_failed) return;
    size_t mesh_start = s_staged_mesh_count;

    if (node->mesh) {
        cgltf_mesh* gltf_mesh = node->mesh;

        for (size_t i = 0; i < gltf_mesh->primitives_count; i++) {
            char mesh_name[256];
            snprintf(mesh_name, sizeof(mesh_name), "%s_prim_%zu",
                     node->name ? node->name : "node", i);

            Mesh mesh = create_mesh_from_primitive(&gltf_mesh->primitives[i], data, mesh_name);

            if (mesh.vertexCount > 0 && !s_staged_mesh_failed) {
                mesh.node = node;
                glm_mat4_copy(world_transform, mesh.model);
                glm_mat4_copy(local_transform, mesh.local_transform);

                if (mesh.morph_data && gltf_mesh->weights_count > 0) {
                    for (size_t w = 0; w < mesh.morph_data->target_count && w < gltf_mesh->weights_count; w++) {
                        mesh.morph_data->weights[w] = gltf_mesh->weights[w];
                    }
                }

                // Both cache hits and misses stage privately. No meshes become
                // visible until all GPU geometry is confirmed and storage is ready.
                if (!staged_mesh_push(mesh)) return;
            } else {
                // Unused/failed primitive retains no staged allocations.
                mesh_destroy(VK_NULL_HANDLE, &mesh);
                if (s_staged_mesh_failed) return;
            }
        }
    }

    size_t mesh_end = s_staged_mesh_count;
    size_t mesh_count = mesh_end - mesh_start;
    if (mesh_count > 0 && node_mapping_count < 256) {
        node_mappings[node_mapping_count].node = node;
        node_mappings[node_mapping_count].mesh_start_index = mesh_start;
        node_mappings[node_mapping_count].mesh_count = mesh_count;
        node_mapping_count++;
    }

    for (size_t i = 0; i < node->children_count; i++) {
        process_node(node->children[i], data, meshes, world_transform);
    }
}

void load_gltf_meshes(cgltf_data* data, Meshes* meshes) {
    if (!data || !meshes) {
        fprintf(stderr, "Invalid parameters to load_gltf_meshes\n");
        return;
    }

    if (data->scenes_count == 0) {
        fprintf(stderr, "No scenes in glTF file\n");
        return;
    }

    node_mapping_count = 0;
    cgltf_scene* scene_root = data->scene ? data->scene : &data->scenes[0];

    printf("Processing scene with %zu root nodes\n", scene_root->nodes_count);

    mat4 identity;
    glm_mat4_identity(identity);

    for (size_t i = 0; i < scene_root->nodes_count; i++) {
        process_node(scene_root->nodes[i], data, meshes, identity);
    }

    printf("Prepared %zu private glTF meshes\n", s_staged_mesh_count);
}

/// Cache-hit candidate preparation — no live meshes or texture-pool writes
static bool prepare_cache_hit_scene(cgltf_data *data, Scene *target,
                                    const OmdlHeader *header) {
    if (!data || !target || !header || !omdl_cache_osg) return false;
    staged_mesh_discard();
    omdl_cache_idx = 0;
    omdl_cache_expected_meshes = header->mesh_count;
    if (!staged_mesh_reserve(header->mesh_count))
        return false;
    load_gltf_meshes(data, &target->meshes);
    if (s_staged_mesh_failed || s_staged_mesh_count != header->mesh_count ||
        omdl_cache_idx != header->mesh_count) return false;
    /* Validate, copy and intern every cached texture path before GPU work.
     * This phase performs NO texture-pool uploads or descriptor mutations. */
    if (!obs_cache_textures_prepare(&s_cache_texture_plan,
             s_staged_mesh_count, omdl_texture_path, omdl_cache_metas)) return false;
    if (s_staged_mesh_count > SIZE_MAX - target->meshes.count ||
        target->meshes.count + s_staged_mesh_count > INT32_MAX ||
        target->tree.count < 0 ||
        s_staged_mesh_count > (SCENE_TREE_MAX_NODES - 1) / 2 ||
        (size_t)target->tree.count > SCENE_TREE_MAX_NODES - 1 - 2 * s_staged_mesh_count)
        return false;
    /* Storage is allocated before GPU work; the final append cannot fail. */
    if (!meshes_reserve(&target->meshes, target->meshes.count + s_staged_mesh_count))
        return false;
    if (target->gltf_instance_count == SIZE_MAX) return false;
    if (target->gltf_instance_count == target->gltf_instance_capacity) {
        size_t old = target->gltf_instance_capacity;
        size_t capacity = old ? old * 2 : 4;
        if (capacity < old || capacity > SIZE_MAX / sizeof(GLTFInstance)) return false;
        GLTFInstance *p = realloc(target->gltf_instances, capacity * sizeof(GLTFInstance));
        if (!p) return false;
        target->gltf_instances = p;
        target->gltf_instance_capacity = capacity;
    }
    /* Prepare frame-animation scratch and validate the hierarchy before upload. */
    OmdlSceneGraph *g = omdl_cache_osg;
    size_t nodes = g->node_count;
    if (nodes) {
        if (nodes > SIZE_MAX / sizeof(mat4) ||
            nodes > SIZE_MAX / sizeof(vec3) ||
            nodes > SIZE_MAX / (sizeof(uint32_t) + sizeof(uint8_t))) return false;
        if (!g->local_transforms) g->local_transforms = calloc(nodes, sizeof(mat4));
        if (!g->world_transforms) g->world_transforms = calloc(nodes, sizeof(mat4));
        if (!g->script_translations) g->script_translations = calloc(nodes, sizeof(vec3));
        if (!g->topology_order) g->topology_order = malloc(nodes * sizeof(uint32_t));
        unsigned char *scratch = malloc(nodes * (sizeof(uint32_t) + sizeof(uint8_t)));
        bool ordered = scratch && g->nodes && g->local_transforms &&
            g->world_transforms && g->script_translations && g->topology_order &&
            obs_node_topological_order(g->nodes, nodes, sizeof(OmdlNode),
                offsetof(OmdlNode, parent), g->topology_order,
                (uint32_t *)scratch, scratch + nodes * sizeof(uint32_t));
        free(scratch);
        if (!ordered) return false;
    }
    return true;
}

bool load_gltf_animations(cgltf_data* data, GLTFInstance* instance) {
    if (data->animations_count == 0) {
        printf("No animations in glTF file\n");
        instance->animations = NULL;
        instance->animation_count = 0;
        return true;
    }

    printf("Loading %zu animations from glTF...\n", data->animations_count);

    Animation* animations = malloc(data->animations_count * sizeof(Animation));

    for (size_t i = 0; i < data->animations_count; i++) {
        cgltf_animation* anim = &data->animations[i];

        animations[i].name = anim->name ? strdup(anim->name) : strdup("unnamed");
        animations[i].duration = 0.0f;
        animations[i].channel_count = anim->channels_count;
        animations[i].channels = malloc(anim->channels_count * sizeof(AnimationChannel));

        printf("  Animation %zu: '%s' with %zu channels\n",
               i, animations[i].name, anim->channels_count);

        for (size_t c = 0; c < anim->channels_count; c++) {
            cgltf_animation_channel* gltf_channel = &anim->channels[c];
            cgltf_animation_sampler* sampler = gltf_channel->sampler;
            AnimationChannel* channel = &animations[i].channels[c];

            channel->target_node = gltf_channel->target_node;
            channel->path = gltf_channel->target_path;

            cgltf_accessor* input = sampler->input;
            cgltf_accessor* output = sampler->output;

            channel->keyframe_count = input->count;
            channel->times = malloc(input->count * sizeof(float));
            channel->translations = NULL;
            channel->rotations = NULL;
            channel->scales = NULL;
            channel->weights = NULL;

            for (size_t k = 0; k < input->count; k++) {
                cgltf_accessor_read_float(input, k, &channel->times[k], 1);
                if (channel->times[k] > animations[i].duration) {
                    animations[i].duration = channel->times[k];
                }
            }

            if (gltf_channel->target_path == cgltf_animation_path_type_translation) {
                channel->translations = malloc(output->count * sizeof(vec3));
                for (size_t k = 0; k < output->count; k++) {
                    cgltf_accessor_read_float(output, k, channel->translations[k], 3);
                }
            } else if (gltf_channel->target_path == cgltf_animation_path_type_rotation) {
                channel->rotations = malloc(output->count * sizeof(versor));
                for (size_t k = 0; k < output->count; k++) {
                    cgltf_accessor_read_float(output, k, channel->rotations[k], 4);
                }
            } else if (gltf_channel->target_path == cgltf_animation_path_type_scale) {
                channel->scales = malloc(output->count * sizeof(vec3));
                for (size_t k = 0; k < output->count; k++) {
                    cgltf_accessor_read_float(output, k, channel->scales[k], 3);
                }
            } else if (gltf_channel->target_path == cgltf_animation_path_type_weights) {
                // Morph target weights animation
                size_t num_weights = output->count / input->count;
                channel->weights = malloc(output->count * sizeof(float));
                for (size_t k = 0; k < output->count; k++) {
                    cgltf_accessor_read_float(output, k, &channel->weights[k], 1);
                }
                printf("    Channel %zu: Morph weights (%zu targets)\n", c, num_weights);
            }
        }

        printf("    Duration: %.2f seconds\n", animations[i].duration);
    }

    instance->animations = animations;
    instance->animation_count = data->animations_count;
    return true;
}

/// Scene Tree

int32_t scene_tree_add_node(SceneTree* tree, const char* name, int32_t parent_idx, int32_t mesh_index) {
    if (tree->count >= SCENE_TREE_MAX_NODES) return -1;

    int32_t idx = tree->count++;
    scene_tree_bump_revision(tree);
    SceneNode* node = &tree->nodes[idx];
    node->foreign_identity = 0;
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    node->parent       = parent_idx;
    node->first_child  = -1;
    node->next_sibling = -1;
    node->mesh_index   = mesh_index;
    node->sdf_index = -1;
    node->expanded     = false;
    node->visible      = true;
    node->selected_group = false;
    node->selected       = false;

    // Wire into parent's child linked list (append to front for O(1))
    if (parent_idx >= 0 && parent_idx < tree->count) {
        node->next_sibling = tree->nodes[parent_idx].first_child;
        tree->nodes[parent_idx].first_child = idx;
    }

    return idx;
}

void scene_tree_register_gltf(Scene* s, GLTFInstance* inst, const char* filepath) {
    // Derive a clean display name from the filepath (e.g. "sponza" from "./assets/sponza/sponza.gltf")
    const char* slash = strrchr(filepath, '/');
    const char* base  = slash ? slash + 1 : filepath;
    char display[128];
    strncpy(display, base, sizeof(display) - 1);
    display[sizeof(display) - 1] = '\0';
    // Strip extension
    char* dot = strrchr(display, '.');
    if (dot) *dot = '\0';

    // Create one group node parented to the virtual root (index 0)
    int32_t group = scene_tree_add_node(&s->tree, display, 0, -1);

// Create one leaf per mesh
    for (size_t i = 0; i < inst->mesh_count; i++) {
        Mesh* m = &s->meshes.items[inst->mesh_start_index + i];
        const char* mesh_name = m->name ? m->name : "(unnamed)";
        int32_t mesh_node = scene_tree_add_node(&s->tree, mesh_name, group, (int32_t)(inst->mesh_start_index + i));

        // Append Material as a child of the mesh
        scene_tree_add_node(&s->tree, "Material", mesh_node, -1);
    }
}

bool load_gltf(const char* filepath, Scene* scene) {
    if (!filepath || !scene) return false;
    // Explicitly discard any abandoned staged state from an earlier failure.
    staged_mesh_discard();
    // Guard: ensure GPU is idle before any staging upload commands are submitted.
    // This function is always called from the deferred queue in drawFrame() after
    // waiting on all in-flight fences, so vkDeviceWaitIdle is a no-op in normal operation.
    // It acts as a safety net if called directly (e.g. from main() at startup).
    vkDeviceWaitIdle(context.device);

    /* Cache paths are optional. Unrepresentable paths must never prevent
     * loading the original source model. */
    char omdl_path[512] = {0};
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dir_path[512];
    bool cache_enabled = strlen(filepath) < 256;
    const char *bench_cache_mode = getenv("OBSIDIAN_BENCH_SCENE_CACHE");
    if (bench_cache_mode && strcmp(bench_cache_mode, "off") == 0)
        cache_enabled = false;
    if (snprintf(dir_path, sizeof(dir_path), "%s/.cache", home) >= (int)sizeof(dir_path)) cache_enabled=false;
    if (cache_enabled) mkdir(dir_path, 0777);
    if (snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home) >= (int)sizeof(dir_path)) cache_enabled=false;
    if (cache_enabled) mkdir(dir_path, 0777);
    if (snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/models", home) >= (int)sizeof(dir_path)) cache_enabled=false;
    if (cache_enabled) mkdir(dir_path, 0777);
    if (cache_enabled) {
        char safe_name[256];
        memcpy(safe_name, filepath, strlen(filepath) + 1);
        for (size_t i = 0; safe_name[i]; ++i)
            if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.')
                safe_name[i] = '_';
        if (snprintf(omdl_path, sizeof(omdl_path), "%s/%s.omdl", dir_path, safe_name)
            >= (int)sizeof(omdl_path)) cache_enabled = false;
    }


    /* The source is parsed before validating a cache hit so that current
     * decoded buffer content participates in the cache identity. */
    OmdlLayout cache_layout = {0};
    is_omdl_cache_hit = false;
    FILE *fomdl = NULL;
    uint64_t source_digest = 0;
    uint64_t abi_tag = omdl_native_abi_tag();
    cgltf_options options = {0};
    cgltf_data* data = NULL;
    if (cgltf_parse_file(&options, filepath, &data) != cgltf_result_success) {
        if (fomdl) fclose(fomdl);
        free(omdl_cache_metas); omdl_cache_metas = NULL;
        omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
        is_omdl_cache_hit = false;
        return false;
    }

    // Must load buffers even on cache hit until animations/skins are fully serialized into OMDL
    if (cgltf_load_buffers(&options, data, filepath) != cgltf_result_success) {
        cgltf_free(data);
        if (fomdl) fclose(fomdl);
        free(omdl_cache_metas); omdl_cache_metas = NULL;
        omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
        is_omdl_cache_hit = false;
        return false;
    }

    /* Old ABI-native cache files cannot accidentally masquerade as v2:
     * they fail the portable envelope check and are recooked automatically. */
    if (!omdl_source_digest(data, &source_digest))
        cache_enabled = false;
    fomdl = cache_enabled ? fopen(omdl_path, "rb") : NULL;
    if (fomdl) {
        if (omdl_cache_preflight(fomdl, &cache_layout,
                                 source_digest, abi_tag,
                                 &omdl_cache_metas, &omdl_cache_osg)) {
            is_omdl_cache_hit = true;
        } else {
            fprintf(stderr, "[OMDL] invalid, stale or incompatible cache; rebuilding: %s\n", omdl_path);
            fclose(fomdl);
            fomdl = NULL;
        }
    }

    if (is_omdl_cache_hit) {
        uint32_t source_mesh_count = 0;
        const OmdlHeader *h = &cache_layout.header;
        if (!omdl_source_mesh_count(data, &source_mesh_count) ||
            source_mesh_count != h->mesh_count ||
            data->nodes_count != h->node_count ||
            data->skins_count != h->skin_count ||
            data->animations_count != h->anim_count) {
            fprintf(stderr, "[OMDL] cache/source structure changed; rebuilding\n");
            fclose(fomdl); fomdl=NULL;
            free(omdl_cache_metas); omdl_cache_metas=NULL;
            omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg=NULL;
            is_omdl_cache_hit=false;
        }
    }

    if (is_omdl_cache_hit &&
        !prepare_cache_hit_scene(data, scene, &cache_layout.header)) {
        fprintf(stderr, "[OMDL] cache scene preparation failed; live meshes untouched\n");
        staged_mesh_discard();
        fclose(fomdl);
        free(omdl_cache_metas); omdl_cache_metas = NULL;
        omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
        cgltf_free(data);
        is_omdl_cache_hit = false;
        return false;
    }

    if (is_omdl_cache_hit) {
        /* All CPU-side cache sections and graph references were validated
         * before reaching a GPU upload. Only the immutable geometry sections
         * are streamed below; the graph is already fully owned by the host. */
        const OmdlHeader *reservation_header = &cache_layout.header;
        const MegaGeometryCounts needed = {
            .vertices = reservation_header->total_vertices,
            .indices = reservation_header->total_indices,
            .morphs = reservation_header->total_morph_deltas,
            .meshlets = reservation_header->total_meshlets,
            .meshlet_vertices = reservation_header->total_meshlet_vertices,
            .meshlet_triangles = reservation_header->total_meshlet_triangles
        };
        if (!omdl_cache_seek(fomdl, &cache_layout, OMDL_VERTICES)) {
            staged_mesh_discard();
            cgltf_free(data);
            fclose(fomdl);
            free(omdl_cache_metas); omdl_cache_metas = NULL;
            omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
            is_omdl_cache_hit = false;
            return false;
        }
        MegaGeometryBases uploaded = {0};
        if (!megaGeometryUploadCacheBatch(&context, fomdl, &needed, &uploaded)) {
            fprintf(stderr, "[OMDL] prepared GPU cache transfer failed; scene remains unpublished\n");
            staged_mesh_discard();
            fclose(fomdl);
            free(omdl_cache_metas); omdl_cache_metas = NULL;
            omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
            is_omdl_cache_hit = false;
            cgltf_free(data);
            return false;
        }
        omdl_base_v = uploaded.vertices;
        omdl_base_i = uploaded.indices;
        omdl_base_m = uploaded.morphs;
        omdl_base_ml = uploaded.meshlets;
        omdl_base_ml_v = uploaded.meshlet_vertices;
        omdl_base_ml_t = uploaded.meshlet_triangles;
        fprintf(stdout, "[OMDL] validated cache hit: %s\n", omdl_path);
        fclose(fomdl);
        fomdl = NULL;
    }

    if (!is_omdl_cache_hit) {
        fprintf(stdout, "\033[33m[OMDL] Cache Miss: Compiling GLTF to monolithic binary...\033[0m\n");

        omdl_vertex_count = 0; omdl_index_count = 0; omdl_morph_count = 0; omdl_mesh_count = 0;
        omdl_meshlet_count = 0; omdl_meshlet_vertex_count = 0; omdl_meshlet_triangle_count = 0;
        omdl_vertices = NULL; omdl_indices = NULL; omdl_morphs = NULL; omdl_metas = NULL;
        omdl_meshlets = NULL; omdl_meshlet_bounds = NULL; omdl_meshlet_skins = NULL;
        omdl_meshlet_vertices = NULL; omdl_meshlet_triangles = NULL;

        load_gltf_textures(data, filepath);
    }

    // Create new glTF instance
    if (scene->gltf_instance_count == scene->gltf_instance_capacity) {
        size_t new_cap = scene->gltf_instance_capacity ? scene->gltf_instance_capacity * 2 : 4;
        scene->gltf_instances = realloc(scene->gltf_instances, new_cap * sizeof(GLTFInstance));
        scene->gltf_instance_capacity = new_cap;
    }

    GLTFInstance* instance = &scene->gltf_instances[scene->gltf_instance_count];
    instance->mesh_start_index = scene->meshes.count;
    glm_mat4_identity(instance->instance_transform);
    instance->gltf_data = (OmdlSceneGraph *)data;
    instance->source_path = NULL;
    instance->animations = NULL;
    instance->animation_count = 0;
    instance->mesh_count = 0;
    instance->texture_count = data->textures_count;
    instance->texture_lease = (ObsCacheTextureLease){0};

    if (!is_omdl_cache_hit) load_gltf_meshes(data, &scene->meshes);
    else {
        // Resolve texture references and publish the prepared mesh batch only
        // after confirmed geometry completion and preallocated scene storage.
        /* Texture registration starts only after checked geometry completion.
         * Each interned path acquires one lease-owned renderer reference. */
        ObsCacheTextureLease staged_textures = {0};
        if (!obs_cache_textures_stage_retained(&s_cache_texture_plan,
                                               texture_pool_lease_acquire,
                                               texture_pool_lease_release,
                                               &context, &staged_textures)) {
            obs_cache_texture_lease_release(&staged_textures);
            staged_mesh_discard();
            cgltf_free(data);
            free(omdl_cache_metas); omdl_cache_metas = NULL;
            omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
            return false;
        }
        for (size_t i = 0; i < s_staged_mesh_count; ++i) {
            Mesh *m = &s_staged_meshes[i];
            const OmdlMeshMeta *meta = &omdl_cache_metas[i];
            m->megaBaseVertex = omdl_base_v != UINT32_MAX
                ? omdl_base_v + meta->vertexOffset : UINT32_MAX;
            m->megaBaseIndex = omdl_base_i != UINT32_MAX && meta->indexCount
                ? omdl_base_i + meta->indexOffset : UINT32_MAX;
            m->megaBaseMeshlet = omdl_base_ml != UINT32_MAX
                ? omdl_base_ml + meta->meshletOffset : UINT32_MAX;
            m->megaBaseMeshletVertex = omdl_base_ml_v != UINT32_MAX
                ? omdl_base_ml_v + meta->meshletVertexOffset : UINT32_MAX;
            m->megaBaseMeshletTriangle = omdl_base_ml_t != UINT32_MAX
                ? omdl_base_ml_t + meta->meshletTriangleOffset : UINT32_MAX;
            m->morphDeltaOffset = meta->morphCount && omdl_base_m != UINT32_MAX
                ? omdl_base_m + meta->morphOffset : UINT32_MAX;
            if (!cached_mesh_resolve_textures(m, i)) {
                obs_cache_texture_lease_release(&staged_textures);
                staged_mesh_discard();
                cgltf_free(data);
                free(omdl_cache_metas); omdl_cache_metas = NULL;
                omdl_scene_graph_destroy(omdl_cache_osg); omdl_cache_osg = NULL;
                return false;
            }
        }
        /* Transfer before mesh publication. This move is allocation-free. */
        if (!obs_cache_texture_lease_move(&instance->texture_lease,
                                          &staged_textures)) {
            obs_cache_texture_lease_release(&staged_textures);
            staged_mesh_discard();
            cgltf_free(data);
            return false;
        }
        if (!meshes_append_prepared(&scene->meshes, s_staged_meshes,
                                    s_staged_mesh_count)) {
            fprintf(stderr, "[OMDL] impossible pre-reserved mesh commit failed\n");
            obs_cache_texture_lease_release(&instance->texture_lease);
            staged_mesh_discard();
            cgltf_free(data);
            return false;
        }
        instance->mesh_count = s_staged_mesh_count;
        staged_mesh_reset(); // ownership moved to Scene
    }

    if (!is_omdl_cache_hit) {
        const MegaGeometryCounts needed = {
            .vertices = omdl_vertex_count, .indices = omdl_index_count,
            .morphs = omdl_morph_count, .meshlets = omdl_meshlet_count,
            .meshlet_vertices = omdl_meshlet_vertex_count,
            .meshlet_triangles = omdl_meshlet_triangle_count
        };
        if (!megaGeometryPreflight(&context, &needed)) {
            fprintf(stderr, "[OMDL] insufficient geometry capacity; cook upload cancelled before GPU work\n");
            omdl_discard_cook_arrays();
            cgltf_free(data);
            return false;
        }
        // Phase 1: prepare every cooked stream before submitting any GPU copy.
        // Cache hits and misses now share the same checked, fence-retired path.
        const MegaCookedGeometry cooked = {
            .vertices = omdl_vertices, .indices = omdl_indices,
            .morphs = omdl_morphs, .meshlets = omdl_meshlets,
            .bounds = omdl_meshlet_bounds, .skins = omdl_meshlet_skins,
            .meshlet_vertices = omdl_meshlet_vertices,
            .meshlet_triangles = omdl_meshlet_triangles,
        };
        MegaGeometryBases uploaded = {0};
        if (!megaGeometryUploadCookedBatch(&context, &needed, &cooked, &uploaded)) {
            fprintf(stderr, "[OMDL] cooked geometry upload rejected; live geometry offsets unchanged\n");
            omdl_discard_cook_arrays();
            cgltf_free(data);
            return false;
        }
        const uint32_t base_v = uploaded.vertices;
        const uint32_t base_i = uploaded.indices;
        const uint32_t base_m = uploaded.morphs;
        const uint32_t base_ml = uploaded.meshlets;
        const uint32_t base_ml_v = uploaded.meshlet_vertices;
        const uint32_t base_ml_t = uploaded.meshlet_triangles;

        // Phase 2: Patch GPU addresses into staged meshes before they enter the live scene.
        // This is the critical invariant: megaBaseVertex is NEVER UINT32_MAX in the live scene.
        for (size_t i = 0; i < s_staged_mesh_count; i++) {
            Mesh* m = &s_staged_meshes[i];
            m->megaBaseVertex   = (base_v  != UINT32_MAX) ? base_v  + omdl_metas[i].vertexOffset          : UINT32_MAX;
            if (m->indexCount > 0)  m->megaBaseIndex      = (base_i  != UINT32_MAX) ? base_i  + omdl_metas[i].indexOffset           : UINT32_MAX;
            if (m->morphCount > 0)  m->morphDeltaOffset   = (base_m  != UINT32_MAX) ? base_m  + omdl_metas[i].morphOffset            : UINT32_MAX;
            m->megaBaseMeshlet         = (base_ml   != UINT32_MAX) ? base_ml   + omdl_metas[i].meshletOffset         : UINT32_MAX;
            m->megaBaseMeshletVertex   = (base_ml_v != UINT32_MAX) ? base_ml_v + omdl_metas[i].meshletVertexOffset   : UINT32_MAX;
            m->megaBaseMeshletTriangle = (base_ml_t != UINT32_MAX) ? base_ml_t + omdl_metas[i].meshletTriangleOffset : UINT32_MAX;
        }
        // Phase 3: One allocation-free append after reserve. Cache misses
        // still have other legacy side effects and aren't fully transactional.
        if (!meshes_reserve(&scene->meshes, scene->meshes.count + s_staged_mesh_count) ||
            !meshes_append_prepared(&scene->meshes, s_staged_meshes, s_staged_mesh_count)) {
            staged_mesh_discard();
            cgltf_free(data);
            return false;
        }
        instance->mesh_count = s_staged_mesh_count;
        staged_mesh_reset();

        // 3. Extract Scene Graph (Nodes, Skins, Animations)
        OmdlSceneGraph* osg = calloc(1, sizeof(OmdlSceneGraph));
        osg->node_count = data->nodes_count;
        osg->nodes = calloc(data->nodes_count, sizeof(OmdlNode));
        for (size_t i = 0; i < data->nodes_count; i++) {
            cgltf_node* n = &data->nodes[i];
            strncpy(osg->nodes[i].name, n->name ? n->name : "Bone", 63);
            osg->nodes[i].parent = n->parent ? (int32_t)(n->parent - data->nodes) : -1;
            osg->nodes[i].mesh_idx = -1;
            osg->nodes[i].skin_idx = n->skin ? (int32_t)(n->skin - data->skins) : -1;
            osg->nodes[i].expanded = false;

            // Extract valid base matrix for hierarchy resolution
            cgltf_node_transform_local(n, (float*)osg->nodes[i].base_matrix);

            if (n->has_translation) { memcpy(osg->nodes[i].translation, n->translation, sizeof(vec3)); }
            else { memset(osg->nodes[i].translation, 0, sizeof(vec3)); }

            if (n->has_rotation) { memcpy(osg->nodes[i].rotation, n->rotation, sizeof(versor)); }
            else { osg->nodes[i].rotation[0]=0; osg->nodes[i].rotation[1]=0; osg->nodes[i].rotation[2]=0; osg->nodes[i].rotation[3]=1.0f; }

            if (n->has_scale) { memcpy(osg->nodes[i].scale, n->scale, sizeof(vec3)); }
            else { osg->nodes[i].scale[0]=1.0f; osg->nodes[i].scale[1]=1.0f; osg->nodes[i].scale[2]=1.0f; }
        }

        for (size_t i = 0; i < omdl_mesh_count; i++) {
            Mesh* m = &scene->meshes.items[instance->mesh_start_index + i];
            if (m->node) {
                size_t node_idx = (cgltf_node*)m->node - data->nodes;
                osg->nodes[node_idx].mesh_idx = (int32_t)i;
                m->node = (void*)(uintptr_t)node_idx;
            }
        }

        osg->skin_count = data->skins_count;
        osg->skins = calloc(data->skins_count, sizeof(OmdlSkin));
        uint32_t total_joints = 0;
        for (size_t i = 0; i < data->skins_count; i++) total_joints += data->skins[i].joints_count;
        osg->skin_joints = calloc(total_joints, sizeof(uint32_t));
        osg->skin_ibms = calloc(total_joints, sizeof(mat4));

        uint32_t j_offset = 0;
        for (size_t i = 0; i < data->skins_count; i++) {
            cgltf_skin* s = &data->skins[i];
            osg->skins[i].joints_count = s->joints_count;
            osg->skins[i].joints_offset = j_offset;
            osg->skins[i].ibm_offset = j_offset;
            for (size_t j = 0; j < s->joints_count; j++) {
                osg->skin_joints[j_offset + j] = s->joints[j] - data->nodes;
                if (s->inverse_bind_matrices) {
                    cgltf_accessor_read_float(s->inverse_bind_matrices, j, (float*)osg->skin_ibms[j_offset + j], 16);
                } else {
                    mat4 ident = GLM_MAT4_IDENTITY_INIT;
                    memcpy(osg->skin_ibms[j_offset + j], ident, sizeof(mat4));
                }
            }
            j_offset += s->joints_count;
        }

        osg->anim_count = data->animations_count;
        osg->anims = calloc(data->animations_count, sizeof(OmdlAnimation));
        uint32_t total_channels = 0;
        uint32_t total_floats = 0;
        for (size_t i = 0; i < data->animations_count; i++) {
            total_channels += data->animations[i].channels_count;
            for (size_t c = 0; c < data->animations[i].channels_count; c++) {
                cgltf_animation_channel* chan = &data->animations[i].channels[c];
                total_floats += chan->sampler->input->count;
                uint32_t out_count = chan->sampler->output->count;
                if (chan->target_path == cgltf_animation_path_type_weights) total_floats += out_count;
                else total_floats += out_count * ((chan->target_path == cgltf_animation_path_type_rotation) ? 4 : 3);
            }
        }
        osg->channels = calloc(total_channels, sizeof(OmdlChannel));
        osg->anim_floats = calloc(total_floats, sizeof(float));

        uint32_t c_offset = 0;
        uint32_t f_offset = 0;
        for (size_t i = 0; i < data->animations_count; i++) {
            cgltf_animation* a = &data->animations[i];
            strncpy(osg->anims[i].name, a->name ? a->name : "unnamed", 63);
            osg->anims[i].channel_count = a->channels_count;
            osg->anims[i].channel_offset = c_offset;
            float max_time = 0.0f;

            for (size_t c = 0; c < a->channels_count; c++) {
                cgltf_animation_channel* chan = &a->channels[c];
                OmdlChannel* ochan = &osg->channels[c_offset + c];
                ochan->target_node = chan->target_node ? (uint32_t)(chan->target_node - data->nodes) : UINT32_MAX;
                ochan->path_type = chan->target_path;
                ochan->keyframe_count = chan->sampler->input->count;

                ochan->times_offset = f_offset;

                for (size_t k = 0; k < ochan->keyframe_count; k++) {
                    cgltf_accessor_read_float(chan->sampler->input, k, &osg->anim_floats[f_offset++], 1);
                    if (osg->anim_floats[f_offset-1] > max_time) max_time = osg->anim_floats[f_offset-1];
                }

                ochan->values_offset = f_offset;
                uint32_t out_count = chan->sampler->output->count;
                if (chan->target_path == cgltf_animation_path_type_weights) {
                    for (size_t k = 0; k < out_count; k++) cgltf_accessor_read_float(chan->sampler->output, k, &osg->anim_floats[f_offset++], 1);
                } else {
                    uint32_t comps = (chan->target_path == cgltf_animation_path_type_rotation) ? 4 : 3;
                    for (size_t k = 0; k < out_count; k++) {
                        cgltf_accessor_read_float(chan->sampler->output, k, &osg->anim_floats[f_offset], comps);
                        f_offset += comps;
                    }
                }
            }
            osg->anims[i].duration = max_time;
            c_offset += a->channels_count;
        }

        // 4. Write the cache to an exclusive temporary file, then publish
        //    atomically. A failed/short write never corrupts the live cache.
        char pending_path[sizeof(omdl_path) + 16];
        int pending_len = snprintf(pending_path, sizeof(pending_path),
                                   "%s.XXXXXX", omdl_path);
        if (cache_enabled && pending_len > 0 && (size_t)pending_len < sizeof(pending_path)) {
            int fd = mkstemp(pending_path);
            if (fd >= 0) {
                FILE *fout = fdopen(fd, "w+b");
                if (!fout) { close(fd); unlink(pending_path); }
                else {
                    OmdlHeader header = { OMDL_MAGIC, omdl_mesh_count, omdl_vertex_count, omdl_index_count, omdl_morph_count,
                        omdl_meshlet_count, omdl_meshlet_vertex_count, omdl_meshlet_triangle_count,
                        osg->node_count, osg->skin_count, osg->anim_count, total_joints, total_channels, total_floats };
                    bool written = omdl_cache_v2_begin(fout) &&
                        omdl_cache_write(fout, &header, sizeof(header), 1) &&
                        omdl_cache_write(fout, omdl_metas, sizeof(OmdlMeshMeta), omdl_mesh_count) &&
                        omdl_cache_write(fout, omdl_vertices, sizeof(Vertex), omdl_vertex_count) &&
                        omdl_cache_write(fout, omdl_indices, sizeof(uint32_t), omdl_index_count) &&
                        omdl_cache_write(fout, omdl_morphs, sizeof(MorphDelta), omdl_morph_count) &&
                        omdl_cache_write(fout, omdl_meshlets, sizeof(Meshlet), omdl_meshlet_count) &&
                        omdl_cache_write(fout, omdl_meshlet_bounds, sizeof(MeshletBounds), omdl_meshlet_count) &&
                        omdl_cache_write(fout, omdl_meshlet_skins, sizeof(MeshletSkinData), omdl_meshlet_count) &&
                        omdl_cache_write(fout, omdl_meshlet_vertices, sizeof(uint32_t), omdl_meshlet_vertex_count) &&
                        omdl_cache_write(fout, omdl_meshlet_triangles, sizeof(uint8_t), omdl_meshlet_triangle_count) &&
                        omdl_cache_write(fout, osg->nodes, sizeof(OmdlNode), osg->node_count) &&
                        omdl_cache_write(fout, osg->skins, sizeof(OmdlSkin), osg->skin_count) &&
                        omdl_cache_write(fout, osg->skin_joints, sizeof(uint32_t), total_joints) &&
                        omdl_cache_write(fout, osg->skin_ibms, sizeof(mat4), total_joints) &&
                        omdl_cache_write(fout, osg->anims, sizeof(OmdlAnimation), osg->anim_count) &&
                        omdl_cache_write(fout, osg->channels, sizeof(OmdlChannel), total_channels) &&
                        omdl_cache_write(fout, osg->anim_floats, sizeof(float), total_floats);
                    if (written) written = omdl_cache_v2_finish(fout, source_digest, abi_tag);
                    if (written) written = fflush(fout) == 0 && fsync(fileno(fout)) == 0;
                    if (fclose(fout) != 0) written = false;
                    if (written) written = rename(pending_path, omdl_path) == 0;
                    if (!written) {
                        fprintf(stderr, "[OMDL] cache write failed, retaining previous cache\n");
                        unlink(pending_path);
                    }
                }
            }
        }

        instance->gltf_data = (void*)osg;
        osg->world_transforms = malloc(osg->node_count * sizeof(mat4));
        osg->local_transforms = malloc(osg->node_count * sizeof(mat4));

        if (omdl_vertices) free(omdl_vertices);
        if (omdl_indices) free(omdl_indices);
        if (omdl_morphs) free(omdl_morphs);
        if (omdl_metas) free(omdl_metas);
        if (omdl_meshlets) { free(omdl_meshlets); omdl_meshlets = NULL; }
        if (omdl_meshlet_bounds) { free(omdl_meshlet_bounds); omdl_meshlet_bounds = NULL; }
        if (omdl_meshlet_skins) { free(omdl_meshlet_skins); omdl_meshlet_skins = NULL; }
        if (omdl_meshlet_vertices) { free(omdl_meshlet_vertices); omdl_meshlet_vertices = NULL; }
        if (omdl_meshlet_triangles) { free(omdl_meshlet_triangles); omdl_meshlet_triangles = NULL; }
    } else {
        instance->gltf_data = (void*)omdl_cache_osg;

        for (size_t i = 0; i < instance->mesh_count; i++) {
            scene->meshes.items[instance->mesh_start_index + i].node = NULL;
            for (size_t n = 0; n < omdl_cache_osg->node_count; n++) {
                if (omdl_cache_osg->nodes[n].mesh_idx == (int32_t)i) {
                    scene->meshes.items[instance->mesh_start_index + i].node = (void*)(uintptr_t)n;
                    break;
                }
            }
        }

        if (omdl_cache_metas) free(omdl_cache_metas);
        omdl_cache_metas = NULL;
        omdl_cache_osg = NULL;
    }

    // Assign morphWeightOffset: pack each morph mesh into a unique slice of morphWeightBuffer.

    // The buffer is 1MB = 262144 floats, more than enough for all meshes combined.
    {
        static int global_morph_weight_counter = 0;
        size_t end = instance->mesh_start_index + instance->mesh_count;
        for (size_t i = instance->mesh_start_index; i < end; i++) {
            Mesh* m = &scene->meshes.items[i];
            if (m->morph_data && m->morphCount > 0) {
                m->morphWeightOffset = global_morph_weight_counter;
                global_morph_weight_counter += m->morphCount;
                printf("  Mesh '%s': morphWeightOffset=%d, morphCount=%d\n",
                       m->name ? m->name : "?", m->morphWeightOffset, m->morphCount);
            }
        }
    }

    // Prepare a parent-first hierarchy exactly once, for cache hits and misses.
    // Invalid or cyclic cache data never reaches the per-frame graph walker.
    // The live scene remains intact if a graph cannot be prepared, but its
    // animations are disabled until a valid scene is loaded.
    OmdlSceneGraph* osg = (OmdlSceneGraph*)instance->gltf_data;
    if (osg && osg->node_count && !osg->topology_order) {
        const size_t count = osg->node_count;
        // One-time scratch instead of persistent per-node status/DFS buffers.
        // The verified order is the ONLY topology metadata kept per frame.
        const bool sized = count <= SIZE_MAX / (sizeof(uint32_t) + sizeof(uint8_t));
        unsigned char *scratch = sized ? malloc(count * (sizeof(uint32_t) + sizeof(uint8_t))) : NULL;
        osg->topology_order = count <= SIZE_MAX / sizeof(uint32_t) ?
                              malloc(count * sizeof(uint32_t)) : NULL;
        uint32_t *stack = (uint32_t *)scratch;
        uint8_t *states = scratch ? scratch + count * sizeof(uint32_t) : NULL;
        const bool ordered = osg->nodes && osg->local_transforms &&
            osg->world_transforms && scratch && osg->topology_order &&
            obs_node_topological_order(osg->nodes, osg->node_count,
                sizeof(OmdlNode), offsetof(OmdlNode, parent),
                osg->topology_order, stack, states);
        free(scratch);
        if (!ordered) {
            free(osg->topology_order);
            osg->topology_order = NULL;
            fprintf(stderr, "[OMDL] invalid or unprepared hierarchy: animation disabled\n");
        }
    }
    // Kept only in memory, NOT serialized to an .omdl node cache.
    // Failed allocation leaves native animation working but rejects edits.
    if (osg && osg->node_count && !osg->script_translations)
        osg->script_translations = calloc(osg->node_count, sizeof(vec3));
    if (osg->skin_count > 0) {
        int* skin_offsets = malloc(osg->skin_count * sizeof(int));
        for (size_t s = 0; s < osg->skin_count; s++) {
            skin_offsets[s] = global_joint_counter;
            global_joint_counter += osg->skins[s].joints_count;
        }

        for (size_t i = instance->mesh_start_index; i < instance->mesh_start_index + instance->mesh_count; i++) {
            Mesh* m = &scene->meshes.items[i];
            if (m->node) {
                uint32_t node_idx = (uint32_t)(uintptr_t)m->node;
                if (osg->nodes[node_idx].skin_idx >= 0) {
                    m->jointOffset = skin_offsets[osg->nodes[node_idx].skin_idx];
                    m->jointCount = osg->skins[osg->nodes[node_idx].skin_idx].joints_count;
                }
            }
        }
        free(skin_offsets);
    }

    if (osg) {
        osg->bone_override_index_attempted = true;
        osg->bone_override_index_ready =
            prepare_bone_override_index(scene, instance, osg);
    }

    scene->gltf_instance_count++;
    scene_bump_gltf_revision(scene);

    printf("Loaded glTF instance #%zu with %zu meshes (indices %zu to %zu)\n",
           scene->gltf_instance_count - 1,
           instance->mesh_count,
           instance->mesh_start_index,
           instance->mesh_start_index + instance->mesh_count - 1);

    scene_topology_dirty = true;
    markMeshesSSBODirty(&context);

    // Register the loaded file into the scene hierarchy tree for UI display
    scene_tree_register_gltf(scene, instance, filepath);
    instance->source_path = strdup(filepath);

    // Completely destroy cgltf! The engine now relies strictly on the OMDL Binary memory.
    if (data) cgltf_free(data);

    return true;
}

bool obs_gltf_last_load_used_cache(void) {
    return is_omdl_cache_hit;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

/// find_keyframe — implementation group
static size_t find_keyframe(float* times, size_t count, float time) {
    if (count < 2) return 0;

    // AAA Binary Search: Slashes O(N) to O(log N)
    size_t low = 0;
    size_t high = count - 2;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        if (time < times[mid]) {
            if (mid == 0) return 0;
            high = mid - 1;
        } else if (time >= times[mid + 1]) {
            low = mid + 1;
        } else {
            return mid;
        }
    }
    return count - 2;
}

static const OmdlSkin *mesh_skin_for_override_index(
    const Scene *scene, const GLTFInstance *instance,
    const OmdlSceneGraph *osg, size_t relative_mesh) {
    const Mesh *mesh =
        &scene->meshes.items[instance->mesh_start_index + relative_mesh];
    if (!mesh->node || mesh->jointCount <= 0)
        return NULL;
    const uint32_t node_index = (uint32_t)(uintptr_t)mesh->node;
    if (node_index >= osg->node_count)
        return NULL;
    const int32_t skin_index = osg->nodes[node_index].skin_idx;
    if (skin_index < 0 || (uint32_t)skin_index >= osg->skin_count)
        return NULL;
    return &osg->skins[skin_index];
}

/* Build once from immutable mesh/skin topology. Allocation failure leaves the
 * old scan available; the per-frame path then retains its original behavior. */
static bool prepare_bone_override_index(Scene *scene,
                                        const GLTFInstance *instance,
                                        OmdlSceneGraph *osg) {
    if (!scene || !instance || !osg || instance->mesh_count > UINT32_MAX ||
        (instance->mesh_count && !scene->meshes.items) ||
        (osg->node_count && !osg->nodes) ||
        (osg->skin_count && !osg->skins))
        return false;
    if (!instance->mesh_count || !osg->skin_count)
        return true;

    size_t source_capacity = 0;
    for (size_t m = 0; m < instance->mesh_count; ++m) {
        const OmdlSkin *skin =
            mesh_skin_for_override_index(scene, instance, osg, m);
        if (!skin)
            continue;
        if (skin->joints_count > SIZE_MAX - source_capacity)
            return false;
        source_capacity += skin->joints_count;
    }
    if (!source_capacity)
        return true;
    if (!osg->skin_joints ||
        source_capacity > SIZE_MAX / sizeof(ObsSkinJointRef))
        return false;

    ObsSkinJointRef *source =
        malloc(source_capacity * sizeof(*source));
    if (!source)
        return false;
    size_t source_count = 0;
    for (size_t m = 0; m < instance->mesh_count; ++m) {
        const OmdlSkin *skin =
            mesh_skin_for_override_index(scene, instance, osg, m);
        if (!skin)
            continue;
        for (uint32_t j = 0; j < skin->joints_count; ++j) {
            source[source_count++] = (ObsSkinJointRef){
                .node_index = osg->skin_joints[skin->joints_offset + j],
                .mesh_index = (uint32_t)m,
                .joint_index = j,
            };
        }
    }

    const bool built = obs_skin_override_index_build(
        osg->node_count, instance->mesh_count, source, source_count,
        &osg->bone_override_index);
    free(source);
    return built;
}

static float animation_channel_factor(OmdlSceneGraph *osg,
                                     const OmdlChannel *channel,
                                     float anim_time, size_t *k0,
                                     size_t *k1) {
    float *times = &osg->anim_floats[channel->times_offset];
    *k0 = find_keyframe(times, channel->keyframe_count, anim_time);
    *k1 = channel->keyframe_count > 1 ? *k0 + 1 : *k0;
    float t0 = times[*k0];
    float t1 = times[*k1];
    float factor = t1 > t0 ? (anim_time - t0) / (t1 - t0) : 0.0f;
    if (factor < 0.0f)
        factor = 0.0f;
    if (factor > 1.0f)
        factor = 1.0f;
    return factor;
}

static void apply_animation_weights(Scene *scene, GLTFInstance *instance,
                                    OmdlSceneGraph *osg,
                                    const OmdlChannel *channel,
                                    float anim_time) {
    size_t k0, k1;
    float factor =
        animation_channel_factor(osg, channel, anim_time, &k0, &k1);
    size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
    for (size_t m = instance->mesh_start_index; m < mesh_end; ++m) {
        Mesh *mesh = &scene->meshes.items[m];
        if (!mesh->node ||
            (uint32_t)(uintptr_t)mesh->node != channel->target_node ||
            !mesh->morph_data)
            continue;
        size_t num_targets = mesh->morph_data->target_count;
        for (size_t t = 0; t < num_targets; ++t) {
            float w0 =
                osg->anim_floats[channel->values_offset + k0 * num_targets + t];
            float w1 =
                osg->anim_floats[channel->values_offset + k1 * num_targets + t];
            mesh->morph_data->weights[t] = lerp(w0, w1, factor);
        }
        markMeshDirty(&context, (uint32_t)m);
    }
}

static void apply_animation_transform(OmdlSceneGraph *osg,
                                      const OmdlChannel *channel,
                                      float anim_time, vec3 T, versor R,
                                      vec3 S) {
    size_t k0, k1;
    float factor =
        animation_channel_factor(osg, channel, anim_time, &k0, &k1);
    size_t components =
        channel->path_type == cgltf_animation_path_type_rotation ? 4 : 3;
    float *v0 =
        &osg->anim_floats[channel->values_offset + k0 * components];
    float *v1 =
        &osg->anim_floats[channel->values_offset + k1 * components];

    if (channel->path_type == cgltf_animation_path_type_translation) {
        T[0] = lerp(v0[0], v1[0], factor);
        T[1] = lerp(v0[1], v1[1], factor);
        T[2] = lerp(v0[2], v1[2], factor);
    } else if (channel->path_type == cgltf_animation_path_type_rotation) {
        versor r0 = {v0[0], v0[1], v0[2], v0[3]};
        versor r1 = {v1[0], v1[1], v1[2], v1[3]};
        glm_quat_slerp(r0, r1, factor, R);
        glm_quat_normalize(R);
    } else if (channel->path_type == cgltf_animation_path_type_scale) {
        S[0] = lerp(v0[0], v1[0], factor);
        S[1] = lerp(v0[1], v1[1], factor);
        S[2] = lerp(v0[2], v1[2], factor);
    }
}

void animate_scene(Scene* scene, float time) {
    (void)time; // Ignore global time to allow Editor control

    for (size_t inst = 0; inst < scene->gltf_instance_count; inst++) {
        GLTFInstance* instance = &scene->gltf_instances[inst];
        OmdlSceneGraph* osg = (OmdlSceneGraph*)instance->gltf_data;
        if (!osg || !osg->node_count || !osg->topology_order ||
            !osg->nodes || !osg->local_transforms || !osg->world_transforms ||
            instance->mesh_start_index > scene->meshes.count ||
            instance->mesh_count > scene->meshes.count - instance->mesh_start_index)
            continue;
        OmdlAnimation* anim = osg->anim_count > 0 ? &osg->anims[0] : NULL;
        ObsAnimationChannelIndex *channel_index =
            &osg->animation_channel_index;
        bool use_channel_index = false;
        if (anim && anim->channel_count &&
            (uint64_t)osg->node_count * anim->channel_count > 4096u) {
            use_channel_index = obs_animation_channel_index_build(
                osg->node_count, osg->channels, anim, channel_index);
        }

        // SYNC WITH EDITOR: Drive animation strictly via the timeline's playhead!
        float anim_time = g_anim_editor.time;
        if (anim && anim_time > anim->duration) anim_time = anim->duration;

        // Phase 1: Evaluate Channels and Build Local Matrices
        for (uint32_t n = 0; n < osg->node_count; n++) {
            OmdlNode* node = &osg->nodes[n];
            vec3 T = { node->translation[0], node->translation[1], node->translation[2] };
            versor R = { node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3] };
            vec3 S = { node->scale[0], node->scale[1], node->scale[2] };
            bool animated = false;

            if (anim) {
                if (use_channel_index) {
                    if (n == 0) {
                        for (uint32_t i = 0;
                             i < channel_index->weight_index_count; ++i) {
                            const OmdlChannel *channel =
                                &osg->channels[anim->channel_offset +
                                               channel_index->weight_indices[i]];
                            apply_animation_weights(scene, instance, osg,
                                                    channel, anim_time);
                        }
                    }
                    for (uint32_t i = channel_index->node_offsets[n];
                         i < channel_index->node_offsets[n + 1]; ++i) {
                        const OmdlChannel *channel =
                            &osg->channels[anim->channel_offset +
                                           channel_index->channel_indices[i]];
                        animated = true;
                        apply_animation_transform(osg, channel, anim_time, T,
                                                  R, S);
                    }
                } else {
                    for (uint32_t c = 0; c < anim->channel_count; c++) {
                        OmdlChannel *channel =
                            &osg->channels[anim->channel_offset + c];
                        if (channel->target_node == UINT32_MAX)
                            continue;
                        if (channel->path_type ==
                            cgltf_animation_path_type_weights) {
                            if (n == 0)
                                apply_animation_weights(scene, instance, osg,
                                                        channel, anim_time);
                            continue;
                        }
                        if (channel->target_node != n)
                            continue;
                        animated = true;
                        apply_animation_transform(osg, channel, anim_time, T,
                                                  R, S);
                    }
                }
            }

            mat4 local;
            if (animated) {
                glm_mat4_identity(local);
                glm_translate(local, T);
                mat4 rot_mat; glm_quat_mat4(R, rot_mat);
                glm_mat4_mul(local, rot_mat, local);
                glm_scale(local, S);
            } else {
                glm_mat4_copy(node->base_matrix, local);
            }

            // Preserve script-owned local offsets after every animation sample.
            // Post-multiplication applies an offset in the node's local axes.
            if (osg->script_translations)
                obs_node_apply_local_translation(local, osg->script_translations[n]);
            glm_mat4_copy(local, osg->local_transforms[n]);
        }

        // Phase 2: One parent-first pass through a validated, cached order.
        // No repeated parent walks, recursion, per-frame clearing or allocation.
        for (uint32_t k = 0; k < osg->node_count; ++k) {
            uint32_t curr = osg->topology_order[k];
            int32_t p = osg->nodes[curr].parent;
            if (p >= 0) {
                glm_mat4_mul(osg->world_transforms[p], osg->local_transforms[curr],
                             osg->world_transforms[curr]);
            } else if (instance->mesh_count > 0) {
                glm_mat4_mul(instance->instance_transform,
                             osg->local_transforms[curr],
                             osg->world_transforms[curr]);
            } else {
                glm_mat4_copy(osg->local_transforms[curr], osg->world_transforms[curr]);
            }

            // Preserve the existing additive order: mesh order, then skin
            // joint order. The inverse map removes the node×mesh×joint scan.
            if (osg->bone_override_index_ready) {
                const ObsSkinOverrideIndex *index = &osg->bone_override_index;
                if (index->ref_count) {
                    for (size_t r = index->offsets[curr];
                         r < index->offsets[curr + 1]; ++r) {
                        const ObsSkinOverrideRef ref = index->refs[r];
                        Mesh *mesh = &scene->meshes.items[
                            instance->mesh_start_index + ref.mesh_index];
                        MeshBoneOverride *override =
                            mesh_bone_override_get(mesh, ref.joint_index, false);
                        if (override && override->active)
                            glm_mat4_mul(override->world_offset,
                                         osg->world_transforms[curr],
                                         osg->world_transforms[curr]);
                    }
                }
            } else {
                size_t mesh_end =
                    instance->mesh_start_index + instance->mesh_count;
                for (size_t m = instance->mesh_start_index; m < mesh_end; ++m) {
                    Mesh *mesh = &scene->meshes.items[m];
                    if (!mesh->node || mesh->jointCount <= 0) continue;
                    uint32_t node_idx = (uint32_t)(uintptr_t)mesh->node;
                    if (node_idx >= osg->node_count) continue;
                    int32_t skin_idx = osg->nodes[node_idx].skin_idx;
                    if (skin_idx < 0 ||
                        (uint32_t)skin_idx >= osg->skin_count) continue;
                    OmdlSkin *skin = &osg->skins[skin_idx];
                    for (uint32_t j = 0; j < skin->joints_count; ++j) {
                        if (osg->skin_joints[skin->joints_offset + j] != curr)
                            continue;
                        MeshBoneOverride *override =
                            mesh_bone_override_get(mesh, j, false);
                        if (override && override->active)
                            glm_mat4_mul(override->world_offset,
                                         osg->world_transforms[curr],
                                         osg->world_transforms[curr]);
                        break;
                    }
                }
            }
            int32_t rel_mesh = osg->nodes[curr].mesh_idx;
            if (rel_mesh >= 0 && (size_t)rel_mesh < instance->mesh_count) {
                size_t m = instance->mesh_start_index + (size_t)rel_mesh;
                Mesh *mesh = &scene->meshes.items[m];
                // Stationary scenes should not submit redundant SSBO updates.
                if (memcmp(mesh->model, osg->world_transforms[curr], sizeof(mat4)) != 0) {
                    memcpy(mesh->model, osg->world_transforms[curr], sizeof(mat4));
                    markMeshDirty(&context, (uint32_t)m);
                }
            }
        }

        // Phase 3: Hardware Skinning
        mat4* joint_buffer = jointSSBOMapped[context.currentFrame];
        if (joint_buffer && osg->skin_count > 0) {
            size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
            for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                Mesh* mesh = &scene->meshes.items[m];
                if (mesh->node && mesh->jointOffset >= 0) {
                    uint32_t node_idx = (uint32_t)(uintptr_t)mesh->node;
                    OmdlSkin* skin = &osg->skins[osg->nodes[node_idx].skin_idx];
                    mat4 inverse_mesh_world; glm_mat4_inv(mesh->model, inverse_mesh_world);

                    for (uint32_t j = 0; j < skin->joints_count; j++) {
                        uint32_t joint_node_idx = osg->skin_joints[skin->joints_offset + j];
                        mat4 final_joint;
                        glm_mat4_mul(inverse_mesh_world, osg->world_transforms[joint_node_idx], final_joint);
                        glm_mat4_mul(final_joint, osg->skin_ibms[skin->ibm_offset + j], final_joint);

                        float* dst = (float*)((uint8_t*)joint_buffer + (mesh->jointOffset + j) * 48);
                        dst[0] = final_joint[0][0]; dst[1] = final_joint[1][0]; dst[2] = final_joint[2][0]; dst[3] = final_joint[3][0];
                        dst[4] = final_joint[0][1]; dst[5] = final_joint[1][1]; dst[6] = final_joint[2][1]; dst[7] = final_joint[3][1];
                        dst[8] = final_joint[0][2]; dst[9] = final_joint[1][2]; dst[10]= final_joint[2][2]; dst[11]= final_joint[3][2];
                    }
                }
            }
        }
    }
}

/// Asynchronous OMDL cache-hit worker — no mutable loader globals
/* This path is deliberately independent of load_gltf's synchronous cooker.
 * The worker owns every CPU result and rejects cache misses. */
static const char *async_meta_texture_path(const void *source, size_t mesh,
                                            ObsCacheTextureChannel channel) {
    return omdl_texture_path(source, mesh, channel);
}

void obs_gltf_cache_destroy(ObsGltfCacheCandidate *c) {
    if (!c) return;
    for (size_t i = 0; c->meshes && i < c->mesh_count; ++i) {
        Mesh *m = &c->meshes[i];
        free(m->name);
        if (m->morph_data) {
            free(m->morph_data->weights);
            free(m->morph_data);
        }
    }
    free(c->meshes);
    omdl_scene_graph_destroy(c->graph);
    free(c->metadata);
    for (size_t i = 0; i < OBS_GEOM_POOL_COUNT; ++i) free(c->streams[i]);
    obs_cache_textures_dispose(&c->textures);
    free(c->source_path);
    free(c);
}

static bool async_cache_location(const char *filepath, char path[512]) {
    if (!filepath || strlen(filepath) >= 256) return false;
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char safe[256];
    memcpy(safe, filepath, strlen(filepath) + 1);
    for (size_t i = 0; safe[i]; ++i)
        if (safe[i] == '/' || safe[i] == '\\' || safe[i] == '.') safe[i] = '_';
    int written = snprintf(path, 512, "%s/.cache/obsidian/models/%s.omdl",
                           home, safe);
    return written >= 0 && written < 512;
}

bool obs_gltf_cache_prepare_cpu(const char *source, ObsGltfCacheCandidate **out) {
    if (!out) return false;
    *out = NULL;
    char cache_path[512];
    if (!async_cache_location(source, cache_path)) return false;
    /* Avoid parsing/loading the entire source twice on a cold cache miss:
     * the async worker immediately builds a source inventory if no OMDL
     * exists. The cook path is still separate from legacy load_gltf(). */
    FILE *fp = fopen(cache_path, "rb");
    if (!fp) return false;
    uint64_t source_digest = 0;
    cgltf_options options = {0};
    cgltf_data *parsed = NULL;
    if (cgltf_parse_file(&options, source, &parsed) != cgltf_result_success) {
        fclose(fp);
        return false;
    }
    bool ok = cgltf_load_buffers(&options, parsed, source) == cgltf_result_success &&
              omdl_source_digest(parsed, &source_digest);
    ObsGltfCacheCandidate *c = ok ? calloc(1, sizeof(*c)) : NULL;
    OmdlLayout layout = {0};
    if (!c || !omdl_cache_preflight(fp, &layout, source_digest,
                                    omdl_native_abi_tag(), &c->metadata, &c->graph))
        ok = false;
    uint32_t source_meshes = 0;
    if (ok && (!omdl_source_mesh_count(parsed, &source_meshes) ||
               source_meshes != layout.header.mesh_count ||
               parsed->nodes_count != layout.header.node_count ||
               parsed->skins_count != layout.header.skin_count ||
               parsed->animations_count != layout.header.anim_count)) ok = false;
    if (ok) {
        const OmdlSection sections[OBS_GEOM_POOL_COUNT] = {
            OMDL_VERTICES, OMDL_INDICES, OMDL_MORPHS, OMDL_MESHLETS,
            OMDL_MESHLET_BOUNDS, OMDL_MESHLET_SKINS,
            OMDL_MESHLET_VERTICES, OMDL_MESHLET_TRIANGLES
        };
        const size_t sizes[OBS_GEOM_POOL_COUNT] = {
            sizeof(Vertex), sizeof(uint32_t), sizeof(MorphDelta), sizeof(Meshlet),
            sizeof(MeshletBounds), sizeof(MeshletSkinData),
            sizeof(uint32_t), sizeof(uint8_t)
        };
        for (size_t i = 0; ok && i < OBS_GEOM_POOL_COUNT; ++i) {
            OmdlSection section = sections[i];
            uint64_t bytes = layout.bytes[section];
            if (bytes > SIZE_MAX || bytes % sizes[i] ||
                bytes / sizes[i] > UINT32_MAX) {
                ok = false;
                break;
            }
            c->counts[i] = (uint32_t)(bytes / sizes[i]);
            if (!omdl_read_array(fp, &layout, section, &c->streams[i])) {
                ok = false;
                break;
            }
            c->spans[i] = (ObsGeometrySourceSpan){c->streams[i], (size_t)bytes};
        }
    }
    if (ok && !layout.header.mesh_count) ok = false;
    if (ok) {
        /* Initialize bind-pose transforms in verified parent-first order. */
        OmdlSceneGraph *g = c->graph;
        for (uint32_t i = 0; i < g->node_count; ++i) {
            uint32_t n = g->topology_order[i];
            OmdlNode *node = &g->nodes[n];
            glm_mat4_copy(node->base_matrix, g->local_transforms[n]);
            if (node->parent >= 0)
                glm_mat4_mul(g->world_transforms[node->parent],
                             g->local_transforms[n], g->world_transforms[n]);
            else
                glm_mat4_copy(g->local_transforms[n], g->world_transforms[n]);
        }
        c->mesh_count = layout.header.mesh_count;
        c->meshes = calloc(c->mesh_count, sizeof(Mesh));
        c->source_path = strdup(source);
        ok = c->meshes && c->source_path;
        for (size_t i = 0; ok && i < c->mesh_count; ++i) {
            const OmdlMeshMeta *meta = &c->metadata[i];
            Mesh *m = &c->meshes[i];
            m->name = strdup(meta->name);
            if (!m->name) {
                ok = false;
                break;
            }
            m->visible = meta->visible;
            m->vertexCount = meta->vertexCount;
            m->indexCount = meta->indexCount;
            m->meshletCount = meta->meshletCount;
            if (meta->morphCount > INT_MAX) {
                ok = false;
                break;
            }
            m->morphCount = (int)meta->morphCount;
            m->megaBaseVertex = m->megaBaseIndex = m->megaBaseMeshlet = UINT32_MAX;
            m->megaBaseMeshletVertex = m->megaBaseMeshletTriangle = UINT32_MAX;
            m->morphDeltaOffset = m->dynamicBaseVertex = UINT32_MAX;
            m->textureIndex = m->normalMapIndex = m->metallicRoughIndex = -1;
            m->aoIndex = m->emissiveIndex = m->transmissionIndex =
                m->thicknessIndex = -1;
            m->jointOffset = -1;
            m->alpha_mode = meta->alpha_mode;
            m->alpha_cutoff = meta->alpha_cutoff;
            m->is_unlit = meta->is_unlit;
            m->metallicFactor = meta->metallicFactor;
            m->roughnessFactor = meta->roughnessFactor;
            m->emissiveStrength = meta->emissiveStrength;
            m->aoStrength = meta->aoStrength;
            m->transmissionFactor = meta->transmissionFactor;
            m->ior = meta->ior;
            m->thicknessFactor = meta->thicknessFactor;
            m->attenuationDistance = meta->attenuationDistance;
            m->dispersion = meta->dispersion;
            glm_vec3_copy(meta->aabbMin, m->aabbMin);
            glm_vec3_copy(meta->aabbMax, m->aabbMax);
            glm_vec4_copy(meta->baseColorFactor, m->baseColorFactor);
            glm_vec3_copy(meta->emissiveFactor, m->emissiveFactor);
            glm_vec3_copy(meta->attenuationColor, m->attenuationColor);
            glm_mat4_identity(m->model);
            glm_mat4_identity(m->local_transform);
            for (uint32_t n = 0; n < c->graph->node_count; ++n) {
                if (c->graph->nodes[n].mesh_idx != (int32_t)i) continue;
                m->node = (void *)(uintptr_t)n;
                glm_mat4_copy(c->graph->world_transforms[n], m->model);
                glm_mat4_copy(c->graph->local_transforms[n], m->local_transform);
                if (c->graph->nodes[n].skin_idx >= 0) {
                    uint32_t skin_index =
                        (uint32_t)c->graph->nodes[n].skin_idx;
                    m->jointCount =
                        (int)c->graph->skins[skin_index].joints_count;
                }
                break;
            }
            if (meta->morphCount) {
                m->morph_data = calloc(1, sizeof(MorphData));
                if (!m->morph_data) {
                    ok = false;
                    break;
                }
                m->morph_data->target_count = meta->morphCount;
                m->morph_data->weights =
                    calloc(meta->morphCount, sizeof(float));
                if (!m->morph_data->weights) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok && c->graph->node_count) {
            c->graph->script_translations =
                calloc(c->graph->node_count, sizeof(vec3));
            ok = c->graph->script_translations != NULL;
        }
        if (ok)
            ok = obs_cache_textures_prepare(&c->textures, c->mesh_count,
                                             async_meta_texture_path,
                                             c->metadata);
    }
    if (fp) fclose(fp);
    cgltf_free(parsed);
    if (!ok) {
        obs_gltf_cache_destroy(c);
        return false;
    }
    *out = c;
    return true;
}
