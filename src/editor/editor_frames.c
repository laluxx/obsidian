#include "editor_frames.h"
#include "camera.h"
#include "easing.h"
#include "editor.h"
#include "image_editor.h"
#include "keychords.h"
#include "live_editor.h"
#include "vertico.h"
#include "window_api.h"
#include "window.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OBS_EDITOR_FRAME_WIDTH 960
#define OBS_EDITOR_FRAME_HEIGHT 640
#define OBS_EDITOR_FRAME_ANIMATION_SECONDS 0.16

typedef struct {
    Panel panels[PANEL_COUNT];
    InspectorState inspector;
    HierarchyState hierarchy;
    float inspector_fs_split;
    float fs_split_target;
    float fs_split_start;
    float fs_split_saved;
    float fs_anim_t;
    bool fs_collapsed;
    double last_tab_click_time;
    vec3 orbit_pivot;
    float orbit_distance;
} ObsEditorWorkspace;

struct ObsEditorFrame {
    ObsWindow *window;
    ObsSceneBuffer *buffer;
    Camera camera;
    CameraFrameTransition camera_transition;
    float camera_transition_duration;
    float camera_transition_time;
    bool camera_transition_active;
    vec3 zoom_start_position;
    vec3 zoom_target_position;
    float zoom_time;
    bool zoom_active;
    ObsEditorWorkspace workspace;
    ObsImageEditor image_editor;
    KeyChordMap local_keymap;
    bool has_local_keymap;
    bool image_surface;
    bool monad_surface;
    bool primary;
    bool closing;
    bool opening;
    int start_x;
    int start_y;
    int target_x;
    int target_y;
    double animation_started;
    struct ObsEditorFrame *next;
};

static ObsEditorFrame *s_frames;
static ObsEditorFrame *s_frames_tail;
static ObsEditorFrame *s_current;
static ObsEditorFrame *s_vertico_owner;
static bool s_initialized;
static bool s_iteration_active;
static uint64_t s_tick;
extern int g_selected_sdf_index;
extern vec3 orbitPivot;
extern float orbitDistance;

static void frame_workspace_store(ObsEditorFrame *frame) {
    if (!frame) return;
    memcpy(frame->workspace.panels, editor.panels,
           sizeof(frame->workspace.panels));
    frame->workspace.inspector = editor.inspector;
    frame->workspace.inspector.selected_mesh_index = -1;
    frame->workspace.hierarchy = editor.hierarchy;
    frame->workspace.hierarchy.selected_index = -1;
    frame->workspace.inspector_fs_split = editor.inspector_fs_split;
    frame->workspace.fs_split_target = editor.fs_split_target;
    frame->workspace.fs_split_start = editor.fs_split_start;
    frame->workspace.fs_split_saved = editor.fs_split_saved;
    frame->workspace.fs_anim_t = editor.fs_anim_t;
    frame->workspace.fs_collapsed = editor.fs_collapsed;
    frame->workspace.last_tab_click_time = editor.last_tab_click_time;
    glm_vec3_copy(orbitPivot, frame->workspace.orbit_pivot);
    frame->workspace.orbit_distance = orbitDistance;
    frame->camera = camera;
    if (frame->buffer)
        obs_scene_buffer_store_selection(
            frame->buffer, editor.inspector.selected_mesh_index,
            editor.hierarchy.selected_index, g_selected_sdf_index);
}

static void frame_workspace_restore(ObsEditorFrame *frame) {
    if (!frame) return;
    memcpy(editor.panels, frame->workspace.panels,
           sizeof(frame->workspace.panels));
    editor.inspector = frame->workspace.inspector;
    editor.hierarchy = frame->workspace.hierarchy;
    editor.inspector_fs_split = frame->workspace.inspector_fs_split;
    editor.fs_split_target = frame->workspace.fs_split_target;
    editor.fs_split_start = frame->workspace.fs_split_start;
    editor.fs_split_saved = frame->workspace.fs_split_saved;
    editor.fs_anim_t = frame->workspace.fs_anim_t;
    editor.fs_collapsed = frame->workspace.fs_collapsed;
    editor.last_tab_click_time = frame->workspace.last_tab_click_time;
    glm_vec3_copy(frame->workspace.orbit_pivot, orbitPivot);
    orbitDistance = frame->workspace.orbit_distance;
    camera = frame->camera;
    obs_scene_buffer_load_selection(
        frame->buffer, &editor.inspector.selected_mesh_index,
        &editor.hierarchy.selected_index, &g_selected_sdf_index);
}

static void frame_append(ObsEditorFrame *frame) {
    if (!s_frames) s_frames = frame;
    else s_frames_tail->next = frame;
    s_frames_tail = frame;
}

static ObsEditorFrame *frame_find_window(ObsWindow *window) {
    for (ObsEditorFrame *frame = s_frames; frame; frame = frame->next)
        if (frame->window == window) return frame;
    return NULL;
}

static void frame_position_near_pointer(ObsWindow *parent, int *x, int *y) {
    int px = 0, py = 0;
    double cx = OBS_EDITOR_FRAME_WIDTH * 0.5;
    double cy = OBS_EDITOR_FRAME_HEIGHT * 0.5;
    GLFWwindow *parent_window = obs_window_glfw(parent);
    if (parent_window) {
        glfwGetWindowPos(parent_window, &px, &py);
        glfwGetCursorPos(parent_window, &cx, &cy);
    }

    int width = OBS_EDITOR_FRAME_WIDTH;
    int height = OBS_EDITOR_FRAME_HEIGHT;
    *x = px + (int)cx - width / 2;
    *y = py + (int)cy - height / 2;

    GLFWmonitor *monitor = parent_window
        ? glfwGetWindowMonitor(parent_window) : NULL;
    if (!monitor) monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    if (mw > width) {
        if (*x < mx) *x = mx;
        if (*x + width > mx + mw) *x = mx + mw - width;
    }
    if (mh > height) {
        if (*y < my) *y = my;
        if (*y + height > my + mh) *y = my + mh - height;
    }
}

static void frame_set_opacity_position(ObsEditorFrame *frame, double now) {
    if (!frame || frame->primary || !frame->window || !frame->window->window)
        return;
    double amount = (now - frame->animation_started) /
                    OBS_EDITOR_FRAME_ANIMATION_SECONDS;
    if (amount < 0.0) amount = 0.0;
    if (amount > 1.0) amount = 1.0;
    float eased = (float)(amount * amount * (3.0 - 2.0 * amount));
    if (frame->closing) eased = 1.0f - eased;
    glfwSetWindowOpacity(frame->window->window, eased);
    int y = frame->target_y +
        (int)lrint((frame->closing ? -18.0 : 18.0) * (1.0f - eased));
    glfwSetWindowPos(frame->window->window, frame->target_x, y);
    if (amount >= 1.0) {
        frame->opening = false;
        if (frame->closing) frame->closing = true;
    }
}

static void frame_destroy(ObsEditorFrame *frame) {
    if (!frame || frame->primary) return;
    if (frame == s_current) {
        frame_workspace_store(frame);
        ObsEditorFrame *replacement = s_frames;
        if (replacement == frame) replacement = frame->next;
        if (!replacement) replacement = s_frames;
        s_current = NULL;
        if (replacement && replacement != frame)
            obs_editor_frame_activate(replacement);
    }
    if (s_vertico_owner == frame) {
        if (vertico.is_active) vertico_quit();
        s_vertico_owner = NULL;
    }
    ObsEditorFrame **link = &s_frames;
    while (*link && *link != frame) link = &(*link)->next;
    if (*link) *link = frame->next;
    if (s_frames_tail == frame) {
        s_frames_tail = NULL;
        for (ObsEditorFrame *it = s_frames; it; it = it->next)
            s_frames_tail = it;
    }
    if (keychord_get_frame_local_map() == &frame->local_keymap)
        keychord_set_frame_local_map(NULL);
    if (frame->has_local_keymap)
        keymap_free(&frame->local_keymap);
    obs_monad_window_frame_destroyed(frame);
    if (frame->image_surface)
        obs_image_editor_close(&frame->image_editor);
    if (frame->window)
        obs_window_destroy(frame->window);
    free(frame);
}

bool obs_editor_frames_init(ObsWindow *primary) {
    if (!primary || !obs_scene_buffer_current() || s_initialized) return false;
    ObsEditorFrame *frame = calloc(1, sizeof(*frame));
    if (!frame) return false;
    frame->window = primary;
    frame->buffer = obs_scene_buffer_current();
    frame->camera = camera;
    frame->primary = true;
    frame_workspace_store(frame);
    frame_append(frame);
    s_current = frame;
    s_initialized = true;
    obs_editor_frame_activate(frame);
    return true;
}

static bool editor_frame_create(ObsSceneBuffer *buffer, bool focus,
                                const char *image_path,
                                const char *monad_title,
                                int requested_width,
                                int requested_height) {
    if (!s_initialized) return false;
    if (!buffer) buffer = obs_scene_buffer_current();
    if (!buffer) return false;
    if (s_current) frame_workspace_store(s_current);

    ObsEditorFrame *frame = calloc(1, sizeof(*frame));
    if (!frame) return false;
    frame->buffer = obs_scene_buffer_current();
    frame->camera = camera;
    frame_workspace_store(frame);
    frame->buffer = buffer;
    frame->has_local_keymap = monad_title == NULL;
    if (frame->has_local_keymap) {
        keymap_init(&frame->local_keymap);
        const char *keymap_name = image_path ? "image-editor" : "frame-local";
        if (!frame->local_keymap.bindings ||
            !obs_monad_live_editor_copy_map(keymap_name,
                                            &frame->local_keymap)) {
            keymap_free(&frame->local_keymap);
            free(frame);
            return false;
        }
    }
    if (image_path) {
        if (!obs_image_editor_open(&frame->image_editor, image_path)) {
            keymap_free(&frame->local_keymap);
            free(frame);
            return false;
        }
        frame->image_surface = true;
    } else {
        frame->image_editor.texture_index = -1;
        frame->monad_surface = monad_title != NULL;
    }

    char title[192];
    if (monad_title) {
        snprintf(title, sizeof(title), "%.191s", monad_title);
    } else if (image_path) {
        const char *base = strrchr(image_path, '/');
        snprintf(title, sizeof(title), "Obsidian — Image — %.120s",
                 base ? base + 1 : image_path);
    } else {
        const char *path = obs_scene_buffer_path(buffer);
        const char *extension = path ? strrchr(path, '.') : NULL;
        const char *kind =
            extension && strcasecmp(extension, ".obj") == 0 ? "OBJ Preview"
          : extension && (strcasecmp(extension, ".gltf") == 0 ||
                          strcasecmp(extension, ".glb") == 0)
              ? "glTF Inspector"
              : NULL;
        if (kind)
            snprintf(title, sizeof(title), "Obsidian — %s — %.120s", kind,
                     obs_scene_buffer_name(buffer));
        else
            snprintf(title, sizeof(title), "Obsidian — %.150s",
                     obs_scene_buffer_name(buffer));
    }
    const int width = monad_title
        ? (requested_width < 64 ? 64 :
           requested_width > 4096 ? 4096 : requested_width)
        : OBS_EDITOR_FRAME_WIDTH;
    const int height = monad_title
        ? (requested_height < 64 ? 64 :
           requested_height > 4096 ? 4096 : requested_height)
        : OBS_EDITOR_FRAME_HEIGHT;
    ObsWindow *window = obs_window_create_hidden(width, height, title);
    if (!window) {
        keymap_free(&frame->local_keymap);
        if (frame->image_surface)
            obs_image_editor_close(&frame->image_editor);
        free(frame);
        return false;
    }
    frame->window = window;
    window->render2DOnly = frame->image_surface || frame->monad_surface;
    ObsWindow *parent = s_current ? s_current->window : context.primary;
    frame_position_near_pointer(parent, &frame->target_x, &frame->target_y);
    frame->start_x = frame->target_x;
    frame->start_y = frame->target_y + 18;
    frame->opening = true;
    frame->animation_started = glfwGetTime();
    glfwSetWindowPos(window->window, frame->start_x, frame->start_y);
    glfwSetWindowOpacity(window->window, 0.0f);
    glfwShowWindow(window->window);
    frame_append(frame);
    if (focus) {
        obs_window_focus(window);
        obs_editor_frame_activate(frame);
    }
    return true;
}

bool obs_editor_frame_create(ObsSceneBuffer *buffer, bool focus) {
    return editor_frame_create(buffer, focus, NULL, NULL, 0, 0);
}

bool obs_editor_frame_create_image(const char *path, bool focus) {
    return path && *path &&
           editor_frame_create(obs_scene_buffer_current(), focus, path,
                               NULL, 0, 0);
}

ObsEditorFrame *obs_editor_frame_create_monad_window(const char *title,
                                                     int width, int height,
                                                     bool focus) {
    if (!title || !*title ||
        !editor_frame_create(obs_scene_buffer_current(), focus, NULL, title,
                             width, height))
        return NULL;
    return s_frames_tail;
}

bool obs_editor_frame_set_buffer(ObsEditorFrame *frame,
                                 ObsSceneBuffer *buffer) {
    if (!frame || !buffer) return false;
    if (s_current == frame) frame_workspace_store(frame);
    ObsSceneBuffer *previous = frame->buffer;
    frame->buffer = buffer;
    if (!obs_scene_buffer_activate(buffer)) {
        frame->buffer = previous;
        return false;
    }
    if (s_current == frame) {
        obs_scene_buffer_load_selection(
            buffer, &editor.inspector.selected_mesh_index,
            &editor.hierarchy.selected_index, &g_selected_sdf_index);
        frame_workspace_store(frame);
    }
    return true;
}

void obs_editor_frame_activate(ObsEditorFrame *frame) {
    if (!frame || frame == s_current) return;
    frame_workspace_store(s_current);
    s_current = frame;
    obs_scene_buffer_activate(frame->buffer);
    frame_workspace_restore(frame);
    obs_window_set_current(frame->window);
    keychord_set_frame_local_map(
        frame->has_local_keymap ? &frame->local_keymap : NULL);
}

bool obs_editor_frames_activate_window(ObsWindow *window) {
    if (!s_initialized || !window) return true;
    ObsEditorFrame *frame = frame_find_window(window);
    if (!frame || frame->closing) return false;
    if (vertico.is_active && s_vertico_owner && s_vertico_owner != frame)
        vertico_quit();
    obs_editor_frame_activate(frame);
    return true;
}

bool obs_editor_frame_is_2d_only(ObsEditorFrame *frame) {
    return frame && (frame->image_surface || frame->monad_surface);
}

void obs_editor_frame_render_surface(ObsEditorFrame *frame) {
    if (frame && frame->image_surface)
        obs_image_editor_render(&frame->image_editor);
    else if (frame && frame->monad_surface)
        obs_monad_window_render(frame);
}
bool obs_editor_frame_is_monad_window(ObsEditorFrame *frame) {
    return frame && frame->monad_surface;
}

bool obs_editor_frame_pointer_button(ObsEditorFrame *frame, int button,
                                     int action, double x, double y) {
    if (!frame || !frame->image_surface) return false;
    obs_image_editor_pointer_button(&frame->image_editor, button, action, x, y);
    return true;
}

bool obs_editor_frame_pointer_move(ObsEditorFrame *frame, double x,
                                   double y) {
    if (!frame || !frame->image_surface) return false;
    obs_image_editor_pointer_move(&frame->image_editor, x, y);
    return true;
}

bool obs_editor_frame_scroll(ObsEditorFrame *frame, double x, double y,
                             double amount) {
    if (!frame || !frame->image_surface) return false;
    obs_image_editor_scroll(&frame->image_editor, x, y, amount);
    return true;
}

bool obs_editor_frame_image_zoom(float factor) {
    if (!s_current || !s_current->image_surface) return false;
    obs_image_editor_zoom(&s_current->image_editor, factor);
    return true;
}

bool obs_editor_frame_image_fit(void) {
    if (!s_current || !s_current->image_surface) return false;
    obs_image_editor_fit(&s_current->image_editor);
    return true;
}

bool obs_editor_frames_request_window_close(ObsWindow *window) {
    ObsEditorFrame *frame = frame_find_window(window);
    if (!frame || frame->primary) return false;
    obs_editor_frame_close(frame);
    glfwSetWindowShouldClose(window->window, GLFW_FALSE);
    return true;
}

void obs_editor_frame_close(ObsEditorFrame *frame) {
    if (!frame || frame->primary || frame->closing) return;
    frame->opening = false;
    frame->closing = true;
    frame->animation_started = glfwGetTime();
}

void obs_editor_frame_close_others(void) {
    for (ObsEditorFrame *frame = s_frames; frame; frame = frame->next)
        if (frame != s_current && !frame->primary)
            obs_editor_frame_close(frame);
}

void obs_editor_frame_focus_next(void) {
    if (!s_current || !s_frames) return;
    ObsEditorFrame *candidate = s_current->next ? s_current->next : s_frames;
    while (candidate != s_current && candidate->closing)
        candidate = candidate->next ? candidate->next : s_frames;
    if (candidate == s_current || candidate->closing) return;
    obs_window_focus(candidate->window);
    obs_editor_frame_activate(candidate);
}

bool obs_editor_frame_close_current(void) {
    if (!s_current || s_current->primary) return false;
    obs_editor_frame_close(s_current);
    return true;
}

void obs_editor_frame_start_camera_transition(
    CameraFrameTransition *transition, float duration) {
    if (!s_current || !transition) return;
    s_current->zoom_active = false;
    s_current->camera_transition = *transition;
    s_current->camera_transition_duration = duration > 0.0f ? duration : 0.01f;
    s_current->camera_transition_time = 0.0f;
    s_current->camera_transition_active = true;
}

bool obs_editor_frame_update_camera_transition(Camera *active_camera,
                                              float delta_time) {
    if (!s_current || !active_camera ||
        !s_current->camera_transition_active)
        return false;
    s_current->camera_transition_time += delta_time;
    float amount = s_current->camera_transition_time /
                   s_current->camera_transition_duration;
    if (amount >= 1.0f) {
        amount = 1.0f;
        s_current->camera_transition_active = false;
    }
    camera_frame_transition_apply(active_camera,
                                  &s_current->camera_transition,
                                  ease_expo_out(amount));
    return true;
}

void obs_editor_frame_start_zoom(vec3 position, vec3 delta) {
    if (!s_current || !position || !delta) return;
    if (s_current->camera_transition_active) return;
    if (!s_current->zoom_active)
        glm_vec3_copy(position, s_current->zoom_target_position);
    glm_vec3_copy(position, s_current->zoom_start_position);
    glm_vec3_add(s_current->zoom_target_position, delta,
                 s_current->zoom_target_position);
    s_current->zoom_time = 0.0f;
    s_current->zoom_active = true;
}

bool obs_editor_frame_update_zoom(Camera *active_camera, float delta_time) {
    if (!s_current || !active_camera || !s_current->zoom_active) return false;
    s_current->zoom_time += delta_time;
    float amount = s_current->zoom_time / 0.15f;
    if (amount >= 1.0f) {
        amount = 1.0f;
        s_current->zoom_active = false;
    }
    glm_vec3_lerp(s_current->zoom_start_position,
                  s_current->zoom_target_position,
                  ease_quart_out(amount), active_camera->position);
    return true;
}

void obs_editor_frames_begin_iteration(void) {
    if (!s_initialized) return;
    obs_poll_events();
    if (++s_tick == 0) ++s_tick;
    const double now = glfwGetTime();
    ObsEditorFrame *frame = s_frames;
    while (frame) {
        ObsEditorFrame *next = frame->next;
        if (!frame->primary && glfwWindowShouldClose(frame->window->window)) {
            frame->closing = true;
            frame->opening = false;
            frame->animation_started = now;
            glfwSetWindowShouldClose(frame->window->window, GLFW_FALSE);
        }
        if (!frame->primary && (frame->opening || frame->closing)) {
            frame_set_opacity_position(frame, now);
            if (frame->closing &&
                now - frame->animation_started >=
                    OBS_EDITOR_FRAME_ANIMATION_SECONDS)
                frame_destroy(frame);
        }
        frame = next;
    }

    ObsEditorFrame *focused = NULL;
    for (frame = s_frames; frame; frame = frame->next) {
        if (!frame->closing && frame->window &&
            glfwGetWindowAttrib(frame->window->window, GLFW_FOCUSED)) {
            focused = frame;
            break;
        }
    }
    if (!focused && s_current && !s_current->closing) focused = s_current;
    if (!focused) focused = s_frames;
    if (focused) obs_editor_frame_activate(focused);
    s_iteration_active = true;
}

void obs_editor_frames_end_iteration(void) {
    if (!s_initialized) return;
    frame_workspace_store(s_current);
    ObsEditorFrame *focused = NULL;
    for (ObsEditorFrame *frame = s_frames; frame; frame = frame->next) {
        if (!frame->closing && frame->window &&
            glfwGetWindowAttrib(frame->window->window, GLFW_FOCUSED)) {
            focused = frame;
            break;
        }
    }
    if (focused) obs_editor_frame_activate(focused);
    s_iteration_active = false;
}

void obs_editor_frames_shutdown(void) {
    if (!s_initialized) return;
    if (vertico.is_active) vertico_quit();
    s_vertico_owner = NULL;
    keychord_set_frame_local_map(NULL);
    ObsEditorFrame *frame = s_frames;
    while (frame) {
        ObsEditorFrame *next = frame->next;
        if (frame->has_local_keymap)
            keymap_free(&frame->local_keymap);
        obs_monad_window_frame_destroyed(frame);
        if (frame->image_surface)
            obs_image_editor_close(&frame->image_editor);
        if (!frame->primary && frame->window)
            obs_window_destroy(frame->window);
        free(frame);
        frame = next;
    }
    s_frames = NULL;
    s_frames_tail = NULL;
    s_current = NULL;
    s_initialized = false;
    s_iteration_active = false;
    s_tick = 0;
}

bool obs_editor_frames_iteration_active(void) { return s_iteration_active; }
ObsEditorFrame *obs_editor_frames_first(void) { return s_frames; }
ObsEditorFrame *obs_editor_frame_next(ObsEditorFrame *frame) {
    return frame ? frame->next : NULL;
}
ObsEditorFrame *obs_editor_frame_current(void) { return s_current; }
ObsWindow *obs_editor_frame_window(ObsEditorFrame *frame) {
    return frame ? frame->window : NULL;
}
ObsSceneBuffer *obs_editor_frame_buffer(ObsEditorFrame *frame) {
    return frame ? frame->buffer : NULL;
}
bool obs_editor_frame_is_primary(ObsEditorFrame *frame) {
    return frame && frame->primary;
}
bool obs_editor_frame_is_closing(ObsEditorFrame *frame) {
    return !frame || frame->closing;
}
uint64_t obs_editor_frames_tick(void) { return s_tick; }
bool obs_editor_frame_is_vertico_owner(void) {
    return s_current && s_vertico_owner == s_current;
}
void obs_editor_frames_set_vertico_owner(void) { s_vertico_owner = s_current; }
void obs_editor_frames_clear_vertico_owner(void) { s_vertico_owner = NULL; }
