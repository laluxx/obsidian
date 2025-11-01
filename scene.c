#include "scene.h"
#include "renderer.h"

Scene scene = {0};

void scene_init(Scene *s) {
    meshes_init(&s->meshes);
    s->gltf_instances = NULL;
    s->gltf_instance_count = 0;
    s->gltf_instance_capacity = 0;
}

void scene_cleanup(Scene *s) {
    meshes_destroy(context.device, &s->meshes);
    
    // Clean up all glTF instances
    for (size_t i = 0; i < s->gltf_instance_count; i++) {
        GLTFInstance* instance = &s->gltf_instances[i];
        
        // Free animations
        if (instance->animations) {
            for (size_t a = 0; a < instance->animation_count; a++) {
                Animation* anim = &instance->animations[a];
                
                if (anim->name) free(anim->name);
                
                for (size_t c = 0; c < anim->channel_count; c++) {
                    AnimationChannel* channel = &anim->channels[c];
                    if (channel->times) free(channel->times);
                    if (channel->translations) free(channel->translations);
                    if (channel->rotations) free(channel->rotations);
                    if (channel->scales) free(channel->scales);
                    if (channel->weights) free(channel->weights);
                }
                
                free(anim->channels);
            }
            free(instance->animations);
        }
        
        // Free cgltf data
        if (instance->gltf_data) {
            cgltf_free(instance->gltf_data);
        }
    }
    
    if (s->gltf_instances) {
        free(s->gltf_instances);
        s->gltf_instances = NULL;
    }
    
    s->gltf_instance_count = 0;
    s->gltf_instance_capacity = 0;
}

void print_scene_meshes() {
    printf("=== SCENE MESHES ===\n");
    printf("Total meshes: %zu\n", scene.meshes.count);
    printf("Capacity: %zu\n", scene.meshes.capacity);
    printf("\n");
    
    for (size_t i = 0; i < scene.meshes.count; i++) {
        Mesh* mesh = &scene.meshes.items[i];
        
        printf("Mesh [%zu]:\n", i);
        printf("  Name: %s\n", mesh->name ? mesh->name : "(null)");
        printf("  Vertex Buffer: %p\n", (void*)mesh->vertexBuffer);
        printf("  Vertex Buffer Memory: %p\n", (void*)mesh->vertexBufferMemory);
        printf("  Vertex Count: %u\n", mesh->vertexCount);
        printf("  Node Pointer: %p\n", mesh->node);
        printf("  Texture Index: %d\n", mesh->textureIndex);
        printf("  Texture Pointer: %p\n", (void*)mesh->texture);
        printf("  Material Index: %d\n", mesh->materialIndex);
        printf("  Morph Data: %p\n", (void*)mesh->morph_data);
        printf("  Is Unlit: %s\n", mesh->is_unlit ? "true" : "false");
        
        // Print alpha mode as string
        const char* alpha_mode_str = "UNKNOWN";
        switch (mesh->alpha_mode) {
            case 0: alpha_mode_str = "OPAQUE"; break;
            case 1: alpha_mode_str = "MASK"; break;
            case 2: alpha_mode_str = "BLEND"; break;
        }
        printf("  Alpha Mode: %d (%s)\n", mesh->alpha_mode, alpha_mode_str);
        printf("  Alpha Cutoff: %.3f\n", mesh->alpha_cutoff);
        
        // Print model matrix
        printf("  Model Matrix:\n");
        for (int row = 0; row < 4; row++) {
            printf("    [");
            for (int col = 0; col < 4; col++) {
                printf("%6.2f", mesh->model[row][col]);
                if (col < 3) printf(", ");
            }
            printf("]\n");
        }
        
        // Print local transform matrix
        printf("  Local Transform:\n");
        for (int row = 0; row < 4; row++) {
            printf("    [");
            for (int col = 0; col < 4; col++) {
                printf("%6.2f", mesh->local_transform[row][col]);
                if (col < 3) printf(", ");
            }
            printf("]\n");
        }
        
        printf("\n");
    }
    
    // Print GLTF instances information
    printf("=== GLTF INSTANCES ===\n");
    printf("Total instances: %zu\n", scene.gltf_instance_count);
    printf("Instance capacity: %zu\n", scene.gltf_instance_capacity);
    
    for (size_t i = 0; i < scene.gltf_instance_count; i++) {
        GLTFInstance* instance = &scene.gltf_instances[i];
        
        printf("GLTF Instance [%zu]:\n", i);
        printf("  Mesh range: [%zu - %zu] (%zu meshes)\n", 
               instance->mesh_start_index, 
               instance->mesh_start_index + instance->mesh_count - 1,
               instance->mesh_count);
        printf("  Animation count: %zu\n", instance->animation_count);
        printf("  GLTF data: %p\n", (void*)instance->gltf_data);
        
        // Print animation details
        for (size_t j = 0; j < instance->animation_count; j++) {
            Animation* anim = &instance->animations[j];
            printf("    Animation [%zu]: %s\n", j, anim->name ? anim->name : "(unnamed)");
            printf("      Duration: %.3f seconds\n", anim->duration);
            printf("      Channels: %zu\n", anim->channel_count);
            
            for (size_t k = 0; k < anim->channel_count; k++) {
                AnimationChannel* channel = &anim->channels[k];
                const char* path_str = "UNKNOWN";
                switch (channel->path) {
                    case cgltf_animation_path_type_translation: path_str = "TRANSLATION"; break;
                    case cgltf_animation_path_type_rotation: path_str = "ROTATION"; break;
                    case cgltf_animation_path_type_scale: path_str = "SCALE"; break;
                    case cgltf_animation_path_type_weights: path_str = "WEIGHTS"; break;
                }
                printf("      Channel [%zu]: %s (%zu keyframes)\n", k, path_str, channel->keyframe_count);
            }
        }
        printf("\n");
    }
}
