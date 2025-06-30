#include "camera.h"
#include <stdio.h>

void camera_init(Camera* cam, vec3 position, float yaw, float pitch, float aspect_ratio) {
    glm_vec3_copy(position, cam->position);
    cam->yaw = yaw;
    cam->pitch = pitch;
    cam->movement_speed = 3.0f;
    cam->mouse_sensitivity = 0.1f;
    cam->aspect_ratio = aspect_ratio;
    cam->fov = 45.0f;
    cam->near_plane = 0.1f;
    cam->far_plane = 100.0f;
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, cam->up);
    camera_update(cam);
}

void camera_update(Camera* cam) {
    vec3 front;
    front[0] = cos(glm_rad(cam->yaw)) * cos(glm_rad(cam->pitch));
    front[1] = sin(glm_rad(cam->pitch));
    front[2] = sin(glm_rad(cam->yaw)) * cos(glm_rad(cam->pitch));
    glm_normalize_to(front, cam->front);

    vec3 target;
    glm_vec3_add(cam->position, cam->front, target);
    glm_lookat(cam->position, target, cam->up, cam->view_matrix);

    glm_perspective(glm_rad(cam->fov), cam->aspect_ratio, cam->near_plane, cam->far_plane, cam->projection_matrix);
    cam->projection_matrix[1][1] *= -1; // Vulkan correction
}

void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time) {
    float velocity = cam->movement_speed * delta_time;
    vec3 tmp;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        glm_vec3_scale(cam->front, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        glm_vec3_scale(cam->front, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        glm_cross(cam->front, cam->up, tmp);
        glm_normalize(tmp);
        glm_vec3_scale(tmp, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        glm_cross(cam->front, cam->up, tmp);
        glm_normalize(tmp);
        glm_vec3_scale(tmp, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        glm_vec3_scale(cam->up, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        glm_vec3_scale(cam->up, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
        printf("camera x:%f y:%f z:%f\n", cam->position[0], cam->position[1], cam->position[2]);
    }
}

void camera_process_mouse(Camera* cam, double xoffset, double yoffset) {
    xoffset *= cam->mouse_sensitivity;
    yoffset *= cam->mouse_sensitivity;

    cam->yaw += xoffset;
    printf("cam->yaw == %f\n", cam->yaw);
    cam->pitch += yoffset;
    printf("cam->pitch == %f\n", cam->pitch);

    if(cam->pitch > 89.0f) cam->pitch = 89.0f;
    
    if(cam->pitch < -89.0f) cam->pitch = -89.0f;
    

    camera_update(cam);
}
