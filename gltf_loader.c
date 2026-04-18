#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include <string.h>
#include "context.h"
#include "vulkan_setup.h"
#include <sys/stat.h>
#include <sys/types.h>

static int32_t gltf_texture_indices[MAX_TEXTURES];
static FILE* geom_cache_in = NULL;
static FILE* geom_cache_out = NULL;
static size_t gltf_texture_count = 0;

typedef struct {
    cgltf_node* node;
    size_t mesh_start_index;
    size_t mesh_count;
} NodeMeshMapping;

static NodeMeshMapping node_mappings[256];
static size_t node_mapping_count = 0;

static int global_joint_counter = 0;
extern mat4* jointSSBOMapped[MAX_FRAMES_IN_FLIGHT];

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
    if (data->textures_count == 0) {
        printf("No textures in glTF file\n");
        return true;
    }

    char dir[512];
    get_directory(base_path, dir, sizeof(dir));

    printf("Loading %zu textures from glTF...\n", data->textures_count);
    gltf_texture_count = 0;
    memset(gltf_texture_indices, -1, sizeof(gltf_texture_indices));

    for (size_t i = 0; i < data->textures_count; i++) {
        cgltf_texture* tex = &data->textures[i];

        if (!tex->image) {
            printf("  Texture %zu: No image data\n", i);
            gltf_texture_indices[i] = -1;
            continue;
        }

        cgltf_image* img = tex->image;
        int32_t tex_id = -1;

        // Case 1: External texture file (typical in .gltf)
        if (img->uri && !strstr(img->uri, "data:")) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s%s", dir, img->uri);
            printf("  Texture %zu: Loading from file '%s'\n", i, full_path);

            tex_id = texture_pool_add(&context, full_path);
        }
        // Case 2: Embedded texture data (typical in .glb)
        else if (img->buffer_view) {
            printf("  Texture %zu: Loading from embedded buffer\n", i);

            cgltf_buffer_view* view = img->buffer_view;
            unsigned char* buffer_data = (unsigned char*)view->buffer->data + view->offset;
            size_t buffer_size = view->size;

            // Load texture from memory buffer
            tex_id = texture_pool_add_from_memory(buffer_data, buffer_size);
        }
        // Case 3: Data URI embedded in .gltf file
        else if (img->uri && strstr(img->uri, "data:")) {
            printf("  Texture %zu: Data URI not yet supported\n", i);
            gltf_texture_indices[i] = -1;
            continue;
        }
        else {
            printf("  Texture %zu: Unknown image format\n", i);
            gltf_texture_indices[i] = -1;
            continue;
        }

        if (tex_id < 0) {
            fprintf(stderr, "  Failed to load texture %zu\n", i);
            gltf_texture_indices[i] = -1;
            continue;
        }

        gltf_texture_indices[i] = tex_id;
        gltf_texture_count = i + 1;
        printf("  -> Loaded as texture #%d\n", tex_id);
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
    mesh.node = NULL;
    mesh.morph_data = NULL;
    mesh.is_unlit = false;
    mesh.alpha_mode = 0; // OPAQUE by default
    mesh.alpha_cutoff = 0.5f; // Default cutoff
    mesh.jointOffset = -1; // CRITICAL FIX: Disable skinning by default to prevent GPU page faults!
    mesh.jointCount = 0;
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);

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

    uint32_t final_vertex_count = 0;
    uint32_t final_index_count = 0;
    uint32_t final_morph_count = 0;

    mesh.megaBaseVertex     = UINT32_MAX;
    mesh.megaBaseIndex      = UINT32_MAX;
    mesh.dynamicBaseVertex  = UINT32_MAX;

    bool loaded_from_cache = false;
    if (geom_cache_in) {
        fread(&final_vertex_count, sizeof(uint32_t), 1, geom_cache_in);
        fread(&final_index_count, sizeof(uint32_t), 1, geom_cache_in);
        fread(&final_morph_count, sizeof(uint32_t), 1, geom_cache_in);
        fread(mesh.aabbMin, sizeof(vec3), 1, geom_cache_in);
        fread(mesh.aabbMax, sizeof(vec3), 1, geom_cache_in);

        // AAA Zero-Copy NVMe to GPU Staging Buffer
        if (final_vertex_count > 0) {
            mesh.megaBaseVertex = megaBufferAllocateFromFile(&context, geom_cache_in, final_vertex_count);
        }
        if (final_index_count > 0) {
            mesh.megaBaseIndex = megaIndexBufferAllocateFromFile(&context, geom_cache_in, final_index_count);
        }
        if (final_morph_count > 0) {
            uint32_t delta_offset = megaMorphBufferAllocateFromFile(&context, geom_cache_in, final_vertex_count * final_morph_count);
            if (delta_offset != UINT32_MAX) {
                mesh.morph_data = malloc(sizeof(MorphData));
                mesh.morph_data->target_count = final_morph_count;
                mesh.morph_data->weights = calloc(final_morph_count, sizeof(float));
                mesh.morphDeltaOffset = (int)delta_offset;
                mesh.morphCount = (int)final_morph_count;
                mesh.dynamicBaseVertex = UINT32_MAX;
            }
        }
        loaded_from_cache = true;
    }

    if (!loaded_from_cache) {
        Vertex* final_vertices = NULL;
        uint32_t* final_indices = NULL;
        MorphDelta* final_morphs = NULL;
        final_vertex_count = (uint32_t)vertex_count;
        final_index_count = (uint32_t)index_count;
        final_morph_count = (uint32_t)prim->targets_count;

        final_vertices = malloc(final_vertex_count * sizeof(Vertex));
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

        if (geom_cache_out) {
            fwrite(&final_vertex_count, sizeof(uint32_t), 1, geom_cache_out);
            fwrite(&final_index_count, sizeof(uint32_t), 1, geom_cache_out);
            fwrite(&final_morph_count, sizeof(uint32_t), 1, geom_cache_out);
            fwrite(mesh.aabbMin, sizeof(vec3), 1, geom_cache_out);
            fwrite(mesh.aabbMax, sizeof(vec3), 1, geom_cache_out);

            if (final_vertex_count > 0) fwrite(final_vertices, sizeof(Vertex), final_vertex_count, geom_cache_out);
            if (final_index_count > 0) fwrite(final_indices, sizeof(uint32_t), final_index_count, geom_cache_out);
            if (final_morph_count > 0) {
                    size_t total_deltas = (size_t)final_vertex_count * final_morph_count;
                    fwrite(final_morphs, sizeof(MorphDelta), total_deltas, geom_cache_out);
                }
            }

            if (final_morph_count > 0 && final_morphs) {
                uint32_t delta_offset = megaMorphBufferAllocate(&context, final_morphs, final_vertex_count * final_morph_count);
                if (delta_offset != UINT32_MAX) {
                    mesh.morph_data = malloc(sizeof(MorphData));
                    mesh.morph_data->target_count = final_morph_count;
                    mesh.morph_data->weights = calloc(final_morph_count, sizeof(float));
                    mesh.morphDeltaOffset = (int)delta_offset;
                    mesh.morphCount = (int)final_morph_count;
                }
                free(final_morphs);
            }

            mesh.megaBaseVertex = megaBufferAllocate(&context, final_vertices, final_vertex_count);
            if (final_indices && final_index_count > 0) {
                mesh.megaBaseIndex = megaIndexBufferAllocate(&context, final_indices, final_index_count);
            }

            if (mesh.morph_data) mesh.dynamicBaseVertex = UINT32_MAX;
            if (final_indices) free(final_indices);
            if (final_vertices) free(final_vertices);
        }

        mesh.vertexCount = final_vertex_count;

    mesh.indexCount  = final_index_count;

    // Load texture if present
    if (prim->material && prim->material->has_pbr_metallic_roughness) {
        cgltf_pbr_metallic_roughness* pbr = &prim->material->pbr_metallic_roughness;
        if (pbr->base_color_texture.texture) {
            cgltf_texture* base_texture = pbr->base_color_texture.texture;
            for (size_t t = 0; t < data->textures_count; t++) {
                if (&data->textures[t] == base_texture) {
                    if (t < gltf_texture_count && gltf_texture_indices[t] >= 0) {
                        mesh.textureIndex = gltf_texture_indices[t];
                        mesh.texture = texture_pool_get(mesh.textureIndex);
                        printf("  -> Using texture pool #%d\n", mesh.textureIndex);
                    }
                    break;
                }
            }
        }
    }

    printf("Loaded mesh '%s' with %zu vertices", mesh.name, (size_t)final_vertex_count);

    if (is_unlit) {
        printf(" (UNLIT)");
    }
    switch (mesh.alpha_mode) {
        case 1:
            printf(" (ALPHA_MASK cutoff=%.2f)", mesh.alpha_cutoff);
            break;
        case 2:
            printf(" (ALPHA_BLEND)");
            break;
    }
    printf("\n");

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
    cgltf_options options = {0};
    cgltf_data* data = NULL;

    FILE* test = fopen(filepath, "r");
    if (!test) {
        printf("Cannot open file '%s'\n", filepath);
        return false;
    }
    fclose(test);

static char cache_path[512];
    const char* home = getenv("HOME");
    if (!home) home = ".";
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.cache", home);
    mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian", home);
    mkdir(dir_path, 0777);
    snprintf(dir_path, sizeof(dir_path), "%s/.cache/obsidian/geometry", home);
    mkdir(dir_path, 0777);

    char safe_name[256];
    strncpy(safe_name, filepath, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';
    for (int i = 0; safe_name[i]; i++) {
        if (safe_name[i] == '/' || safe_name[i] == '\\' || safe_name[i] == '.') safe_name[i] = '_';
    }
    snprintf(cache_path, sizeof(cache_path), "%s/%s.ogeom", dir_path, safe_name);

    geom_cache_in = fopen(cache_path, "rb");
    geom_cache_out = NULL;
    if (geom_cache_in) {
        fprintf(stdout, "\033[32m[GEOMETRY] Cache Hit: Loading pre-processed mesh data from %s\033[0m\n", cache_path);
    } else {
        fprintf(stdout, "\033[33m[GEOMETRY] Cache Miss: Extracting attributes and building %s\033[0m\n", cache_path);
        geom_cache_out = fopen(cache_path, "wb");
    }

    printf("Parsing glTF file: %s\n", filepath);

    cgltf_result result = cgltf_parse_file(&options, filepath, &data);
    if (result != cgltf_result_success) {
        printf("Failed to parse GLTF file '%s': %d\n", filepath, result);
        return false;
    }

    printf("Successfully parsed GLTF: %s\n", filepath);
    printf("  Scenes: %zu\n", data->scenes_count);
    printf("  Nodes: %zu\n", data->nodes_count);
    printf("  Meshes: %zu\n", data->meshes_count);
    printf("  Materials: %zu\n", data->materials_count);
    printf("  Textures: %zu\n", data->textures_count);
    printf("  Animations: %zu\n", data->animations_count);
    printf("\n");

    result = cgltf_load_buffers(&options, data, filepath);
    if (result != cgltf_result_success) {
        printf("Failed to load buffers: %d\n", result);
        cgltf_free(data);
        return false;
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

    if (!load_gltf_textures(data, filepath)) {
        printf("Warning: Failed to load some textures\n");
    }

    /* create one large staging buffer for the entire model upload —
       all mesh vertex/index data is batched and transferred in one GPU command */
    createUploadStagingBuffer(&context, 256 * 1024 * 1024); /* 256 MB upload window */

    load_gltf_meshes(data, &scene->meshes);

    /* flush all accumulated vertex+index copies in a single command buffer */
    flushUploadStagingBuffer(&context);
    destroyUploadStagingBuffer(&context);

    instance->mesh_count = scene->meshes.count - instance->mesh_start_index;

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
    updateMeshSSBOAndIndirect(&context, &scene->meshes);

    if (geom_cache_in) { fclose(geom_cache_in); geom_cache_in = NULL; }
    if (geom_cache_out) { fclose(geom_cache_out); geom_cache_out = NULL; }

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
