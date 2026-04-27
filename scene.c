#include "scene.h"
#include "renderer.h"

Scene scene = {0};

void scene_init(Scene *s) {
    // Initialize the virtual root node at index 0
    s->tree.count = 0;
    s->tree.root  = 0;
    scene_tree_add_node(&s->tree, "Scene", -1, -1);
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
