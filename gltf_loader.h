#pragma once
#include <stdbool.h>
#include "renderer.h"
/* #include "cgltf.h" */
#include "scene.h"


void get_directory(const char* filepath, char* dir, size_t dir_size);
bool load_gltf_textures(cgltf_data* data, const char* base_path);
bool load_gltf_animations(cgltf_data* data, Scene* scene);
bool load_gltf_complete(const char* filepath, Scene* scene);
void free_gltf_animations(Scene* scene);

// Animation playback
void animate_scene(Scene* scene, float time);

