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
    float* weights;   // For morph
} AnimationChannel;

typedef struct {
    char* name;
    float duration;
    size_t channel_count;
    AnimationChannel* channels;
} Animation;

typedef struct {
    Animation* animations;
    size_t animation_count;
    cgltf_data* gltf_data;
    size_t mesh_start_index;  // First mesh from this glTF
    size_t mesh_count;        // Number of meshes from this glTF
} GLTFInstance;

typedef struct {
    Meshes meshes;
    GLTFInstance* gltf_instances;
    size_t gltf_instance_count;
    size_t gltf_instance_capacity;
} Scene;

extern Scene scene;

void scene_init(Scene *s);
void scene_cleanup(Scene *s);

void print_scene_meshes();
