#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>

/// Public interface

typedef enum {
    CAMERA_PERSPECTIVE,
    CAMERA_ORTHOGRAPHIC
} CameraProjectionMode;

typedef struct {
    bool active;
    float start_yaw;
    float target_yaw;
    float start_pitch;
    float target_pitch;
    float time;
    float orbit_distance;
    vec3 pivot;
} CameraSnapTransition;

typedef struct {
     vec3 position;
     vec3 front;
     vec3 up;
    float yaw;
    float pitch;
    float movement_speed;
    float mouse_sensitivity;
     mat4 view_matrix;
     mat4 projection_matrix;
    float fov;
    float aspect_ratio;
    float near_plane;
    float far_plane;
    bool active;
    vec3 look_at;
    bool use_look_at;
    CameraSnapTransition snap_transition;
} Camera;

typedef struct {
    vec3 start_position;
    vec3 target_position;
    vec3 start_look_at;
    vec3 target_look_at;
} CameraFrameTransition;

extern Camera camera;
extern bool smooth_camera_snap;

/* Initializes at the requested world position and angles, initially looking
 * at the world origin in orbit mode. Yaw/pitch must face the target. */
void camera_init(Camera* cam, vec3 position, float yaw, float pitch, float aspect_ratio);
void camera_update(Camera* cam);
void camera_update_animations(Camera* cam, float delta_time);
void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time);
void camera_process_mouse(Camera* cam, double xoffset, double yoffset);
void camera_set_look_at(Camera *cam, vec3 look_at);
void camera_frame_transition_begin(Camera *cam, vec3 target,
                                   float distance,
                                   CameraFrameTransition *transition);
void camera_frame_transition_apply(Camera *cam,
                                   CameraFrameTransition *transition,
                                   float amount);
/* Deltas are radians. Positive delta_pitch raises the orbit camera. Pitch stays
 * in the upright hemisphere instead of crossing a pole and flipping the view. */
void camera_orbit_around_point(Camera* cam, vec3 pivot_point, float delta_yaw, float delta_pitch);
void camera_disable_orbit_mode(Camera* cam);


bool raycast_to_ground(Camera* cam, vec3 hitPoint);

/// GIZMO SNAP

void camera_snap_to_angles(Camera* cam, float target_yaw, float target_pitch, vec3 pivot);
void camera_snap_to_next_angle(Camera* cam, bool forward, bool vertical, vec3 pivot);
void look_at_world_origin();


void camera_snap_left();
void camera_snap_right();
void camera_snap_up();
void camera_snap_down();

#endif
