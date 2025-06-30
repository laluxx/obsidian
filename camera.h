#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <GLFW/glfw3.h>


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
} Camera;

void camera_init(Camera* cam, vec3 position, float yaw, float pitch, float aspect_ratio);
void camera_update(Camera* cam);
void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time);
void camera_process_mouse(Camera* cam, double xoffset, double yoffset);

#endif
