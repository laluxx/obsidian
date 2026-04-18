#include "camera.h"
#include "easing.h"
#include "editor.h"
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
    // Compute dynamic UP vector based on true yaw and pitch for 360-degree rotation
    cam->up[0] = -cos(glm_rad(cam->yaw)) * sin(glm_rad(cam->pitch));
    cam->up[1] = cos(glm_rad(cam->pitch));
    cam->up[2] = -sin(glm_rad(cam->yaw)) * sin(glm_rad(cam->pitch));
    glm_normalize(cam->up);

    if (cam->use_look_at) {
        // ORBIT MODE: Always look at the specified point
        vec3 look_dir;
        glm_vec3_sub(cam->look_at, cam->position, look_dir);
        glm_vec3_normalize_to(look_dir, cam->front);

        // Update view matrix to look at the target point
        glm_lookat(cam->position, cam->look_at, cam->up, cam->view_matrix);
    } else {
        // FPS MODE: Use the dynamic 360-degree UP vector to support inverted panning
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
    camera_update(cam);
}

void camera_orbit_around_point(Camera* cam, vec3 pivot_point, float delta_yaw, float delta_pitch) {
    vec3 pivot_to_cam;
    glm_vec3_sub(cam->position, pivot_point, pivot_to_cam);
    float distance = glm_vec3_norm(pivot_to_cam);

    float old_pitch = cam->pitch;
    cam->yaw += glm_deg(delta_yaw);
    cam->pitch += glm_deg(delta_pitch);

    // Prevent manually crossing the poles, but allow staying in the upside-down hemisphere
    if (old_pitch <= 90.0f && cam->pitch > 90.0f) cam->pitch = 89.9f;
    if (old_pitch >= 90.0f && cam->pitch < 90.0f) cam->pitch = 90.1f;
    if (old_pitch >= -90.0f && cam->pitch < -90.0f) cam->pitch = -89.9f;
    if (old_pitch <= -90.0f && cam->pitch > -90.0f) cam->pitch = -90.1f;

    while (cam->yaw > 180.0f) cam->yaw -= 360.0f;
    while (cam->yaw < -180.0f) cam->yaw += 360.0f;

    float yaw_rad = glm_rad(cam->yaw);
    float pitch_rad = glm_rad(cam->pitch);

    cam->position[0] = pivot_point[0] - distance * cosf(pitch_rad) * cosf(yaw_rad);
    cam->position[1] = pivot_point[1] - distance * sinf(pitch_rad);
    cam->position[2] = pivot_point[2] - distance * cosf(pitch_rad) * sinf(yaw_rad);

    camera_set_look_at(cam, pivot_point);
}

void camera_disable_orbit_mode(Camera* cam) {
    if (cam->use_look_at) {
        printf("Orbit disabled. Angles maintained - yaw: %.1f°, pitch: %.1f°\n", cam->yaw, cam->pitch);
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

    // If the camera is currently upside down, normalize it to the right-side-up equivalent view
    if (cam->pitch > 90.0f) {
        cam->pitch = 180.0f - cam->pitch;
        cam->yaw += 180.0f;
    } else if (cam->pitch < -90.0f) {
        cam->pitch = -180.0f - cam->pitch;
        cam->yaw += 180.0f;
    }

    cam->yaw += xoffset;
    cam->pitch += yoffset;

    // Strict clamp for FPS mode: never allow looking past straight up or straight down
    if (cam->pitch > 89.9f) cam->pitch = 89.9f;
    if (cam->pitch < -89.9f) cam->pitch = -89.9f;

    while (cam->yaw > 180.0f) cam->yaw -= 360.0f;
    while (cam->yaw < -180.0f) cam->yaw += 360.0f;

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
    0.0f,
    45.0f,
    90.0f,
    135.0f,
    180.0f,
    225.0f,
    270.0f,
    315.0f
};

#define NUM_YAW_ANGLES (sizeof(STANDARD_YAW_ANGLES) / sizeof(float))
#define NUM_PITCH_ANGLES (sizeof(STANDARD_PITCH_ANGLES) / sizeof(float))

bool smooth_camera_snap = true;
bool is_snapping = false;
float snap_start_yaw = 0.0f;
float snap_target_yaw = 0.0f;
float snap_start_pitch = 0.0f;
float snap_target_pitch = 0.0f;
float snap_anim_time = 0.0f;
const float SNAP_ANIM_DURATION = 0.25f;
float snap_orbit_distance = 10.0f;
vec3 snap_pivot = {0.0f, 0.0f, 0.0f};

static float normalize_angle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle <= -180.0f) angle += 360.0f;
    return angle;
}

static float lerp_angle(float start, float end, float t) {
    float diff = normalize_angle(end - start);
    return start + diff * t;
}

static void calculate_orbit_position(vec3 position_out, vec3 pivot, float distance, float yaw_deg, float pitch_deg) {
    float yaw_rad = glm_rad(yaw_deg);
    float pitch_rad = glm_rad(pitch_deg);

    // Spherical to Cartesian conversion using inverse view vector
    // This ensures yaw/pitch are universally treated as view angles
    position_out[0] = pivot[0] - distance * cosf(pitch_rad) * cosf(yaw_rad);
    position_out[1] = pivot[1] - distance * sinf(pitch_rad);
    position_out[2] = pivot[2] - distance * cosf(pitch_rad) * sinf(yaw_rad);
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

static float find_next_angle(float current_angle, const float* angles, int num_angles, bool increase) {
    current_angle = normalize_angle(current_angle);
    float best_angle = current_angle;
    float min_diff = 360.0f;

    for (int i = 0; i < num_angles; i++) {
        float diff = normalize_angle(angles[i] - current_angle);

        if (increase) {
            if (diff <= 0.1f) diff += 360.0f;
        } else {
            if (diff >= -0.1f) diff -= 360.0f;
            diff = -diff;
        }

        if (diff < min_diff) {
            min_diff = diff;
            best_angle = angles[i];
        }
    }

    return best_angle;
}

void camera_snap_to_angles(Camera* cam, float target_yaw, float target_pitch, vec3 pivot) {
    vec3 to_cam;
    glm_vec3_sub(cam->position, pivot, to_cam);
    float distance = glm_vec3_norm(to_cam);

    if (smooth_camera_snap) {
        if (is_snapping) {
            float t = snap_anim_time / SNAP_ANIM_DURATION;
            if (t > 1.0f) t = 1.0f;
            float ease_t = ease_quart_out(t);
            snap_start_yaw = lerp_angle(snap_start_yaw, snap_target_yaw, ease_t);
            snap_start_pitch = lerp_angle(snap_start_pitch, snap_target_pitch, ease_t);
        } else {
            // Start fresh from true internal angles
            snap_start_yaw = cam->yaw;
            snap_start_pitch = cam->pitch;
        }

        is_snapping = true;
        snap_target_yaw = target_yaw;
        snap_target_pitch = target_pitch;
        snap_anim_time = 0.0f;
        snap_orbit_distance = distance;
        glm_vec3_copy(pivot, snap_pivot);
    } else {
        cam->yaw = target_yaw;
        cam->pitch = target_pitch;
        calculate_orbit_position(cam->position, pivot, distance, target_yaw, target_pitch);
        camera_set_look_at(cam, pivot);
    }
}

void camera_update_animations(Camera* cam, float delta_time) {
    if (is_snapping) {
        snap_anim_time += delta_time;
        float t = snap_anim_time / SNAP_ANIM_DURATION;
        if (t >= 1.0f) {
            t = 1.0f;
            is_snapping = false;
        }

        float ease_t = ease_quart_out(t);

        cam->yaw = lerp_angle(snap_start_yaw, snap_target_yaw, ease_t);
        cam->pitch = lerp_angle(snap_start_pitch, snap_target_pitch, ease_t);

        calculate_orbit_position(cam->position, snap_pivot, snap_orbit_distance, cam->yaw, cam->pitch);
        camera_set_look_at(cam, snap_pivot);
    }
}

void camera_snap_to_next_angle(Camera* cam, bool forward, bool vertical, vec3 pivot) {
    float base_pitch = is_snapping ? snap_target_pitch : cam->pitch;
    float base_yaw   = is_snapping ? snap_target_yaw   : cam->yaw;

    float target_pitch = base_pitch;
    float target_yaw = base_yaw;

    if (vertical) {
        bool increase = forward;
        target_pitch = find_next_angle(base_pitch, STANDARD_PITCH_ANGLES, NUM_PITCH_ANGLES, increase);
    } else {
        bool increase = forward;
        target_yaw = find_next_angle(base_yaw, STANDARD_YAW_ANGLES, NUM_YAW_ANGLES, increase);
    }
    camera_snap_to_angles(cam, target_yaw, target_pitch, pivot);
}

// Helper to get the correct pivot point for orbiting/snapping
void get_target_pivot(vec3 out_pivot) {
    glm_vec3_zero(out_pivot);
    if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene.meshes.count) {
        Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];
        vec3 local_center;
        glm_vec3_add(m->aabbMin, m->aabbMax, local_center);
        glm_vec3_scale(local_center, 0.5f, local_center);
        vec4 lc4 = {local_center[0], local_center[1], local_center[2], 1.0f};
        vec4 wc4;
        glm_mat4_mulv(m->model, lc4, wc4);
        glm_vec3_copy(wc4, out_pivot);
    }
}

void camera_snap_left()  { vec3 p; get_target_pivot(p); camera_snap_to_next_angle(&camera, true,  false, p); }
void camera_snap_right() { vec3 p; get_target_pivot(p); camera_snap_to_next_angle(&camera, false, false, p); }
void camera_snap_up()    { vec3 p; get_target_pivot(p); camera_snap_to_next_angle(&camera, false, true,  p); }
void camera_snap_down()  { vec3 p; get_target_pivot(p); camera_snap_to_next_angle(&camera, true,  true,  p); }

// Direct snap to standard views
void camera_snap_to_front() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, 180.0f, 0.0f, pivot);
}

void camera_snap_to_back() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, 0.0f, 0.0f, pivot);
}

void camera_snap_to_left() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, 90.0f, 0.0f, pivot);
}

void camera_snap_to_right() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, -90.0f, 0.0f, pivot);
}

void camera_snap_to_top() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, camera.yaw, 90.0f, pivot);
}

void camera_snap_to_bottom() {
    vec3 pivot = {0.0f, 0.0f, 0.0f};
    camera_snap_to_angles(&camera, camera.yaw, -90.0f, pivot);
}

void look_at_world_origin() {
    vec3 world_origin = {0.0f, 0.0f, 0.0f};
    camera_set_look_at(&camera, world_origin);
}

