#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include <string.h>
#include "context.h"
#include "vulkan_setup.h"
#include <sys/stat.h>
#include <sys/types.h>

static int32_t gltf_texture_indices[MAX_TEXTURES];
static size_t gltf_texture_count = 0;

#define OMDL_MAGIC 0x4C444D4F // 'OMDL'

typedef struct {
    uint32_t magic;
    uint32_t mesh_count;
    uint32_t total_vertices;
    uint32_t total_indices;
    uint32_t total_morph_deltas;
    uint32_t reserved[3];
} OmdlHeader;

typedef struct {
    char name[256];
    vec3 aabbMin;
    vec3 aabbMax;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t morphCount;
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t morphOffset;

    vec4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float emissiveStrength;
    vec3 emissiveFactor;
    int alpha_mode;
    float alpha_cutoff;
    bool is_unlit;

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    vec3 attenuationColor;
    float attenuationDistance;
    float dispersion;

    char tex_albedo[512];
    char tex_normal[512];
    char tex_metallicRoughness[512];
    char tex_occlusion[512];
    char tex_emissive[512];
    char tex_transmission[512];
    char tex_thickness[512];
} OmdlMeshMeta;

static char gltf_texture_paths[MAX_TEXTURES][512];

// OMDL Cooker Arrays
static Vertex* omdl_vertices = NULL;
static uint32_t* omdl_indices = NULL;
static MorphDelta* omdl_morphs = NULL;
static OmdlMeshMeta* omdl_metas = NULL;
static uint32_t omdl_vertex_count = 0;
static uint32_t omdl_index_count = 0;
static uint32_t omdl_morph_count = 0;
static uint32_t omdl_mesh_count = 0;

// OMDL Loader State
static bool is_omdl_cache_hit = false;
static OmdlMeshMeta* omdl_cache_metas = NULL;
static uint32_t omdl_cache_idx = 0;
static uint32_t omdl_base_v = 0;
static uint32_t omdl_base_i = 0;
static uint32_t omdl_base_m = 0;

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
            snprintf(virtual_filename, sizeof(virtual_filename), "%s_embedded_tex_%zu.png", base_path, i);
            strcpy(gltf_texture_paths[i], virtual_filename);

            cgltf_buffer_view* view = img->buffer_view;
            unsigned char* buffer_data = (unsigned char*)view->buffer->data + view->offset;
            tex_id = texture_pool_add_embedded(&context, virtual_filename, buffer_data, view->size);
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

static Mesh create_mesh_from_primitive(cgltf_primitive* prim, cgltf_data* data, const char* name) {
    Mesh mesh = {0};
    mesh.name = strdup(name);
    mesh.textureIndex = -1;
    mesh.normalMapIndex = -1;
    mesh.metallicRoughIndex = -1;
    mesh.aoIndex = -1;
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
        OmdlMeshMeta meta = omdl_cache_metas[omdl_cache_idx++];
        glm_vec3_copy(meta.aabbMin, mesh.aabbMin);
        glm_vec3_copy(meta.aabbMax, mesh.aabbMax);
        mesh.vertexCount = meta.vertexCount;
        mesh.indexCount = meta.indexCount;

        mesh.megaBaseVertex = (omdl_base_v != UINT32_MAX) ? omdl_base_v + meta.vertexOffset : UINT32_MAX;
        mesh.megaBaseIndex = (omdl_base_i != UINT32_MAX && meta.indexCount > 0) ? omdl_base_i + meta.indexOffset : UINT32_MAX;
        mesh.dynamicBaseVertex = UINT32_MAX;

        glm_vec4_copy(meta.baseColorFactor, mesh.baseColorFactor);
        mesh.metallicFactor = meta.metallicFactor;
        mesh.roughnessFactor = meta.roughnessFactor;
        mesh.emissiveStrength = meta.emissiveStrength;
        glm_vec3_copy(meta.emissiveFactor, mesh.emissiveFactor);
        mesh.alpha_mode = meta.alpha_mode;
        mesh.alpha_cutoff = meta.alpha_cutoff;
        mesh.is_unlit = meta.is_unlit;
        mesh.transmissionFactor = meta.transmissionFactor;
        mesh.ior = meta.ior;
        mesh.thicknessFactor = meta.thicknessFactor;
        glm_vec3_copy(meta.attenuationColor, mesh.attenuationColor);
        mesh.attenuationDistance = meta.attenuationDistance;
        mesh.dispersion = meta.dispersion;

        // Route cached texture paths back through the instant DDS pipeline
        if (meta.tex_albedo[0]) mesh.textureIndex = texture_pool_add(&context, meta.tex_albedo);
        if (meta.tex_normal[0]) mesh.normalMapIndex = texture_pool_add(&context, meta.tex_normal);
        if (meta.tex_metallicRoughness[0]) mesh.metallicRoughIndex = texture_pool_add(&context, meta.tex_metallicRoughness);
        if (meta.tex_occlusion[0]) mesh.aoIndex = texture_pool_add(&context, meta.tex_occlusion);
        if (meta.tex_emissive[0]) mesh.emissiveIndex = texture_pool_add(&context, meta.tex_emissive);
        if (meta.tex_transmission[0]) mesh.transmissionIndex = texture_pool_add(&context, meta.tex_transmission);
        if (meta.tex_thickness[0]) mesh.thicknessIndex = texture_pool_add(&context, meta.tex_thickness);
        if (mesh.textureIndex >= 0) mesh.texture = texture_pool_get(mesh.textureIndex);

        if (meta.morphCount > 0) {
            mesh.morphCount = meta.morphCount;
            mesh.morphDeltaOffset = (omdl_base_m != UINT32_MAX) ? omdl_base_m + meta.morphOffset : UINT32_MAX;
            mesh.morph_data = malloc(sizeof(MorphData));
            mesh.morph_data->target_count = meta.morphCount;
            mesh.morph_data->weights = calloc(meta.morphCount, sizeof(float));
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

    /* PBR material defaults */
    glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, mesh.baseColorFactor);
    mesh.metallicFactor    = 0.0f; // Fix: Default to dielectric (plastic), not metal!
    mesh.roughnessFactor   = 0.5f; // Fix: Default to matte, not a perfect mirror!
    mesh.emissiveStrength  = 1.0f;
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

        /* Occlusion map */
        if (prim->material->occlusion_texture.texture) {
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] == prim->material->occlusion_texture.texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0)
                        mesh.aoIndex = gltf_texture_indices[t];
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

    final_indices = indices;
    indices = NULL;

    final_morphs = extract_morph_targets(prim, final_vertex_count);

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
    glm_vec3_copy(mesh.aabbMin, meta.aabbMin);
    glm_vec3_copy(mesh.aabbMax, meta.aabbMax);
    meta.vertexCount = final_vertex_count;
    meta.indexCount = final_index_count;
    meta.morphCount = final_morph_count;
    meta.vertexOffset = omdl_vertex_count;
    meta.indexOffset = omdl_index_count;
    meta.morphOffset = omdl_morph_count;

    glm_vec4_copy(mesh.baseColorFactor, meta.baseColorFactor);
    meta.metallicFactor = mesh.metallicFactor;
    meta.roughnessFactor = mesh.roughnessFactor;
    meta.emissiveStrength = mesh.emissiveStrength;
    glm_vec3_copy(mesh.emissiveFactor, meta.emissiveFactor);
    meta.alpha_mode = mesh.alpha_mode;
    meta.alpha_cutoff = mesh.alpha_cutoff;
    meta.is_unlit = mesh.is_unlit;
    meta.transmissionFactor = mesh.transmissionFactor;
    meta.ior = mesh.ior;
    meta.thicknessFactor = mesh.thicknessFactor;
    glm_vec3_copy(mesh.attenuationColor, meta.attenuationColor);
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

    if (final_vertices) free(final_vertices);
    if (final_indices) free(final_indices);
    if (final_morphs) free(final_morphs);

    mesh.vertexCount = final_vertex_count;
    mesh.indexCount  = final_index_count;
    mesh.megaBaseVertex = UINT32_MAX;
    mesh.megaBaseIndex = UINT32_MAX;
    mesh.dynamicBaseVertex = UINT32_MAX;

    return mesh;
}


static void process_node(cgltf_node* node, cgltf_data* data, Meshes* meshes, mat4 parent_transform) {
    mat4 local_transform;
    mat4 world_transform;

    cgltf_node_transform_local(node, (float*)local_transform);
    glm_mat4_mul(parent_transform, local_transform, world_transform);

    size_t mesh_start = meshes->count;

    if (node->mesh) {
        cgltf_mesh* gltf_mesh = node->mesh;

        for (size_t i = 0; i < gltf_mesh->primitives_count; i++) {
            char mesh_name[256];
            snprintf(mesh_name, sizeof(mesh_name), "%s_prim_%zu",
                     node->name ? node->name : "node", i);

            Mesh mesh = create_mesh_from_primitive(&gltf_mesh->primitives[i], data, mesh_name);

            if (mesh.vertexCount > 0) {
                mesh.node = node;
                glm_mat4_copy(world_transform, mesh.model);
                glm_mat4_copy(local_transform, mesh.local_transform);

                // Copy initial weights from mesh if available
                if (mesh.morph_data && gltf_mesh->weights_count > 0) {
                    for (size_t w = 0; w < mesh.morph_data->target_count && w < gltf_mesh->weights_count; w++) {
                        mesh.morph_data->weights[w] = gltf_mesh->weights[w];
                    }
                }

                meshes_add(meshes, mesh);
            }
        }
    }

    size_t mesh_count = meshes->count - mesh_start;
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

    printf("Successfully loaded %zu meshes from scene graph\n", meshes->count);
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

bool load_gltf(const char* filepath, Scene* scene) {
    char omdl_path[512];
    const char* home = getenv("HOME");
    if (!home) home = ".";
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.cache", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home); mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/models", home); mkdir(dir_path, 0777);

    char safe_name[256];
    strncpy(safe_name, filepath, sizeof(safe_name) - 1);
    for (int i = 0; safe_name[i]; i++) {
        if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.') safe_name[i] = '_';
    }
    snprintf(omdl_path, sizeof(omdl_path), "%s/%s.omdl", dir_path, safe_name);

    is_omdl_cache_hit = false;
    FILE* fomdl = fopen(omdl_path, "rb");
    if (fomdl) {
        OmdlHeader header;
        if (fread(&header, sizeof(OmdlHeader), 1, fomdl) == 1 && header.magic == OMDL_MAGIC) {
            fprintf(stdout, "\033[32m[OMDL] AAA Pipeline: Instant Cache Hit -> %s\033[0m\n", omdl_path);
            is_omdl_cache_hit = true;

            omdl_cache_metas = malloc(header.mesh_count * sizeof(OmdlMeshMeta));
            fread(omdl_cache_metas, sizeof(OmdlMeshMeta), header.mesh_count, fomdl);

            omdl_base_v = header.total_vertices > 0 ? megaBufferAllocateFromFile(&context, fomdl, header.total_vertices) : UINT32_MAX;
            omdl_base_i = header.total_indices > 0 ? megaIndexBufferAllocateFromFile(&context, fomdl, header.total_indices) : UINT32_MAX;
            omdl_base_m = header.total_morph_deltas > 0 ? megaMorphBufferAllocateFromFile(&context, fomdl, header.total_morph_deltas) : UINT32_MAX;

            /* CRITICAL FIX: Flush the pending staging buffer! The NVMe read the data into the
               persistent CPU-mapped staging memory, but the GPU copy commands were never submitted! */
            flushUploadStagingBuffer(&context);

            omdl_cache_idx = 0;
        }
        fclose(fomdl);
    }

    cgltf_options options = {0};
    cgltf_data* data = NULL;
    if (cgltf_parse_file(&options, filepath, &data) != cgltf_result_success) return false;

    // Must load buffers even on cache hit until animations/skins are fully serialized into OMDL
    if (cgltf_load_buffers(&options, data, filepath) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    if (!is_omdl_cache_hit) {
        fprintf(stdout, "\033[33m[OMDL] Cache Miss: Compiling GLTF to monolithic binary...\033[0m\n");

        omdl_vertex_count = 0; omdl_index_count = 0; omdl_morph_count = 0; omdl_mesh_count = 0;
        omdl_vertices = NULL; omdl_indices = NULL; omdl_morphs = NULL; omdl_metas = NULL;

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
    instance->gltf_data = data;
    instance->animations = NULL;
    instance->animation_count = 0;
    instance->mesh_count = 0;

    load_gltf_meshes(data, &scene->meshes);
    instance->mesh_count = scene->meshes.count - instance->mesh_start_index;

    if (!is_omdl_cache_hit) {
        // 1. Bulk GPU Allocation
        createUploadStagingBuffer(&context, 256 * 1024 * 1024);
        uint32_t base_v = omdl_vertex_count > 0 ? megaBufferAllocate(&context, omdl_vertices, omdl_vertex_count) : UINT32_MAX;
        uint32_t base_i = omdl_index_count > 0 ? megaIndexBufferAllocate(&context, omdl_indices, omdl_index_count) : UINT32_MAX;
        uint32_t base_m = omdl_morph_count > 0 ? megaMorphBufferAllocate(&context, omdl_morphs, omdl_morph_count) : UINT32_MAX;
        flushUploadStagingBuffer(&context);
        destroyUploadStagingBuffer(&context);

        // 2. Patch Runtime Meshes
        for (size_t i = 0; i < omdl_mesh_count; i++) {
            Mesh* m = &scene->meshes.items[instance->mesh_start_index + i];
            m->megaBaseVertex = (base_v != UINT32_MAX) ? base_v + omdl_metas[i].vertexOffset : UINT32_MAX;
            if (m->indexCount > 0) m->megaBaseIndex = (base_i != UINT32_MAX) ? base_i + omdl_metas[i].indexOffset : UINT32_MAX;
            if (m->morphCount > 0) m->morphDeltaOffset = (base_m != UINT32_MAX) ? base_m + omdl_metas[i].morphOffset : UINT32_MAX;
        }

        // 3. Write Monolithic OMDL Binary
        FILE* fout = fopen(omdl_path, "wb");
        if (fout) {
            OmdlHeader header = { OMDL_MAGIC, omdl_mesh_count, omdl_vertex_count, omdl_index_count, omdl_morph_count, {0,0,0} };
            fwrite(&header, sizeof(OmdlHeader), 1, fout);
            fwrite(omdl_metas, sizeof(OmdlMeshMeta), omdl_mesh_count, fout);
            if (omdl_vertex_count > 0) fwrite(omdl_vertices, sizeof(Vertex), omdl_vertex_count, fout);
            if (omdl_index_count > 0) fwrite(omdl_indices, sizeof(uint32_t), omdl_index_count, fout);
            if (omdl_morph_count > 0) fwrite(omdl_morphs, sizeof(MorphDelta), omdl_morph_count, fout);
            fclose(fout);
        }

        if (omdl_vertices) free(omdl_vertices);
        if (omdl_indices) free(omdl_indices);
        if (omdl_morphs) free(omdl_morphs);
        if (omdl_metas) free(omdl_metas);
    } else {
        if (omdl_cache_metas) free(omdl_cache_metas);
        omdl_cache_metas = NULL;
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

    // AAA: Map glTF skins to our Global Joint SSBO
    if (data->skins_count > 0) {
        int* skin_offsets = malloc(data->skins_count * sizeof(int));
        for (size_t s = 0; s < data->skins_count; s++) {
            skin_offsets[s] = global_joint_counter;
            global_joint_counter += data->skins[s].joints_count;
        }

        for (size_t i = instance->mesh_start_index; i < instance->mesh_start_index + instance->mesh_count; i++) {
            Mesh* m = &scene->meshes.items[i];
            cgltf_node* cnode = (cgltf_node*)m->node;
            if (cnode && cnode->skin) {
                size_t skin_idx = cnode->skin - data->skins;
                m->jointOffset = skin_offsets[skin_idx];
                m->jointCount = cnode->skin->joints_count;
            }
        }
        free(skin_offsets);
    }

    load_gltf_animations(data, instance);

    scene->gltf_instance_count++;

    printf("Loaded glTF instance #%zu with %zu meshes (indices %zu to %zu)\n",
           scene->gltf_instance_count - 1,
           instance->mesh_count,
           instance->mesh_start_index,
           instance->mesh_start_index + instance->mesh_count - 1);

    /* Rebuild indirect draw commands after every GLTF load so the
       indirect pass stays in sync with the current mesh list.      */
    scene_topology_dirty = true;

    /* CRITICAL FIX: Guarantee static meshes without animations push their
       initial transforms and PBR materials to the GPU SSBO. */
    markMeshesSSBODirty(&context);

    // Do NOT free cgltf_data here! The runtime animation/hierarchy system holds
    // pointers to it. It must stay alive for the lifetime of the scene.
    return true;
}

static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

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

#define MAX_NODES 16384
static mat4 node_transform_scratch[MAX_NODES];

// AAA O(N) Single-Pass Top-Down Hierarchy Evaluator
static void compute_node_hierarchy(cgltf_node* node, Animation* anim, float anim_time, mat4 parent_mat, cgltf_data* data) {
    mat4 local;
    bool is_animated = false;

    // Standard stack variables (compiler aligns these naturally)
    vec3 T = {0.0f, 0.0f, 0.0f};
    versor R = {0.0f, 0.0f, 0.0f, 1.0f};
    vec3 S = {1.0f, 1.0f, 1.0f};

    // Shielding cglm from unaligned heap memory via scalar assignment
    if (node->has_translation) { T[0] = node->translation[0]; T[1] = node->translation[1]; T[2] = node->translation[2]; }
    if (node->has_rotation) { R[0] = node->rotation[0]; R[1] = node->rotation[1]; R[2] = node->rotation[2]; R[3] = node->rotation[3]; }
    if (node->has_scale) { S[0] = node->scale[0]; S[1] = node->scale[1]; S[2] = node->scale[2]; }

    if (anim) {
        for (size_t c = 0; c < anim->channel_count; c++) {
            AnimationChannel* chan = &anim->channels[c];
            if (chan->target_node == node && chan->path != cgltf_animation_path_type_weights) {
                is_animated = true;
                size_t k0 = find_keyframe(chan->times, chan->keyframe_count, anim_time);
                size_t k1 = chan->keyframe_count > 1 ? k0 + 1 : k0;

                // CRITICAL: Clamp factor to prevent infinite extrapolation!
                float factor = 0.0f;
                if (chan->keyframe_count > 1) {
                    float t_diff = chan->times[k1] - chan->times[k0];
                    if (t_diff > 0.00001f) {
                        factor = (anim_time - chan->times[k0]) / t_diff;
                        if (factor < 0.0f) factor = 0.0f;
                        if (factor > 1.0f) factor = 1.0f;
                    }
                }

                // Shielding cglm again by pulling heap arrays into stack variables first
                if (chan->path == cgltf_animation_path_type_translation && chan->translations) {
                    vec3 t0 = {chan->translations[k0][0], chan->translations[k0][1], chan->translations[k0][2]};
                    vec3 t1 = {chan->translations[k1][0], chan->translations[k1][1], chan->translations[k1][2]};
                    glm_vec3_lerp(t0, t1, factor, T);
                } else if (chan->path == cgltf_animation_path_type_rotation && chan->rotations) {
                    versor r0 = {chan->rotations[k0][0], chan->rotations[k0][1], chan->rotations[k0][2], chan->rotations[k0][3]};
                    versor r1 = {chan->rotations[k1][0], chan->rotations[k1][1], chan->rotations[k1][2], chan->rotations[k1][3]};
                    glm_quat_slerp(r0, r1, factor, R);
                } else if (chan->path == cgltf_animation_path_type_scale && chan->scales) {
                    vec3 s0 = {chan->scales[k0][0], chan->scales[k0][1], chan->scales[k0][2]};
                    vec3 s1 = {chan->scales[k1][0], chan->scales[k1][1], chan->scales[k1][2]};
                    glm_vec3_lerp(s0, s1, factor, S);
                }
            }
        }
    }

    if (is_animated) {
        mat4 rot_mat;
        glm_quat_mat4(R, rot_mat);
        glm_mat4_identity(local);
        glm_translate(local, T);
        glm_mat4_mul(local, rot_mat, local);
        glm_scale(local, S);
    } else {
        cgltf_node_transform_local(node, (float*)local);
    }

    // Resolve hierarchy against parent
    mat4 world;
    glm_mat4_mul(parent_mat, local, world);

    // Cache the resolved world matrix
    size_t node_idx = node - data->nodes;
    if (node_idx < MAX_NODES) {
        memcpy(node_transform_scratch[node_idx], world, sizeof(mat4));
    }

    // Recurse children exactly once
    for (size_t i = 0; i < node->children_count; i++) {
        compute_node_hierarchy(node->children[i], anim, anim_time, world, data);
    }
}

void animate_scene(Scene* scene, float time) {
    // Animate each glTF instance independently
    for (size_t inst = 0; inst < scene->gltf_instance_count; inst++) {
        GLTFInstance* instance = &scene->gltf_instances[inst];
        if (!instance->animations) continue;

        for (size_t a = 0; a < instance->animation_count; a++) {
            Animation* anim = &instance->animations[a];
            float anim_time = fmodf(time, anim->duration);

            // Phase 1: Evaluate morph weights (with clamping)
            for (size_t c = 0; c < anim->channel_count; c++) {
                AnimationChannel* channel = &anim->channels[c];
                if (channel->path == cgltf_animation_path_type_weights && channel->weights) {
                    cgltf_node* target_node = channel->target_node;
                    size_t k0 = find_keyframe(channel->times, channel->keyframe_count, anim_time);
                    size_t k1 = channel->keyframe_count > 1 ? k0 + 1 : k0;

                    float factor = 0.0f;
                    if (channel->keyframe_count > 1) {
                        float t_diff = channel->times[k1] - channel->times[k0];
                        if (t_diff > 0.00001f) {
                            factor = (anim_time - channel->times[k0]) / t_diff;
                            if (factor < 0.0f) factor = 0.0f;
                            if (factor > 1.0f) factor = 1.0f;
                        }
                    }

                    size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
                    for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                        Mesh* mesh = &scene->meshes.items[m];
                        if (mesh->node == target_node && mesh->morph_data) {
                            size_t num_targets = mesh->morph_data->target_count;
                            for (size_t t = 0; t < num_targets; t++) {
                                float w0 = channel->weights[k0 * num_targets + t];
                                float w1 = channel->weights[k1 * num_targets + t];
                                mesh->morph_data->weights[t] = lerp(w0, w1, factor);
                            }
                        }
                    }
                }
            }

            // Phase 2: Top-Down Single Pass Hierarchy Evaluation
            cgltf_scene* active_scene = instance->gltf_data->scene ? instance->gltf_data->scene : &instance->gltf_data->scenes[0];
            mat4 identity;
            glm_mat4_identity(identity);

            for (size_t i = 0; i < active_scene->nodes_count; i++) {
                compute_node_hierarchy(active_scene->nodes[i], anim, anim_time, identity, instance->gltf_data);
            }

            // Phase 3: Fast linear sweep to apply cached transforms to meshes
            size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
            for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                Mesh* mesh = &scene->meshes.items[m];
                if (mesh->node) {
                    // CRITICAL: Cast the opaque void* back to cgltf_node* so C can do pointer math!
                    size_t node_idx = (cgltf_node*)mesh->node - instance->gltf_data->nodes;
                    if (node_idx < MAX_NODES) {
                        memcpy(mesh->model, node_transform_scratch[node_idx], sizeof(mat4));
                        markMeshDirty(&context, (uint32_t)m);
                    }
                } else if (mesh->morph_data) {
                    markMeshDirty(&context, (uint32_t)m);
                }
            }

            // Phase 4: Hardware Skinning (Evaluate and upload joint matrices to the SSBO)
            mat4* joint_buffer = jointSSBOMapped[context.currentFrame];
            if (joint_buffer && instance->gltf_data->skins_count > 0) {
                for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                    Mesh* mesh = &scene->meshes.items[m];
                    cgltf_node* cnode = (cgltf_node*)mesh->node;

                    if (cnode && cnode->skin && mesh->jointOffset >= 0) {
                        cgltf_skin* skin = cnode->skin;
                        mat4 inverse_mesh_world;
                        glm_mat4_inv(mesh->model, inverse_mesh_world);

                        for (size_t j = 0; j < skin->joints_count; j++) {
                            cgltf_node* joint_node = skin->joints[j];

                            mat4 inverse_bind;
                            glm_mat4_identity(inverse_bind);
                            if (skin->inverse_bind_matrices) {
                                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, (float*)inverse_bind, 16);
                            }

                            size_t joint_idx = joint_node - instance->gltf_data->nodes;
                            mat4 joint_world;
                            if (joint_idx < MAX_NODES) {
                                memcpy(joint_world, node_transform_scratch[joint_idx], sizeof(mat4));
                            } else {
                                glm_mat4_identity(joint_world);
                            }

                            // The glTF 2.0 Spec Formula: Final = Inverse(MeshWorld) * JointWorld * InverseBind
                            mat4 final_joint;
                            glm_mat4_mul(inverse_mesh_world, joint_world, final_joint);
                            glm_mat4_mul(final_joint, inverse_bind, final_joint);

                            // AAA 4x3 Packing: cglm is column-major. We transpose the top 3 rows into 3 vec4s.
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
}
