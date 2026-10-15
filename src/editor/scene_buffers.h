#pragma once

#include "scene.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ObsSceneBuffer ObsSceneBuffer;

typedef enum {
    OBS_SCENE_CACHE_NOT_APPLICABLE,
    OBS_SCENE_CACHE_MISS,
    OBS_SCENE_CACHE_HIT
} ObsSceneCacheState;

bool obs_scene_buffers_init(Scene *initial_scene, const char *name,
                            const char *source_path);
ObsSceneBuffer *obs_scene_buffer_open(const char *path);
void obs_scene_buffers_cleanup(void);

size_t obs_scene_buffer_count(void);
ObsSceneBuffer *obs_scene_buffer_first(void);
ObsSceneBuffer *obs_scene_buffer_next(const ObsSceneBuffer *buffer);
ObsSceneBuffer *obs_scene_buffer_at(size_t index);
ObsSceneBuffer *obs_scene_buffer_current(void);
bool obs_scene_buffer_activate(ObsSceneBuffer *buffer);
bool obs_scene_buffer_should_animate(ObsSceneBuffer *buffer, uint64_t tick);
bool obs_scene_buffer_needs_alpha_sort(ObsSceneBuffer *buffer,
                                      vec3 camera_position);

Scene *obs_scene_buffer_scene(const ObsSceneBuffer *buffer);
const char *obs_scene_buffer_name(const ObsSceneBuffer *buffer);
const char *obs_scene_buffer_path(const ObsSceneBuffer *buffer);
ObsSceneCacheState obs_scene_buffer_cache_state(const ObsSceneBuffer *buffer);
size_t obs_scene_buffer_mesh_count(const ObsSceneBuffer *buffer);
size_t obs_scene_buffer_instance_count(const ObsSceneBuffer *buffer);
void obs_scene_buffer_store_selection(ObsSceneBuffer *buffer, int mesh_index,
                                      int hierarchy_index, int sdf_index);
void obs_scene_buffer_load_selection(const ObsSceneBuffer *buffer,
                                     int *mesh_index, int *hierarchy_index,
                                     int *sdf_index);
