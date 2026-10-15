#include "scene_buffers.h"
#include "editor.h"
#include "gltf_loader.h"
#include "obj.h"
#include "object_identity.h"
#include "renderer.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OBS_SCENE_BUFFER_NAME_MAX 128
#define OBS_SCENE_BUFFER_PATH_MAX 1024

struct ObsSceneBuffer {
    Scene *scene;
    char name[OBS_SCENE_BUFFER_NAME_MAX];
    char path[OBS_SCENE_BUFFER_PATH_MAX];
    ObsSceneCacheState cache_state;
    uint64_t last_animation_tick;
    bool animation_tick_valid;
    vec3 last_alpha_sort_camera;
    size_t last_alpha_sort_mesh_count;
    bool alpha_sort_valid;
    bool owns_scene;
    bool selection_valid;
    int selected_mesh_index;
    int selected_hierarchy_index;
    int selected_sdf_index;
    struct ObsSceneBuffer *next;
};

static ObsSceneBuffer *s_buffers;
static ObsSceneBuffer *s_buffers_tail;
static ObsSceneBuffer *s_current;
static size_t s_buffer_count;
static bool s_buffers_initialized;
static bool s_unregistered_scene_cleaned;
extern int g_selected_sdf_index;

static void copy_text(char *destination, size_t capacity, const char *source) {
    if (!capacity) return;
    if (!source) source = "";
    size_t length = strlen(source);
    if (length >= capacity) length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void scene_buffer_set_name(ObsSceneBuffer *buffer, const char *name) {
    if (name && *name) {
        copy_text(buffer->name, sizeof(buffer->name), name);
        return;
    }
    copy_text(buffer->name, sizeof(buffer->name), "Scene");
}

static void scene_buffer_set_path(ObsSceneBuffer *buffer, const char *path) {
    if (!path || !*path) return;
    char *canonical = realpath(path, NULL);
    const char *display = canonical ? canonical : path;
    copy_text(buffer->path, sizeof(buffer->path), display);
    const char *base = strrchr(display, '/');
    base = base ? base + 1 : display;
    char name[OBS_SCENE_BUFFER_NAME_MAX];
    copy_text(name, sizeof(name), base);
    char *extension = strrchr(name, '.');
    if (extension) *extension = '\0';
    scene_buffer_set_name(buffer, name);
    free(canonical);
}

static void scene_buffer_append(ObsSceneBuffer *buffer) {
    if (!s_buffers) s_buffers = buffer;
    else s_buffers_tail->next = buffer;
    s_buffers_tail = buffer;
    ++s_buffer_count;
}

bool obs_scene_buffers_init(Scene *initial_scene, const char *name,
                            const char *source_path) {
    if (!initial_scene || s_buffers_initialized) return false;
    ObsSceneBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return false;
    buffer->scene = initial_scene;
    buffer->cache_state = OBS_SCENE_CACHE_NOT_APPLICABLE;
    buffer->selected_mesh_index = editor.inspector.selected_mesh_index;
    buffer->selected_hierarchy_index = editor.hierarchy.selected_index;
    buffer->selected_sdf_index = g_selected_sdf_index;
    buffer->selection_valid = true;
    scene_buffer_set_name(buffer, name);
    scene_buffer_set_path(buffer, source_path);
    const char *extension = source_path ? strrchr(source_path, '.') : NULL;
    if (extension &&
        (strcasecmp(extension, ".gltf") == 0 ||
         strcasecmp(extension, ".glb") == 0))
        buffer->cache_state = obs_gltf_last_load_used_cache()
            ? OBS_SCENE_CACHE_HIT : OBS_SCENE_CACHE_MISS;
    scene_buffer_append(buffer);
    s_current = buffer;
    s_buffers_initialized = true;
    s_unregistered_scene_cleaned = false;
    return true;
}

static bool scene_buffer_load(ObsSceneBuffer *buffer, const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return false;
    if (strcasecmp(extension, ".gltf") == 0 ||
        strcasecmp(extension, ".glb") == 0) {
        bool loaded = load_gltf(path, buffer->scene);
        if (loaded)
            buffer->cache_state = obs_gltf_last_load_used_cache()
                ? OBS_SCENE_CACHE_HIT : OBS_SCENE_CACHE_MISS;
        return loaded;
    }
    if (strcasecmp(extension, ".obj") == 0) {
        Scene *active_scene = scene;
        scene = buffer->scene;
        size_t before = buffer->scene->meshes.count;
        /* load_obj takes a borrowed name and keeps an owned copy in Mesh. */
        (void)load_obj(path, buffer->name,
                       (vec4){1.0f, 1.0f, 1.0f, 1.0f});
        scene = active_scene;
        buffer->cache_state = OBS_SCENE_CACHE_NOT_APPLICABLE;
        return buffer->scene->meshes.count > before;
    }
    return false;
}

ObsSceneBuffer *obs_scene_buffer_open(const char *path) {
    if (!path || !*path || !s_buffers_initialized) return NULL;
    char *canonical = realpath(path, NULL);
    const char *identity = canonical ? canonical : path;
    for (ObsSceneBuffer *it = s_buffers; it; it = it->next) {
        if (it->path[0] && strcmp(it->path, identity) == 0) {
            free(canonical);
            return it;
        }
    }

    ObsSceneBuffer *buffer = calloc(1, sizeof(*buffer));
    Scene *loaded_scene = calloc(1, sizeof(*loaded_scene));
    if (!buffer || !loaded_scene) {
        free(buffer);
        free(loaded_scene);
        free(canonical);
        return NULL;
    }
    buffer->scene = loaded_scene;
    buffer->owns_scene = true;
    buffer->cache_state = OBS_SCENE_CACHE_NOT_APPLICABLE;
    scene_buffer_set_path(buffer, identity);
    scene_init(buffer->scene);
    bool loaded = scene_buffer_load(buffer, path);
    if (!loaded) {
        scene_cleanup(buffer->scene);
        obs_identity_destroy(&buffer->scene->foreign_objects);
        free(buffer->scene);
        free(buffer);
        free(canonical);
        return NULL;
    }
    scene_buffer_append(buffer);
    free(canonical);
    return buffer;
}

bool obs_scene_buffer_activate(ObsSceneBuffer *buffer) {
    if (!buffer || !buffer->scene) return false;
    if (s_current && s_current != buffer) {
        obs_scene_buffer_store_selection(
            s_current, editor.inspector.selected_mesh_index,
            editor.hierarchy.selected_index, g_selected_sdf_index);
    }
    s_current = buffer;
    scene = buffer->scene;
    obs_scene_buffer_load_selection(
        buffer, &editor.inspector.selected_mesh_index,
        &editor.hierarchy.selected_index, &g_selected_sdf_index);
    return true;
}

bool obs_scene_buffer_should_animate(ObsSceneBuffer *buffer, uint64_t tick) {
    if (!buffer) return false;
    if (buffer->animation_tick_valid &&
        buffer->last_animation_tick == tick)
        return false;
    buffer->last_animation_tick = tick;
    buffer->animation_tick_valid = true;
    return true;
}

bool obs_scene_buffer_needs_alpha_sort(ObsSceneBuffer *buffer,
                                      vec3 camera_position) {
    if (!buffer || !buffer->scene || !camera_position) return false;
    const size_t mesh_count = buffer->scene->meshes.count;
    const bool stale = !buffer->alpha_sort_valid ||
        mesh_count != buffer->last_alpha_sort_mesh_count ||
        glm_vec3_distance2(camera_position, buffer->last_alpha_sort_camera) >
            0.01f;
    if (stale) {
        glm_vec3_copy(camera_position, buffer->last_alpha_sort_camera);
        buffer->last_alpha_sort_mesh_count = mesh_count;
        buffer->alpha_sort_valid = true;
    }
    return stale;
}

void obs_scene_buffers_cleanup(void) {
    if (!s_buffers_initialized) {
        if (s_unregistered_scene_cleaned || !scene) return;
        scene_cleanup(scene);
        obs_identity_destroy(&scene->foreign_objects);
        s_unregistered_scene_cleaned = true;
        return;
    }

    Scene *initial_scene = s_buffers ? s_buffers->scene : scene;
    ObsSceneBuffer *buffer = s_buffers;
    while (buffer) {
        ObsSceneBuffer *next = buffer->next;
        if (buffer->scene) {
            scene_cleanup(buffer->scene);
            obs_identity_destroy(&buffer->scene->foreign_objects);
        }
        if (buffer->owns_scene) free(buffer->scene);
        free(buffer);
        buffer = next;
    }
    s_buffers = NULL;
    s_buffers_tail = NULL;
    s_current = NULL;
    s_buffer_count = 0;
    s_buffers_initialized = false;
    s_unregistered_scene_cleaned = true;
    scene = initial_scene;
}

size_t obs_scene_buffer_count(void) { return s_buffer_count; }

ObsSceneBuffer *obs_scene_buffer_first(void) { return s_buffers; }

ObsSceneBuffer *obs_scene_buffer_next(const ObsSceneBuffer *buffer) {
    return buffer ? buffer->next : NULL;
}

ObsSceneBuffer *obs_scene_buffer_at(size_t index) {
    ObsSceneBuffer *buffer = s_buffers;
    while (buffer && index) {
        buffer = buffer->next;
        --index;
    }
    return buffer;
}

ObsSceneBuffer *obs_scene_buffer_current(void) { return s_current; }

Scene *obs_scene_buffer_scene(const ObsSceneBuffer *buffer) {
    return buffer ? buffer->scene : NULL;
}

const char *obs_scene_buffer_name(const ObsSceneBuffer *buffer) {
    return buffer ? buffer->name : "";
}

const char *obs_scene_buffer_path(const ObsSceneBuffer *buffer) {
    return buffer ? buffer->path : "";
}

ObsSceneCacheState obs_scene_buffer_cache_state(const ObsSceneBuffer *buffer) {
    return buffer ? buffer->cache_state : OBS_SCENE_CACHE_NOT_APPLICABLE;
}

size_t obs_scene_buffer_mesh_count(const ObsSceneBuffer *buffer) {
    return buffer && buffer->scene ? buffer->scene->meshes.count : 0;
}

size_t obs_scene_buffer_instance_count(const ObsSceneBuffer *buffer) {
    return buffer && buffer->scene ? buffer->scene->gltf_instance_count : 0;
}

void obs_scene_buffer_store_selection(ObsSceneBuffer *buffer, int mesh_index,
                                      int hierarchy_index, int sdf_index) {
    if (!buffer) return;
    buffer->selected_mesh_index = mesh_index;
    buffer->selected_hierarchy_index = hierarchy_index;
    buffer->selected_sdf_index = sdf_index;
    buffer->selection_valid = true;
}

void obs_scene_buffer_load_selection(const ObsSceneBuffer *buffer,
                                     int *mesh_index, int *hierarchy_index,
                                     int *sdf_index) {
    if (mesh_index)
        *mesh_index = buffer && buffer->selection_valid
            ? buffer->selected_mesh_index : -1;
    if (hierarchy_index)
        *hierarchy_index = buffer && buffer->selection_valid
            ? buffer->selected_hierarchy_index : -1;
    if (sdf_index)
        *sdf_index = buffer && buffer->selection_valid
            ? buffer->selected_sdf_index : -1;
}
