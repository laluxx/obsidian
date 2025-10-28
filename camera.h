#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <GLFW/glfw3.h>
#include <stdbool.h>


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
} Camera;

void camera_init(Camera* cam, vec3 position, float yaw, float pitch, float aspect_ratio);
void camera_update(Camera* cam);
void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time);
void camera_process_mouse(Camera* cam, double xoffset, double yoffset);
void camera_set_look_at(Camera *cam, vec3 look_at);
void camera_orbit_around_point(Camera* cam, vec3 pivot_point, float delta_yaw, float delta_pitch);
void camera_disable_orbit_mode(Camera* cam);


/// GIZMO SNAP

void camera_snap_to_next_angle(Camera* cam, bool forward, bool vertical);

#endif
