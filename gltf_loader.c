#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include <string.h>
#include "context.h"


static int32_t gltf_texture_indices[MAX_TEXTURES];
static size_t gltf_texture_count = 0;

// Store node-to-mesh mapping for animation
typedef struct {
    cgltf_node* node;
    size_t mesh_start_index;  // First mesh from this node
    size_t mesh_count;         // Number of meshes from this node
} NodeMeshMapping;

static NodeMeshMapping node_mappings[256];
static size_t node_mapping_count = 0;

// Helper to get directory from filepath
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

// Load textures from glTF
bool load_gltf_textures(cgltf_data* data, const char* base_path) {
    if (data->textures_count == 0) {
        printf("No textures in glTF file\n");
        return true;
    }

    char dir[512];
    get_directory(base_path, dir, sizeof(dir));

    printf("Loading %zu textures from glTF...\n", data->textures_count);
    gltf_texture_count = 0;

    for (size_t i = 0; i < data->textures_count; i++) {
        cgltf_texture* tex = &data->textures[i];
        
        if (!tex->image || !tex->image->uri) {
            printf("  Texture %zu: No image URI\n", i);
            gltf_texture_indices[i] = -1;
            continue;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s%s", dir, tex->image->uri);

        printf("  Texture %zu: Loading '%s'\n", i, full_path);

        int32_t tex_id = texture_pool_add(&context, full_path);
        if (tex_id < 0) {
            fprintf(stderr, "  Failed to load texture: %s\n", full_path);
            gltf_texture_indices[i] = -1;
            continue;
        }

        gltf_texture_indices[i] = tex_id;
        gltf_texture_count = i + 1;
        
        printf("  -> Loaded as texture #%d\n", tex_id);
    }

    return true;
}

// Process a single primitive with indices
static Mesh create_mesh_from_primitive(cgltf_primitive* prim, cgltf_data* data, const char* name) {
    Mesh mesh = {0};
    mesh.name = strdup(name);
    mesh.textureIndex = -1;
    mesh.texture = NULL;
    mesh.node = NULL;  // Will be set by caller
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);

    // Get accessors
    cgltf_accessor* pos_accessor = NULL;
    cgltf_accessor* normal_accessor = NULL;
    cgltf_accessor* texcoord_accessor = NULL;
    cgltf_accessor* color_accessor = NULL;

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
            default:
                break;
        }
    }

    if (!pos_accessor) {
        printf("  Primitive has no position data\n");
        return mesh;
    }

    size_t vertex_count = pos_accessor->count;
    
    // Check for indices
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

    // Allocate vertices
    Vertex* vertices = malloc(vertex_count * sizeof(Vertex));

    // Read vertex data
    for (size_t v = 0; v < vertex_count; v++) {
        if (pos_accessor) {
            cgltf_accessor_read_float(pos_accessor, v, vertices[v].pos, 3);
        }

        if (normal_accessor) {
            cgltf_accessor_read_float(normal_accessor, v, vertices[v].normal, 3);
        } else {
            vertices[v].normal[0] = 0.0f;
            vertices[v].normal[1] = 1.0f;
            vertices[v].normal[2] = 0.0f;
        }

        if (texcoord_accessor) {
            cgltf_accessor_read_float(texcoord_accessor, v, vertices[v].texCoord, 2);
        } else {
            vertices[v].texCoord[0] = 0.0f;
            vertices[v].texCoord[1] = 0.0f;
        }

        if (color_accessor) {
            float color[4];
            cgltf_accessor_read_float(color_accessor, v, color, 4);
            vertices[v].color[0] = color[0];
            vertices[v].color[1] = color[1];
            vertices[v].color[2] = color[2];
            vertices[v].color[3] = color[3];
        } else {
            vertices[v].color[0] = 1.0f;
            vertices[v].color[1] = 1.0f;
            vertices[v].color[2] = 1.0f;
            vertices[v].color[3] = 1.0f;
        }

        vertices[v].textureIndex = 0;
    }

    // Expand vertices if indexed
    Vertex* final_vertices = NULL;
    size_t final_vertex_count = 0;
    
    if (indices) {
        final_vertex_count = index_count;
        final_vertices = malloc(final_vertex_count * sizeof(Vertex));
        
        for (size_t i = 0; i < index_count; i++) {
            final_vertices[i] = vertices[indices[i]];
        }
        
        free(vertices);
        free(indices);
    } else {
        final_vertices = vertices;
        final_vertex_count = vertex_count;
    }

    mesh.vertexCount = final_vertex_count;

    // Get texture from material
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

    // Create Vulkan buffers
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = final_vertex_count * sizeof(Vertex),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(context.device, &bufferInfo, NULL, &mesh.vertexBuffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context.device, mesh.vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(
            context.physicalDevice, 
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        )
    };

    vkAllocateMemory(context.device, &allocInfo, NULL, &mesh.vertexBufferMemory);
    vkBindBufferMemory(context.device, mesh.vertexBuffer, mesh.vertexBufferMemory, 0);

    // Upload vertex data
    void* data_ptr;
    vkMapMemory(context.device, mesh.vertexBufferMemory, 0, bufferInfo.size, 0, &data_ptr);
    memcpy(data_ptr, final_vertices, final_vertex_count * sizeof(Vertex));
    vkUnmapMemory(context.device, mesh.vertexBufferMemory);

    free(final_vertices);

    printf("Loaded mesh '%s' with %zu vertices\n", mesh.name, final_vertex_count);
    return mesh;
}

// Process node hierarchy recursively
static void process_node(cgltf_node* node, cgltf_data* data, Meshes* meshes, mat4 parent_transform) {
    mat4 local_transform;
    mat4 world_transform;
    
    // Get local transform
    cgltf_node_transform_local(node, (float*)local_transform);
    
    // Combine with parent
    glm_mat4_mul(parent_transform, local_transform, world_transform);
    
    // Track where this node's meshes start
    size_t mesh_start = meshes->count;
    
    // Process mesh if this node has one
    if (node->mesh) {
        cgltf_mesh* gltf_mesh = node->mesh;
        
        for (size_t i = 0; i < gltf_mesh->primitives_count; i++) {
            char mesh_name[256];
            snprintf(mesh_name, sizeof(mesh_name), "%s_prim_%zu", 
                     node->name ? node->name : "node", i);
            
            Mesh mesh = create_mesh_from_primitive(&gltf_mesh->primitives[i], data, mesh_name);
            
            if (mesh.vertexCount > 0) {
                // Store node reference and transforms
                mesh.node = node;
                glm_mat4_copy(world_transform, mesh.model);
                glm_mat4_copy(local_transform, mesh.local_transform);
                meshes_add(meshes, mesh);
            }
        }
    }
    
    // Store node mapping for animation
    size_t mesh_count = meshes->count - mesh_start;
    if (mesh_count > 0 && node_mapping_count < 256) {
        node_mappings[node_mapping_count].node = node;
        node_mappings[node_mapping_count].mesh_start_index = mesh_start;
        node_mappings[node_mapping_count].mesh_count = mesh_count;
        node_mapping_count++;
    }
    
    // Process children recursively
    for (size_t i = 0; i < node->children_count; i++) {
        process_node(node->children[i], data, meshes, world_transform);
    }
}

// Load all meshes by processing the scene graph
void load_gltf_meshes(cgltf_data* data, Meshes* meshes) {
    if (!data || !meshes) {
        fprintf(stderr, "Invalid parameters to load_gltf_meshes\n");
        return;
    }

    if (data->scenes_count == 0) {
        fprintf(stderr, "No scenes in glTF file\n");
        return;
    }

    // Reset node mappings
    node_mapping_count = 0;

    cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0];
    
    printf("Processing scene with %zu root nodes\n", scene->nodes_count);

    mat4 identity;
    glm_mat4_identity(identity);

    for (size_t i = 0; i < scene->nodes_count; i++) {
        process_node(scene->nodes[i], data, meshes, identity);
    }

    printf("Successfully loaded %zu meshes from scene graph\n", meshes->count);
}

bool load_gltf_animations(cgltf_data* data, Scene* scene) {
    if (data->animations_count == 0) {
        printf("No animations in glTF file\n");
        scene->animations = NULL;
        scene->animation_count = 0;
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

        // Process each channel
        for (size_t c = 0; c < anim->channels_count; c++) {
            cgltf_animation_channel* gltf_channel = &anim->channels[c];
            cgltf_animation_sampler* sampler = gltf_channel->sampler;
            AnimationChannel* channel = &animations[i].channels[c];
            
            // Store target node
            channel->target_node = gltf_channel->target_node;
            channel->path = gltf_channel->target_path;
            
            // Read keyframe times and values
            cgltf_accessor* input = sampler->input;
            cgltf_accessor* output = sampler->output;
            
            channel->keyframe_count = input->count;
            channel->times = malloc(input->count * sizeof(float));
            
            // Read times
            for (size_t k = 0; k < input->count; k++) {
                cgltf_accessor_read_float(input, k, &channel->times[k], 1);
                if (channel->times[k] > animations[i].duration) {
                    animations[i].duration = channel->times[k];
                }
            }
            
            // Read values based on path type
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
            }
        }
        
        printf("    Duration: %.2f seconds\n", animations[i].duration);
    }

    scene->animations = animations;
    scene->animation_count = data->animations_count;
    return true;
}

bool load_gltf_complete(const char* filepath, Scene* scene) {
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
    
    // Load textures first
    if (!load_gltf_textures(data, filepath)) {
        printf("Warning: Failed to load some textures\n");
    }
    
    // Load meshes with scene graph transforms
    load_gltf_meshes(data, &scene->meshes);
    
    // Load animations
    load_gltf_animations(data, scene);
    
    // Store cgltf_data for animation runtime (needed for node lookups)
    scene->gltf_data = data;
    
    return true;
}

void free_gltf_animations(Scene* scene) {
    if (!scene->animations) return;
    
    for (size_t i = 0; i < scene->animation_count; i++) {
        Animation* anim = &scene->animations[i];
        
        if (anim->name) free(anim->name);
        
        for (size_t c = 0; c < anim->channel_count; c++) {
            AnimationChannel* channel = &anim->channels[c];
            if (channel->times) free(channel->times);
            if (channel->translations) free(channel->translations);
            if (channel->rotations) free(channel->rotations);
            if (channel->scales) free(channel->scales);
        }
        
        free(anim->channels);
    }
    
    free(scene->animations);
    scene->animations = NULL;
    scene->animation_count = 0;
    
    if (scene->gltf_data) {
        cgltf_free((cgltf_data*)scene->gltf_data);
        scene->gltf_data = NULL;
    }
}


/// ANIMATION


// Linear interpolation helper
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Find keyframe index for given time
static size_t find_keyframe(float* times, size_t count, float time) {
    for (size_t i = 0; i < count - 1; i++) {
        if (time >= times[i] && time < times[i + 1]) {
            return i;
        }
    }
    return count - 2;
}

// Apply animation to scene
void animate_scene(Scene* scene, float time) {
    if (!scene->animations) return;
    
    // Play all animations (you can modify to play specific ones)
    for (size_t a = 0; a < scene->animation_count; a++) {
        Animation* anim = &scene->animations[a];
        
        // Loop animation
        float anim_time = fmodf(time, anim->duration);
        
        // Process each channel
        for (size_t c = 0; c < anim->channel_count; c++) {
            AnimationChannel* channel = &anim->channels[c];
            cgltf_node* target_node = channel->target_node;
            
            // Find keyframes
            size_t k0 = find_keyframe(channel->times, channel->keyframe_count, anim_time);
            size_t k1 = k0 + 1;
            
            // Calculate interpolation factor
            float t0 = channel->times[k0];
            float t1 = channel->times[k1];
            float factor = (anim_time - t0) / (t1 - t0);
            
            // Find meshes that use this node and update them
            for (size_t m = 0; m < scene->meshes.count; m++) {
                Mesh* mesh = &scene->meshes.items[m];
                if (mesh->node == target_node) {
                    mat4 animated_transform;
                    glm_mat4_identity(animated_transform);
                    
                    // Apply translation
                    if (channel->path == cgltf_animation_path_type_translation && channel->translations) {
                        vec3 pos;
                        glm_vec3_lerp(channel->translations[k0], channel->translations[k1], factor, pos);
                        glm_translate(animated_transform, pos);
                    }
                    
                    // Apply rotation
                    if (channel->path == cgltf_animation_path_type_rotation && channel->rotations) {
                        versor rot;
                        glm_quat_slerp(channel->rotations[k0], channel->rotations[k1], factor, rot);
                        mat4 rot_mat;
                        glm_quat_mat4(rot, rot_mat);
                        glm_mat4_mul(animated_transform, rot_mat, animated_transform);
                    }
                    
                    // Apply scale
                    if (channel->path == cgltf_animation_path_type_scale && channel->scales) {
                        vec3 scale;
                        glm_vec3_lerp(channel->scales[k0], channel->scales[k1], factor, scale);
                        glm_scale(animated_transform, scale);
                    }
                    
                    // Update mesh model matrix
                    glm_mat4_copy(animated_transform, mesh->model);
                }
            }
        }
    }
}




/* #define CGLTF_IMPLEMENTATION */
/* #include "gltf_loader.h" */
/* #include <string.h> */
/* #include "context.h" */

/* static int32_t gltf_texture_indices[MAX_TEXTURES]; */
/* static size_t gltf_texture_count = 0; */

/* // Helper to get directory from filepath */
/* void get_directory(const char* filepath, char* dir, size_t dir_size) { */
/*     const char* last_slash = strrchr(filepath, '/'); */
/*     if (last_slash) { */
/*         size_t len = last_slash - filepath + 1; */
/*         if (len < dir_size) { */
/*             memcpy(dir, filepath, len); */
/*             dir[len] = '\0'; */
/*         } */
/*     } else { */
/*         dir[0] = '\0'; */
/*     } */
/* } */

/* // Load textures from glTF */
/* bool load_gltf_textures(cgltf_data* data, const char* base_path) { */
/*     if (data->textures_count == 0) { */
/*         printf("No textures in glTF file\n"); */
/*         return true; */
/*     } */

/*     char dir[512]; */
/*     get_directory(base_path, dir, sizeof(dir)); */

/*     printf("Loading %zu textures from glTF...\n", data->textures_count); */
/*     gltf_texture_count = 0; */

/*     for (size_t i = 0; i < data->textures_count; i++) { */
/*         cgltf_texture* tex = &data->textures[i]; */
        
/*         if (!tex->image || !tex->image->uri) { */
/*             printf("  Texture %zu: No image URI\n", i); */
/*             gltf_texture_indices[i] = -1; */
/*             continue; */
/*         } */

/*         char full_path[1024]; */
/*         snprintf(full_path, sizeof(full_path), "%s%s", dir, tex->image->uri); */

/*         printf("  Texture %zu: Loading '%s'\n", i, full_path); */

/*         int32_t tex_id = texture_pool_add(&context, full_path); */
/*         if (tex_id < 0) { */
/*             fprintf(stderr, "  Failed to load texture: %s\n", full_path); */
/*             gltf_texture_indices[i] = -1; */
/*             continue; */
/*         } */

/*         gltf_texture_indices[i] = tex_id; */
/*         gltf_texture_count = i + 1; */
        
/*         printf("  -> Loaded as texture #%d\n", tex_id); */
/*     } */

/*     return true; */
/* } */

/* // NEW: Process a single primitive with indices */
/* static Mesh create_mesh_from_primitive(cgltf_primitive* prim, cgltf_data* data, const char* name) { */
/*     Mesh mesh = {0}; */
/*     mesh.name = strdup(name); */
/*     mesh.textureIndex = -1; */
/*     mesh.texture = NULL; */
/*     glm_mat4_identity(mesh.model); */

/*     // Get accessors */
/*     cgltf_accessor* pos_accessor = NULL; */
/*     cgltf_accessor* normal_accessor = NULL; */
/*     cgltf_accessor* texcoord_accessor = NULL; */
/*     cgltf_accessor* color_accessor = NULL; */

/*     for (size_t j = 0; j < prim->attributes_count; ++j) { */
/*         cgltf_attribute* attr = &prim->attributes[j]; */
/*         switch (attr->type) { */
/*             case cgltf_attribute_type_position: */
/*                 pos_accessor = attr->data; */
/*                 break; */
/*             case cgltf_attribute_type_normal: */
/*                 normal_accessor = attr->data; */
/*                 break; */
/*             case cgltf_attribute_type_texcoord: */
/*                 texcoord_accessor = attr->data; */
/*                 break; */
/*             case cgltf_attribute_type_color: */
/*                 color_accessor = attr->data; */
/*                 break; */
/*             default: */
/*                 break; */
/*         } */
/*     } */

/*     if (!pos_accessor) { */
/*         printf("  Primitive has no position data\n"); */
/*         return mesh; */
/*     } */

/*     size_t vertex_count = pos_accessor->count; */
    
/*     // CRITICAL: Check for indices */
/*     cgltf_accessor* indices_accessor = prim->indices; */
/*     size_t index_count = 0; */
/*     uint32_t* indices = NULL; */
    
/*     if (indices_accessor) { */
/*         index_count = indices_accessor->count; */
/*         indices = malloc(index_count * sizeof(uint32_t)); */
        
/*         // Read indices (glTF can use uint16 or uint32) */
/*         for (size_t i = 0; i < index_count; i++) { */
/*             indices[i] = (uint32_t)cgltf_accessor_read_index(indices_accessor, i); */
/*         } */
        
/*         printf("  -> Has %zu indices\n", index_count); */
/*     } else { */
/*         printf("  -> No indices (drawing sequential)\n"); */
/*     } */

/*     // Allocate vertices */
/*     Vertex* vertices = malloc(vertex_count * sizeof(Vertex)); */

/*     // Read vertex data */
/*     for (size_t v = 0; v < vertex_count; v++) { */
/*         if (pos_accessor) { */
/*             cgltf_accessor_read_float(pos_accessor, v, vertices[v].pos, 3); */
/*         } */

/*         if (normal_accessor) { */
/*             cgltf_accessor_read_float(normal_accessor, v, vertices[v].normal, 3); */
/*         } else { */
/*             vertices[v].normal[0] = 0.0f; */
/*             vertices[v].normal[1] = 1.0f; */
/*             vertices[v].normal[2] = 0.0f; */
/*         } */

/*         if (texcoord_accessor) { */
/*             cgltf_accessor_read_float(texcoord_accessor, v, vertices[v].texCoord, 2); */
/*         } else { */
/*             vertices[v].texCoord[0] = 0.0f; */
/*             vertices[v].texCoord[1] = 0.0f; */
/*         } */

/*         if (color_accessor) { */
/*             float color[4]; */
/*             cgltf_accessor_read_float(color_accessor, v, color, 4); */
/*             vertices[v].color[0] = color[0]; */
/*             vertices[v].color[1] = color[1]; */
/*             vertices[v].color[2] = color[2]; */
/*             vertices[v].color[3] = color[3]; */
/*         } else { */
/*             vertices[v].color[0] = 1.0f; */
/*             vertices[v].color[1] = 1.0f; */
/*             vertices[v].color[2] = 1.0f; */
/*             vertices[v].color[3] = 1.0f; */
/*         } */

/*         vertices[v].textureIndex = 0; */
/*     } */

/*     // If we have indices, expand vertices to indexed order */
/*     Vertex* final_vertices = NULL; */
/*     size_t final_vertex_count = 0; */
    
/*     if (indices) { */
/*         // Create expanded vertex array based on indices */
/*         final_vertex_count = index_count; */
/*         final_vertices = malloc(final_vertex_count * sizeof(Vertex)); */
        
/*         for (size_t i = 0; i < index_count; i++) { */
/*             final_vertices[i] = vertices[indices[i]]; */
/*         } */
        
/*         free(vertices); */
/*         free(indices); */
/*     } else { */
/*         // No indices, use vertices as-is */
/*         final_vertices = vertices; */
/*         final_vertex_count = vertex_count; */
/*     } */

/*     mesh.vertexCount = final_vertex_count; */

/*     // Get texture from material */
/*     if (prim->material && prim->material->has_pbr_metallic_roughness) { */
/*         cgltf_pbr_metallic_roughness* pbr = &prim->material->pbr_metallic_roughness; */
        
/*         if (pbr->base_color_texture.texture) { */
/*             cgltf_texture* base_texture = pbr->base_color_texture.texture; */
            
/*             for (size_t t = 0; t < data->textures_count; t++) { */
/*                 if (&data->textures[t] == base_texture) { */
/*                     if (t < gltf_texture_count && gltf_texture_indices[t] >= 0) { */
/*                         mesh.textureIndex = gltf_texture_indices[t]; */
/*                         mesh.texture = texture_pool_get(mesh.textureIndex); */
/*                         printf("  -> Using texture pool #%d\n", mesh.textureIndex); */
/*                     } */
/*                     break; */
/*                 } */
/*             } */
/*         } */
/*     } */

/*     // Create Vulkan buffers */
/*     VkBufferCreateInfo bufferInfo = { */
/*         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, */
/*         .size = final_vertex_count * sizeof(Vertex), */
/*         .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, */
/*         .sharingMode = VK_SHARING_MODE_EXCLUSIVE */
/*     }; */

/*     vkCreateBuffer(context.device, &bufferInfo, NULL, &mesh.vertexBuffer); */

/*     VkMemoryRequirements memRequirements; */
/*     vkGetBufferMemoryRequirements(context.device, mesh.vertexBuffer, &memRequirements); */

/*     VkMemoryAllocateInfo allocInfo = { */
/*         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, */
/*         .allocationSize = memRequirements.size, */
/*         .memoryTypeIndex = findMemoryType( */
/*             context.physicalDevice,  */
/*             memRequirements.memoryTypeBits, */
/*             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT */
/*         ) */
/*     }; */

/*     vkAllocateMemory(context.device, &allocInfo, NULL, &mesh.vertexBufferMemory); */
/*     vkBindBufferMemory(context.device, mesh.vertexBuffer, mesh.vertexBufferMemory, 0); */

/*     // Upload vertex data */
/*     void* data_ptr; */
/*     vkMapMemory(context.device, mesh.vertexBufferMemory, 0, bufferInfo.size, 0, &data_ptr); */
/*     memcpy(data_ptr, final_vertices, final_vertex_count * sizeof(Vertex)); */
/*     vkUnmapMemory(context.device, mesh.vertexBufferMemory); */

/*     free(final_vertices); */

/*     printf("Loaded mesh '%s' with %zu vertices\n", mesh.name, final_vertex_count); */
/*     return mesh; */
/* } */

/* // NEW: Process node hierarchy recursively */
/* static void process_node(cgltf_node* node, cgltf_data* data, Meshes* meshes, mat4 parent_transform) { */
/*     mat4 local_transform; */
/*     mat4 world_transform; */
    
/*     // Get local transform */
/*     cgltf_node_transform_local(node, (float*)local_transform); */
    
/*     // Combine with parent */
/*     glm_mat4_mul(parent_transform, local_transform, world_transform); */
    
/*     // Process mesh if this node has one */
/*     if (node->mesh) { */
/*         cgltf_mesh* gltf_mesh = node->mesh; */
        
/*         for (size_t i = 0; i < gltf_mesh->primitives_count; i++) { */
/*             char mesh_name[256]; */
/*             snprintf(mesh_name, sizeof(mesh_name), "%s_prim_%zu",  */
/*                      node->name ? node->name : "node", i); */
            
/*             Mesh mesh = create_mesh_from_primitive(&gltf_mesh->primitives[i], data, mesh_name); */
            
/*             if (mesh.vertexCount > 0) { */
/*                 // Apply the world transform to this mesh */
/*                 glm_mat4_copy(world_transform, mesh.model); */
/*                 meshes_add(meshes, mesh); */
/*             } */
/*         } */
/*     } */
    
/*     // Process children recursively */
/*     for (size_t i = 0; i < node->children_count; i++) { */
/*         process_node(node->children[i], data, meshes, world_transform); */
/*     } */
/* } */

/* // NEW: Load all meshes by processing the scene graph */
/* void load_gltf_meshes(cgltf_data* data, Meshes* meshes) { */
/*     if (!data || !meshes) { */
/*         fprintf(stderr, "Invalid parameters to load_gltf_meshes\n"); */
/*         return; */
/*     } */

/*     if (data->scenes_count == 0) { */
/*         fprintf(stderr, "No scenes in glTF file\n"); */
/*         return; */
/*     } */

/*     // Use the default scene, or the first scene if no default */
/*     cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0]; */
    
/*     printf("Processing scene with %zu root nodes\n", scene->nodes_count); */

/*     mat4 identity; */
/*     glm_mat4_identity(identity); */

/*     // Process each root node in the scene */
/*     for (size_t i = 0; i < scene->nodes_count; i++) { */
/*         process_node(scene->nodes[i], data, meshes, identity); */
/*     } */

/*     printf("Successfully loaded %zu meshes from scene graph\n", meshes->count); */
/* } */

/* // Animation loading (unchanged) */
/* bool load_gltf_animations(cgltf_data* data, Animation** out_animations, size_t* out_count) { */
/*     if (data->animations_count == 0) { */
/*         printf("No animations in glTF file\n"); */
/*         *out_animations = NULL; */
/*         *out_count = 0; */
/*         return true; */
/*     } */

/*     printf("Loading %zu animations from glTF...\n", data->animations_count); */

/*     Animation* animations = malloc(data->animations_count * sizeof(Animation)); */
    
/*     for (size_t i = 0; i < data->animations_count; i++) { */
/*         cgltf_animation* anim = &data->animations[i]; */
        
/*         printf("  Animation %zu: '%s' with %zu channels\n",  */
/*                i, anim->name ? anim->name : "unnamed", anim->channels_count); */

/*         if (anim->channels_count > 0) { */
/*             cgltf_animation_channel* channel = &anim->channels[0]; */
/*             cgltf_animation_sampler* sampler = channel->sampler; */
            
/*             cgltf_accessor* input = sampler->input; */
/*             cgltf_accessor* output = sampler->output; */
            
/*             size_t keyframe_count = input->count; */
/*             animations[i].keyframe_count = keyframe_count; */
/*             animations[i].keyframes = malloc(keyframe_count * sizeof(AnimationKeyframe)); */
            
/*             for (size_t k = 0; k < keyframe_count; k++) { */
/*                 float time; */
/*                 cgltf_accessor_read_float(input, k, &time, 1); */
/*                 animations[i].keyframes[k].time = time; */
                
/*                 glm_vec3_zero(animations[i].keyframes[k].translation); */
/*                 glm_quat_identity(animations[i].keyframes[k].rotation); */
/*                 glm_vec3_one(animations[i].keyframes[k].scale); */
/*             } */
            
/*             if (channel->target_path == cgltf_animation_path_type_translation) { */
/*                 for (size_t k = 0; k < keyframe_count; k++) { */
/*                     cgltf_accessor_read_float(output, k,  */
/*                         animations[i].keyframes[k].translation, 3); */
/*                 } */
/*             } else if (channel->target_path == cgltf_animation_path_type_rotation) { */
/*                 for (size_t k = 0; k < keyframe_count; k++) { */
/*                     cgltf_accessor_read_float(output, k,  */
/*                         animations[i].keyframes[k].rotation, 4); */
/*                 } */
/*             } else if (channel->target_path == cgltf_animation_path_type_scale) { */
/*                 for (size_t k = 0; k < keyframe_count; k++) { */
/*                     cgltf_accessor_read_float(output, k,  */
/*                         animations[i].keyframes[k].scale, 3); */
/*                 } */
/*             } */
            
/*             float max_time = 0.0f; */
/*             cgltf_accessor_read_float(input, keyframe_count - 1, &max_time, 1); */
/*             animations[i].duration = max_time; */
            
/*             printf("    Duration: %.2f seconds, %zu keyframes\n",  */
/*                    max_time, keyframe_count); */
/*         } */
/*     } */

/*     *out_animations = animations; */
/*     *out_count = data->animations_count; */
/*     return true; */
/* } */

/* // Complete loader */
/* bool load_gltf_complete(const char* filepath, Scene* scene) { */
/*     cgltf_options options = {0}; */
/*     cgltf_data* data = NULL; */
    
/*     FILE* test = fopen(filepath, "r"); */
/*     if (!test) { */
/*         printf("Cannot open file '%s'\n", filepath); */
/*         return false; */
/*     } */
/*     fclose(test); */
    
/*     printf("Parsing glTF file: %s\n", filepath); */
    
/*     cgltf_result result = cgltf_parse_file(&options, filepath, &data); */
/*     if (result != cgltf_result_success) { */
/*         printf("Failed to parse GLTF file '%s': %d\n", filepath, result); */
/*         return false; */
/*     } */
    
/*     printf("Successfully parsed GLTF: %s\n", filepath); */
/*     printf("  Scenes: %zu\n", data->scenes_count); */
/*     printf("  Nodes: %zu\n", data->nodes_count); */
/*     printf("  Meshes: %zu\n", data->meshes_count); */
/*     printf("  Materials: %zu\n", data->materials_count); */
/*     printf("  Textures: %zu\n", data->textures_count); */
/*     printf("  Animations: %zu\n", data->animations_count); */
/*     printf("\n"); */
    
/*     result = cgltf_load_buffers(&options, data, filepath); */
/*     if (result != cgltf_result_success) { */
/*         printf("Failed to load buffers: %d\n", result); */
/*         cgltf_free(data); */
/*         return false; */
/*     } */
    
/*     // Load textures first */
/*     if (!load_gltf_textures(data, filepath)) { */
/*         printf("Warning: Failed to load some textures\n"); */
/*     } */
    
/*     // Load meshes with scene graph transforms */
/*     load_gltf_meshes(data, &scene->meshes); */
    
/*     // Load animations */
/*     Animation* animations = NULL; */
/*     size_t anim_count = 0; */
/*     load_gltf_animations(data, &animations, &anim_count); */
    
/*     cgltf_free(data); */
/*     return true; */
/* } */
