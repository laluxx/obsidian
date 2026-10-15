#pragma once

#include "camera.h"
#include "context.h"
#include "scene_buffers.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ObsEditorFrame ObsEditorFrame;

bool obs_editor_frames_init(ObsWindow *primary);
void obs_editor_frames_shutdown(void);
void obs_editor_frames_begin_iteration(void);
void obs_editor_frames_end_iteration(void);
bool obs_editor_frames_iteration_active(void);

ObsEditorFrame *obs_editor_frames_first(void);
ObsEditorFrame *obs_editor_frame_next(ObsEditorFrame *frame);
ObsEditorFrame *obs_editor_frame_current(void);
ObsWindow *obs_editor_frame_window(ObsEditorFrame *frame);
ObsSceneBuffer *obs_editor_frame_buffer(ObsEditorFrame *frame);
bool obs_editor_frame_is_primary(ObsEditorFrame *frame);
bool obs_editor_frame_is_closing(ObsEditorFrame *frame);

void obs_editor_frame_activate(ObsEditorFrame *frame);
bool obs_editor_frames_activate_window(ObsWindow *window);
bool obs_editor_frames_request_window_close(ObsWindow *window);
bool obs_editor_frame_create(ObsSceneBuffer *buffer, bool focus);
bool obs_editor_frame_create_image(const char *path, bool focus);
ObsEditorFrame *obs_editor_frame_create_monad_window(const char *title,
                                                      int width, int height,
                                                      bool focus);
bool obs_editor_frame_set_buffer(ObsEditorFrame *frame,
                                 ObsSceneBuffer *buffer);
bool obs_editor_frame_is_2d_only(ObsEditorFrame *frame);
bool obs_editor_frame_is_monad_window(ObsEditorFrame *frame);
void obs_editor_frame_render_surface(ObsEditorFrame *frame);
bool obs_editor_frame_pointer_button(ObsEditorFrame *frame, int button,
                                     int action, double x, double y);
bool obs_editor_frame_pointer_move(ObsEditorFrame *frame, double x,
                                   double y);
bool obs_editor_frame_scroll(ObsEditorFrame *frame, double x, double y,
                             double amount);
bool obs_editor_frame_image_zoom(float factor);
bool obs_editor_frame_image_fit(void);
void obs_editor_frame_close(ObsEditorFrame *frame);
void obs_editor_frame_close_others(void);
void obs_editor_frame_focus_next(void);
bool obs_editor_frame_close_current(void);
void obs_editor_frame_start_camera_transition(
    CameraFrameTransition *transition, float duration);
bool obs_editor_frame_update_camera_transition(Camera *camera,
                                              float delta_time);
void obs_editor_frame_start_zoom(vec3 position, vec3 delta);
bool obs_editor_frame_update_zoom(Camera *camera, float delta_time);

uint64_t obs_editor_frames_tick(void);
bool obs_editor_frame_is_vertico_owner(void);
void obs_editor_frames_set_vertico_owner(void);
void obs_editor_frames_clear_vertico_owner(void);
