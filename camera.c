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
    cam->active = true;
    cam->use_look_at = false; // Start in normal FPS mode
    
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, cam->up);
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, cam->look_at); // Initialize to zero
    
    camera_update(cam);
}

void camera_update(Camera* cam) {
    if (cam->use_look_at) {
        // ORBIT MODE: Always look at the specified point
        vec3 look_dir;
        glm_vec3_sub(cam->look_at, cam->position, look_dir);
        glm_vec3_normalize_to(look_dir, cam->front);
        
        // Update view matrix to look at the target point
        glm_lookat(cam->position, cam->look_at, cam->up, cam->view_matrix);
    } else {
        // FPS MODE: Traditional behavior
        vec3 front;
        front[0] = cos(glm_rad(cam->yaw)) * cos(glm_rad(cam->pitch));
        front[1] = sin(glm_rad(cam->pitch));
        front[2] = sin(glm_rad(cam->yaw)) * cos(glm_rad(cam->pitch));
        glm_normalize_to(front, cam->front);

        vec3 target;
        glm_vec3_add(cam->position, cam->front, target);
        glm_lookat(cam->position, target, cam->up, cam->view_matrix);
    }

    // Update projection matrix (same for both modes)
    glm_perspective(glm_rad(cam->fov), cam->aspect_ratio, cam->near_plane, cam->far_plane, cam->projection_matrix);
    cam->projection_matrix[1][1] *= -1; // Vulkan correction
}

void camera_set_look_at(Camera* cam, vec3 look_at) {
    glm_vec3_copy(look_at, cam->look_at);
    cam->use_look_at = true;
    printf("Look at point set to: (%.2f, %.2f, %.2f)\n", look_at[0], look_at[1], look_at[2]);
    camera_update(cam);
}

void camera_orbit_around_point(Camera* cam, vec3 pivot_point, float delta_yaw, float delta_pitch) {
    // Calculate current vector from pivot to camera
    vec3 pivot_to_cam;
    glm_vec3_sub(cam->position, pivot_point, pivot_to_cam);
    
    // Get current distance
    float distance = glm_vec3_norm(pivot_to_cam);
    
    // Convert to spherical coordinates
    vec3 dir;
    glm_vec3_normalize_to(pivot_to_cam, dir);
    
    float theta = atan2f(dir[2], dir[0]); // Horizontal angle
    float phi = acosf(dir[1]);            // Vertical angle
    
    // Apply rotation
    theta += delta_yaw;  
    phi += delta_pitch;  
    
    // Clamp vertical angle
    float min_phi = 0.1f;
    float max_phi = GLM_PI - 0.1f;
    if (phi < min_phi) phi = min_phi;
    if (phi > max_phi) phi = max_phi;
    
    // Convert back to Cartesian coordinates
    vec3 new_dir = {
        sinf(phi) * cosf(theta),
        cosf(phi),
        sinf(phi) * sinf(theta)
    };
    
    // Calculate new camera position
    vec3 new_position;
    glm_vec3_scale(new_dir, distance, new_position);
    glm_vec3_add(pivot_point, new_position, new_position);
    
    // Update camera
    glm_vec3_copy(new_position, cam->position);
    
    // UPDATE YAW AND PITCH TO MATCH THE FINAL ORIENTATION
    // Calculate the direction from camera to pivot (opposite of orbit direction)
    vec3 look_dir;
    glm_vec3_sub(pivot_point, cam->position, look_dir);
    glm_vec3_normalize(look_dir);
    
    // Convert look direction to yaw and pitch
    cam->yaw = glm_deg(atan2f(look_dir[2], look_dir[0]));
    cam->pitch = glm_deg(asinf(look_dir[1]));
    
    // Set look at point and enable orbit mode
    camera_set_look_at(cam, pivot_point);
}

/* void camera_orbit_around_point(Camera* cam, vec3 pivot_point, float delta_yaw, float delta_pitch) { */
/*     // Calculate current vector from pivot to camera */
/*     vec3 pivot_to_cam; */
/*     glm_vec3_sub(cam->position, pivot_point, pivot_to_cam); */
    
/*     // Get current distance */
/*     float distance = glm_vec3_norm(pivot_to_cam); */
    
/*     // Convert to spherical coordinates */
/*     vec3 dir; */
/*     glm_vec3_normalize_to(pivot_to_cam, dir); */
    
/*     float theta = atan2f(dir[2], dir[0]); // Horizontal angle */
/*     float phi = acosf(dir[1]);            // Vertical angle */
    
/*     // Apply rotation - INVERT THE SIGNS for intuitive control */
/*     theta -= delta_yaw;  // Changed from + to - */
/*     phi -= delta_pitch;  // Changed from + to - */
    
/*     // Clamp vertical angle */
/*     float min_phi = 0.1f; */
/*     float max_phi = GLM_PI - 0.1f; */
/*     if (phi < min_phi) phi = min_phi; */
/*     if (phi > max_phi) phi = max_phi; */
    
/*     // Convert back to Cartesian coordinates */
/*     vec3 new_dir = { */
/*         sinf(phi) * cosf(theta), */
/*         cosf(phi), */
/*         sinf(phi) * sinf(theta) */
/*     }; */
    
/*     // Calculate new camera position */
/*     vec3 new_position; */
/*     glm_vec3_scale(new_dir, distance, new_position); */
/*     glm_vec3_add(pivot_point, new_position, new_position); */
    
/*     // Update camera */
/*     glm_vec3_copy(new_position, cam->position); */
    
/*     // Set look at point and enable orbit mode */
/*     camera_set_look_at(cam, pivot_point); */
/* } */


void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time) {
    // Only process keyboard if camera is active
    if (!cam->active) return;
    
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
    // Only process mouse if camera is active
    /* if (!cam->active) return; */
    
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
