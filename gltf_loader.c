#define CGLTF_IMPLEMENTATION
#include "gltf_loader.h"
#include <string.h>
#include "context.h"

static int32_t gltf_texture_indices[MAX_TEXTURES];
static size_t gltf_texture_count = 0;

typedef struct {
    cgltf_node* node;
    size_t mesh_start_index;
    size_t mesh_count;
} NodeMeshMapping;

static NodeMeshMapping node_mappings[256];
static size_t node_mapping_count = 0;


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
    mesh.texture = NULL;
    mesh.node = NULL;
    mesh.morph_data = NULL;
    mesh.is_unlit = false;
    mesh.alpha_mode = 0; // OPAQUE by default
    mesh.alpha_cutoff = 0.5f; // Default cutoff
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);

    // Find all attribute accessors
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
    }

    // Load morph targets BEFORE expanding indices
    mesh.morph_data = load_morph_targets(prim);
    
    // Expand indices or use vertices directly
    Vertex* final_vertices = NULL;
    size_t final_vertex_count = 0;
    
    if (indices) {
        final_vertex_count = index_count;
        final_vertices = malloc(final_vertex_count * sizeof(Vertex));
        
        // Expand vertices AND store the index mapping for morphing
        if (mesh.morph_data) {
            mesh.morph_data->index_map = malloc(index_count * sizeof(uint32_t));
        }
        
        for (size_t i = 0; i < index_count; i++) {
            final_vertices[i] = vertices[indices[i]];
            
            // Store which original vertex this expanded vertex came from
            if (mesh.morph_data) {
                mesh.morph_data->index_map[i] = indices[i];
            }
        }
        
        // Store base vertices for morphing (unexpanded)
        if (mesh.morph_data) {
            mesh.morph_data->base_vertices = malloc(vertex_count * sizeof(Vertex));
            memcpy(mesh.morph_data->base_vertices, vertices, vertex_count * sizeof(Vertex));
            mesh.morph_data->base_vertex_count = vertex_count;
        }
        
        free(vertices);
        free(indices);
    } else {
        final_vertices = vertices;
        final_vertex_count = vertex_count;
        
        // No indices, so identity mapping
        if (mesh.morph_data) {
            mesh.morph_data->base_vertices = malloc(vertex_count * sizeof(Vertex));
            memcpy(mesh.morph_data->base_vertices, vertices, vertex_count * sizeof(Vertex));
            mesh.morph_data->base_vertex_count = vertex_count;
            mesh.morph_data->index_map = NULL;  // No mapping needed
        }
    }

    mesh.vertexCount = final_vertex_count;

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

    // Create Vulkan vertex buffer
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

    void* data_ptr;
    vkMapMemory(context.device, mesh.vertexBufferMemory, 0, bufferInfo.size, 0, &data_ptr);
    memcpy(data_ptr, final_vertices, final_vertex_count * sizeof(Vertex));
    vkUnmapMemory(context.device, mesh.vertexBufferMemory);

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
    cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0];
    
    printf("Processing scene with %zu root nodes\n", scene->nodes_count);

    mat4 identity;
    glm_mat4_identity(identity);

    for (size_t i = 0; i < scene->nodes_count; i++) {
        process_node(scene->nodes[i], data, meshes, identity);
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
    
    load_gltf_meshes(data, &scene->meshes);
    
    instance->mesh_count = scene->meshes.count - instance->mesh_start_index;
    
    load_gltf_animations(data, instance);
    
    scene->gltf_instance_count++;
    
    printf("Loaded glTF instance #%zu with %zu meshes (indices %zu to %zu)\n",
           scene->gltf_instance_count - 1,
           instance->mesh_count,
           instance->mesh_start_index,
           instance->mesh_start_index + instance->mesh_count - 1);
    
    return true;
}    

static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static size_t find_keyframe(float* times, size_t count, float time) {
    for (size_t i = 0; i < count - 1; i++) {
        if (time >= times[i] && time < times[i + 1]) {
            return i;
        }
    }
    return count - 2;
}

void animate_scene(Scene* scene, float time) {
    // Animate each glTF instance independently
    for (size_t inst = 0; inst < scene->gltf_instance_count; inst++) {
        GLTFInstance* instance = &scene->gltf_instances[inst];
        
        if (!instance->animations) continue;
        
        for (size_t a = 0; a < instance->animation_count; a++) {
            Animation* anim = &instance->animations[a];
            
            float anim_time = fmodf(time, anim->duration);
            
            for (size_t c = 0; c < anim->channel_count; c++) {
                AnimationChannel* channel = &anim->channels[c];
                cgltf_node* target_node = channel->target_node;
                
                size_t k0 = find_keyframe(channel->times, channel->keyframe_count, anim_time);
                size_t k1 = k0 + 1;
                
                float t0 = channel->times[k0];
                float t1 = channel->times[k1];
                float factor = (anim_time - t0) / (t1 - t0);
                
                // Handle morph weight animations
                if (channel->path == cgltf_animation_path_type_weights && channel->weights) {
                    // Only search meshes belonging to THIS instance
                    size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
                    for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                        Mesh* mesh = &scene->meshes.items[m];
                        if (mesh->node == target_node && mesh->morph_data) {
                            size_t num_targets = mesh->morph_data->target_count;
                            
                            // Interpolate weights for each morph target
                            for (size_t t = 0; t < num_targets; t++) {
                                float w0 = channel->weights[k0 * num_targets + t];
                                float w1 = channel->weights[k1 * num_targets + t];
                                mesh->morph_data->weights[t] = lerp(w0, w1, factor);
                            }
                            
                            // Update the mesh vertices with morphed positions
                            mesh_update_morph(mesh);
                        }
                    }
                }
                
                // Handle transformation animations
                size_t mesh_end = instance->mesh_start_index + instance->mesh_count;
                for (size_t m = instance->mesh_start_index; m < mesh_end; m++) {
                    Mesh* mesh = &scene->meshes.items[m];
                    if (mesh->node == target_node) {
                        mat4 animated_transform;
                        glm_mat4_identity(animated_transform);
                        
                        if (channel->path == cgltf_animation_path_type_translation && channel->translations) {
                            vec3 pos;
                            glm_vec3_lerp(channel->translations[k0], channel->translations[k1], factor, pos);
                            glm_translate(animated_transform, pos);
                        }
                        
                        if (channel->path == cgltf_animation_path_type_rotation && channel->rotations) {
                            versor rot;
                            glm_quat_slerp(channel->rotations[k0], channel->rotations[k1], factor, rot);
                            mat4 rot_mat;
                            glm_quat_mat4(rot, rot_mat);
                            glm_mat4_mul(animated_transform, rot_mat, animated_transform);
                        }
                        
                        if (channel->path == cgltf_animation_path_type_scale && channel->scales) {
                            vec3 scale;
                            glm_vec3_lerp(channel->scales[k0], channel->scales[k1], factor, scale);
                            glm_scale(animated_transform, scale);
                        }
                        
                        glm_mat4_copy(animated_transform, mesh->model);
                    }
                }
            }
        }
    }
}
