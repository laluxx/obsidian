#pragma once
#include "renderer.h"
#include "cgltf.h"

typedef struct {
    cgltf_node* target_node;
    cgltf_animation_path_type path;
    size_t keyframe_count;
    float* times;
    vec3* translations;
    versor* rotations;
    vec3* scales;
} AnimationChannel;

typedef struct {
    char* name;
    float duration;
    size_t channel_count;
    AnimationChannel* channels;
} Animation;

typedef struct {
    Meshes meshes;
    Animation* animations;
    size_t animation_count;
    void* gltf_data;  // Store cgltf_data for runtime
} Scene;

extern Scene scene;

void scene_init(Scene *s);
void scene_cleanup(Scene *s);
