#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include <string.h>
#include "context.h"
#include "vulkan_setup.h"

static int32_t gltf_texture_indices[MAX_TEXTURES];
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

// Load morph target data from a primitive
static MorphData* load_morph_targets(cgltf_primitive* prim) {
    if (prim->targets_count == 0) {
        return NULL;
    }

    printf("  Loading %zu morph targets\n", prim->targets_count);

    MorphData* morph_data = malloc(sizeof(MorphData));
    morph_data->target_count = prim->targets_count;
    morph_data->targets = malloc(prim->targets_count * sizeof(MorphTarget));
    morph_data->weights = calloc(prim->targets_count, sizeof(float));

    for (size_t t = 0; t < prim->targets_count; t++) {
        cgltf_morph_target* target = &prim->targets[t];
        MorphTarget* morph_target = &morph_data->targets[t];

        morph_target->positions = NULL;
        morph_target->normals = NULL;
        morph_target->vertex_count = 0;

        // Find position and normal delta attributes
        for (size_t a = 0; a < target->attributes_count; a++) {
            cgltf_attribute* attr = &target->attributes[a];

            if (attr->type == cgltf_attribute_type_position) {
                size_t count = attr->data->count;
                morph_target->vertex_count = count;
                morph_target->positions = malloc(count * sizeof(vec3));

                for (size_t v = 0; v < count; v++) {
                    cgltf_accessor_read_float(attr->data, v, morph_target->positions[v], 3);
                }
                printf("    Target %zu: %zu position deltas\n", t, count);
            } else if (attr->type == cgltf_attribute_type_normal) {
                size_t count = attr->data->count;
                morph_target->normals = malloc(count * sizeof(vec3));

                for (size_t v = 0; v < count; v++) {
                    cgltf_accessor_read_float(attr->data, v, morph_target->normals[v], 3);
                }
                printf("    Target %zu: %zu normal deltas\n", t, count);
            }
        }

        if (!morph_target->normals) {
            printf("    Target %zu: WARNING - No normal deltas provided!\n", t);
        }
    }

    return morph_data;
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

    // Create vertex array with material colors
    Vertex* vertices = malloc(vertex_count * sizeof(Vertex));

    for (size_t v = 0; v < vertex_count; v++) {
        // Position
        if (pos_accessor) {
            cgltf_accessor_read_float(pos_accessor, v, vertices[v].pos, 3);
        }

        // Normal
        if (normal_accessor) {
            cgltf_accessor_read_float(normal_accessor, v, vertices[v].normal, 3);
        } else {
            vertices[v].normal[0] = 0.0f;
            vertices[v].normal[1] = 1.0f;
            vertices[v].normal[2] = 0.0f;
        }

        // Texture coordinates
        if (texcoord_accessor) {
            cgltf_accessor_read_float(texcoord_accessor, v, vertices[v].texCoord, 2);
        } else {
            vertices[v].texCoord[0] = 0.0f;
            vertices[v].texCoord[1] = 0.0f;
        }

        // Tangent (vec4: xyz=tangent, w=bitangent sign)
        if (tangent_accessor) {
            cgltf_accessor_read_float(tangent_accessor, v, vertices[v].tangent, 4);
        } else {
            // Fallback tangent — will look wrong for normal maps but won't crash
            vertices[v].tangent[0] = 1.0f;
            vertices[v].tangent[1] = 0.0f;
            vertices[v].tangent[2] = 0.0f;
            vertices[v].tangent[3] = 1.0f;
        }

        // Color - prioritize vertex colors, fallback to material base color
        if (color_accessor) {
            float color[4];
            cgltf_accessor_read_float(color_accessor, v, color, 4);
            vertices[v].color[0] = color[0];
            vertices[v].color[1] = color[1];
            vertices[v].color[2] = color[2];
            vertices[v].color[3] = color[3];
        } else {
            // Use material base color
            vertices[v].color[0] = base_color[0];
            vertices[v].color[1] = base_color[1];
            vertices[v].color[2] = base_color[2];
            vertices[v].color[3] = base_color[3];
        }

        vertices[v].textureIndex = 0;

        // AAA: Parse Joints (cgltf handles the 8/16/32-bit type conversions automatically)
        if (joints_accessor) {
            uint32_t j[4] = {0,0,0,0};
            cgltf_accessor_read_uint(joints_accessor, v, j, 4);
            vertices[v].joints[0] = j[0]; vertices[v].joints[1] = j[1];
            vertices[v].joints[2] = j[2]; vertices[v].joints[3] = j[3];
        } else {
            vertices[v].joints[0] = 0; vertices[v].joints[1] = 0;
            vertices[v].joints[2] = 0; vertices[v].joints[3] = 0;
        }

        // AAA: Parse Bone Weights
        if (weights_accessor) {
            float w[4] = {0,0,0,0};
            cgltf_accessor_read_float(weights_accessor, v, w, 4);
            vertices[v].weights[0] = w[0]; vertices[v].weights[1] = w[1];
            vertices[v].weights[2] = w[2]; vertices[v].weights[3] = w[3];
        } else {
            vertices[v].weights[0] = 0.0f; vertices[v].weights[1] = 0.0f;
            vertices[v].weights[2] = 0.0f; vertices[v].weights[3] = 0.0f;
        }
    }

    // Load morph targets BEFORE expanding indices
    mesh.morph_data = load_morph_targets(prim);

    /* For static meshes: keep vertices and indices separate — no expansion.
       For morph-target meshes: expansion is still required so the CPU can
       remap per-frame without an index fetch.                               */
    Vertex*   final_vertices    = NULL;
    uint32_t* final_indices     = NULL;
    size_t    final_vertex_count = 0;
    size_t    final_index_count  = 0;

    if (mesh.morph_data) {
        /* morph path: expand as before so mesh_update_morph works */
        if (indices) {
            final_vertex_count = index_count;
            final_vertices = malloc(final_vertex_count * sizeof(Vertex));
            mesh.morph_data->index_map = malloc(index_count * sizeof(uint32_t));

            for (size_t i = 0; i < index_count; i++) {
                final_vertices[i] = vertices[indices[i]];
                mesh.morph_data->index_map[i] = indices[i];
            }
            mesh.morph_data->base_vertices = malloc(vertex_count * sizeof(Vertex));
            memcpy(mesh.morph_data->base_vertices, vertices, vertex_count * sizeof(Vertex));
            mesh.morph_data->base_vertex_count = vertex_count;

            free(vertices);
            free(indices);
            indices = NULL;
        } else {
            final_vertices     = vertices;
            final_vertex_count = vertex_count;
            mesh.morph_data->base_vertices = malloc(vertex_count * sizeof(Vertex));
            memcpy(mesh.morph_data->base_vertices, vertices, vertex_count * sizeof(Vertex));
            mesh.morph_data->base_vertex_count = vertex_count;
            mesh.morph_data->index_map = NULL;
        }
        final_index_count = 0; /* morph meshes are non-indexed draws */
        final_indices     = NULL;
    } else {
        /* static path: keep raw arrays, no expansion */
        final_vertices     = vertices;
        final_vertex_count = vertex_count;
        final_indices      = indices;   /* may be NULL for non-indexed primitives */
        final_index_count  = index_count;
        indices = NULL; /* ownership transferred */
    }

    mesh.vertexCount = (uint32_t)(mesh.morph_data ? final_vertex_count : final_vertex_count);
    mesh.indexCount  = (uint32_t)final_index_count;

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

    /* ── compute local-space AABB directly from position accessor ────── */
    /* Use the raw accessor before any expansion — always correct,
       works for both indexed and non-indexed, morph and static.          */
    {
        vec3 bmin = { 1e30f,  1e30f,  1e30f};
        vec3 bmax = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v < vertex_count; v++) {
            float p[3];
            cgltf_accessor_read_float(pos_accessor, v, p, 3);
            bmin[0] = fminf(bmin[0], p[0]);
            bmin[1] = fminf(bmin[1], p[1]);
            bmin[2] = fminf(bmin[2], p[2]);
            bmax[0] = fmaxf(bmax[0], p[0]);
            bmax[1] = fmaxf(bmax[1], p[1]);
            bmax[2] = fmaxf(bmax[2], p[2]);
        }

        glm_vec3_copy(bmin, mesh.aabbMin);
        glm_vec3_copy(bmax, mesh.aabbMax);
    }

    mesh.megaBaseVertex     = UINT32_MAX;
    mesh.megaBaseIndex      = UINT32_MAX;
    mesh.dynamicBaseVertex  = UINT32_MAX;

    if (mesh.morph_data) {
        /* dynamic mesh: uses linear indices and gets appended to dynamic staging buffer every frame */
        mesh.megaBaseVertex = UINT32_MAX;
        mesh.megaBaseIndex  = 0;
        final_index_count   = final_vertex_count;
        mesh.dynamicBaseVertex = append_vertices(final_vertices, final_vertex_count);
    } else {
        /* static mesh: vertices into mega vertex buffer, indices into mega index buffer */
        mesh.megaBaseVertex = megaBufferAllocate(&context,
                                                 final_vertices,
                                                 (uint32_t)final_vertex_count);
        if (final_indices && final_index_count > 0) {
            mesh.megaBaseIndex = megaIndexBufferAllocate(&context,
                                                         final_indices,
                                                         (uint32_t)final_index_count);
        }
        free(final_indices);
    }

    free(final_vertices);

    printf("Loaded mesh '%s' with %zu vertices", mesh.name, final_vertex_count);
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
                    }
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

    // Guarantee that ALL dynamic meshes (even un-animated ones) get their
    // geometry appended to the current frame's dynamic staging buffer.
    VkDeviceSize drawSize = (16384 + 4096) * sizeof(VkDrawIndexedIndirectCommand);
    VkDrawIndexedIndirectCommand* cmds = (VkDrawIndexedIndirectCommand*)((uint8_t*)context.srcIndirectBufferMapped + (context.currentFrame * drawSize));

    for (size_t i = 0; i < scene->meshes.count; i++) {
        Mesh* mesh = &scene->meshes.items[i];
        if (mesh->morph_data) {
            mesh_update_morph(mesh);

            // Sync the indirect draw command for this dynamic mesh so it actually renders!
            // Morph targets have indexCount = 0, so we use our linear index buffer starting from 0.
            cmds[i].indexCount = mesh->vertexCount;
            cmds[i].instanceCount = 1;
            cmds[i].firstIndex = 0;
            cmds[i].vertexOffset = context.megaVertexBufferOffset + (context.currentFrame * MAX_DYNAMIC_VERTICES) + mesh->dynamicBaseVertex;
            cmds[i].firstInstance = i; // CRITICAL: Ensures SSBO material matches!
        }
    }

    // CRITICAL: We dynamically recalculated the AABBs for the morph targets on the CPU,
    // and updated the hierarchical matrices.
    // We MUST flag the SSBO as dirty every frame so the GPU gets the new data!
    markMeshesSSBODirty(&context);
}
