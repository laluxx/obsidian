#include "camera.h"
#include <stdio.h>

Camera camera;

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

void camera_disable_orbit_mode(Camera* cam) {
    if (cam->use_look_at) {
        // Before disabling orbit mode, sync yaw/pitch to match current view direction
        // This prevents camera jumps when transitioning from orbit to FPS mode
        
        // cam->front is already pointing in the correct direction (from orbit mode)
        // Calculate yaw and pitch from the front vector
        vec3 front_normalized;
        glm_vec3_copy(cam->front, front_normalized);
        glm_vec3_normalize(front_normalized);
        
        // Calculate yaw (horizontal angle)
        // yaw = atan2(z, x) where front = (x, y, z)
        cam->yaw = glm_deg(atan2f(front_normalized[2], front_normalized[0]));
        
        // Calculate pitch (vertical angle)
        // pitch = asin(y) where front = (x, y, z) normalized
        cam->pitch = glm_deg(asinf(front_normalized[1]));
        
        printf("Synced camera angles - yaw: %.1f°, pitch: %.1f°\n", cam->yaw, cam->pitch);
    }
    
    cam->use_look_at = false;
    camera_update(cam);
}

void camera_process_keyboard(Camera* cam, GLFWwindow* window, float delta_time) {
    if (!cam->active) return;
    
    float velocity = cam->movement_speed * delta_time;
    vec3 tmp;
    
    // Check if any movement key is pressed
    bool movement_key_pressed = 
        glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    
    // When moving with WASD in FPS mode, disable orbit mode
    if (movement_key_pressed && cam->use_look_at) {
        camera_disable_orbit_mode(cam);
        printf("Movement detected - orbit mode disabled\n");
    }

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        glm_vec3_scale(cam->front, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        glm_vec3_scale(cam->front, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        glm_cross(cam->front, cam->up, tmp);
        glm_normalize(tmp);
        glm_vec3_scale(tmp, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        glm_cross(cam->front, cam->up, tmp);
        glm_normalize(tmp);
        glm_vec3_scale(tmp, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        glm_vec3_scale(cam->up, velocity, tmp);
        glm_vec3_add(cam->position, tmp, cam->position);
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
        glm_vec3_scale(cam->up, velocity, tmp);
        glm_vec3_sub(cam->position, tmp, cam->position);
    }
}

void camera_process_mouse(Camera* cam, double xoffset, double yoffset) {
    xoffset *= cam->mouse_sensitivity;
    yoffset *= cam->mouse_sensitivity;

    // Disable orbit mode when manually moving mouse (any movement)
    if (cam->use_look_at && (fabs(xoffset) > 0.01 || fabs(yoffset) > 0.01)) {
        camera_disable_orbit_mode(cam);
        printf("Orbit mode disabled - manual camera rotation\n");
    }

    cam->yaw += xoffset;
    cam->pitch += yoffset;

    if(cam->pitch > 90.0f) cam->pitch = 90.0f;
    if(cam->pitch < -90.0f) cam->pitch = -90.0f;

    camera_update(cam);
}


/// GIZMO SNAP

static const float STANDARD_YAW_ANGLES[] = {
    0.0f,    // Front (looking from +X towards origin)
    45.0f,
    90.0f,   // Right side
    135.0f,
    180.0f,  // Back (looking from -X towards origin)
    -135.0f,
    -90.0f,  // Left side
    -45.0f
};

static const float STANDARD_PITCH_ANGLES[] = {
    90.9f,    // Bottom view (looking UP at origin)
    45.0f,
    0.0f,     // Eye level with origin
    -45.0f,
    -90.9f    // Top view (looking DOWN at origin)
};


#define NUM_YAW_ANGLES (sizeof(STANDARD_YAW_ANGLES) / sizeof(float))
#define NUM_PITCH_ANGLES (sizeof(STANDARD_PITCH_ANGLES) / sizeof(float))

static float normalize_angle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

static void calculate_orbit_position(vec3 position_out, float distance, float yaw_deg, float pitch_deg) {
    float yaw_rad = glm_rad(yaw_deg);
    float pitch_rad = glm_rad(pitch_deg);
    
    // Spherical to Cartesian conversion
    // Position camera at given angles, looking back at origin
    position_out[0] = distance * cosf(pitch_rad) * cosf(yaw_rad);
    position_out[1] = distance * sinf(pitch_rad);
    position_out[2] = distance * cosf(pitch_rad) * sinf(yaw_rad);
}

bool raycast_to_ground(Camera* cam, vec3 hitPoint) {
    vec3 rayOrigin, rayDir;
    glm_vec3_copy(cam->position, rayOrigin);
    glm_vec3_copy(cam->front, rayDir);
    glm_vec3_normalize(rayDir);
    
    printf("=== RAYCAST DEBUG ===\n");
    printf("Camera pos: (%.2f, %.2f, %.2f)\n", rayOrigin[0], rayOrigin[1], rayOrigin[2]);
    printf("Camera dir: (%.2f, %.2f, %.2f)\n", rayDir[0], rayDir[1], rayDir[2]);
    
    // Check if ray is parallel to ground (no Y component)
    if (fabs(rayDir[1]) < 0.0001f) {
        printf("Ray parallel to ground - using camera XZ position\n");
        hitPoint[0] = rayOrigin[0];
        hitPoint[1] = 0.0f;
        hitPoint[2] = rayOrigin[2];
        return false;
    }
    
    // Calculate intersection with plane y = 0
    // t = -rayOrigin.y / rayDir.y
    float t = -rayOrigin[1] / rayDir[1];
    
    printf("t parameter: %.2f\n", t);
    
    if (t < 0.0f) {
        printf("Intersection behind camera\n");
        // Project camera position onto ground plane
        hitPoint[0] = rayOrigin[0];
        hitPoint[1] = 0.0f;
        hitPoint[2] = rayOrigin[2];
        return false;
    }
    
    // Calculate hit point
    hitPoint[0] = rayOrigin[0] + rayDir[0] * t;
    hitPoint[1] = 0.0f;
    hitPoint[2] = rayOrigin[2] + rayDir[2] * t;
    
    printf("Ground intersection: (%.2f, %.2f, %.2f), Distance: %.2f\n", 
           hitPoint[0], hitPoint[1], hitPoint[2], t);
    
    return true;
}

static float find_next_angle(float current_angle, const float* angles, int num_angles, bool forward) {
    current_angle = normalize_angle(current_angle);
    
    float best_angle = angles[0];
    float min_distance = 360.0f;
    bool found = false;
    
    for (int i = 0; i < num_angles; i++) {
        float angle = angles[i];
        float distance;
        
        if (forward) {
            // UP arrow: move toward more positive pitch (from top to bottom)
            distance = normalize_angle(angle - current_angle);
            if (distance <= 0.0f) distance += 360.0f;
        } else {
            // DOWN arrow: move toward more negative pitch (from bottom to top)  
            distance = normalize_angle(current_angle - angle);
            if (distance <= 0.0f) distance += 360.0f;
        }
        
        // Find the smallest positive distance
        if (distance > 0.1f && distance < min_distance) {
            min_distance = distance;
            best_angle = angle;
            found = true;
        }
    }
    
    // If no suitable angle found, wrap around
    if (!found) {
        if (forward) {
            // Wrap to first angle (bottom) when going positive
            best_angle = angles[0];
        } else {
            // Wrap to last angle (top) when going negative
            best_angle = angles[num_angles - 1];
        }
    }
    
    return best_angle;
}

void camera_snap_to_next_angle(Camera* cam, bool forward, bool vertical) {
    // Get current distance from world origin
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, cam->position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    // Find next angle
    if (vertical) {
        // Snap pitch (vertical orbit)
        float next_pitch = find_next_angle(cam->pitch, STANDARD_PITCH_ANGLES, 
                                          NUM_PITCH_ANGLES, forward);
        cam->pitch = next_pitch;
        /* printf("Snapped pitch to: %.1f°\n", next_pitch); */
    } else {
        // Snap yaw (horizontal orbit)
        float next_yaw = find_next_angle(cam->yaw, STANDARD_YAW_ANGLES, 
                                        NUM_YAW_ANGLES, forward);
        cam->yaw = next_yaw;
        /* printf("Snapped yaw to: %.1f°\n", next_yaw); */
    }
    
    // Calculate new camera position at this angle, maintaining distance
    calculate_orbit_position(cam->position, distance, cam->yaw, cam->pitch);
    
    // Set to look at world origin
    camera_set_look_at(cam, world_origin);
    
    /* printf("Camera position: (%.2f, %.2f, %.2f), distance: %.2f\n", */
    /*        cam->position[0], cam->position[1], cam->position[2], distance); */
}


void camera_snap_left()  {camera_snap_to_next_angle(&camera, true, false);}
void camera_snap_right() {camera_snap_to_next_angle(&camera, false, false);}
void camera_snap_up()    {camera_snap_to_next_angle(&camera, true, true);}
void camera_snap_down()  {camera_snap_to_next_angle(&camera, false, true);}


// Direct snap to standard views - all orbit around world origin
void camera_snap_to_front() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to FRONT view (orbiting world origin)\n");
}

void camera_snap_to_back() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    camera.yaw = 180.0f;
    camera.pitch = 0.0f;
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to BACK view (orbiting world origin)\n");
}

void camera_snap_to_left() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    camera.yaw = -90.0f;
    camera.pitch = 0.0f;
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to LEFT view (orbiting world origin)\n");
}

void camera_snap_to_right() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    camera.yaw = 90.0f;
    camera.pitch = 0.0f;
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to RIGHT view (orbiting world origin)\n");
}

void camera_snap_to_top() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    // For top view, we want to look DOWN at the origin
    // This means the camera should be ABOVE looking DOWN
    camera.pitch = -90.9f; // Just shy of exactly -90 to avoid gimbal lock
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to TOP view (orbiting world origin)\n");
}

void camera_snap_to_bottom() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    vec3 to_origin;
    glm_vec3_sub(world_origin, camera.position, to_origin);
    float distance = glm_vec3_norm(to_origin);
    
    // For bottom view, we want to look UP at the origin
    // This means the camera should be BELOW looking UP
    camera.pitch = 90.9f; // Just shy of exactly 90 to avoid gimbal lock
    calculate_orbit_position(camera.position, distance, camera.yaw, camera.pitch);
    camera_set_look_at(&camera, world_origin);
    printf("Snapped to BOTTOM view (orbiting world origin)\n");
}

void look_at_world_origin() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    camera_set_look_at(&camera, world_origin);
}

