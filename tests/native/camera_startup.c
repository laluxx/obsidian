#include "camera.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void close_to(float actual, float expected) {
    assert(fabsf(actual - expected) < 1.0e-4f);
}

int main(void) {
    const vec3 start = {7.348469f, 6.0f, 7.348469f};
    Camera cam = {0};
    camera_init(&cam, (vec3){start[0], start[1], start[2]},
                -135.0f, -30.0f, 16.0f / 9.0f);

    for (int i = 0; i < 3; ++i) close_to(cam.position[i], start[i]);
    close_to(cam.yaw, -135.0f);
    close_to(cam.pitch, -30.0f);
    assert(cam.use_look_at);

    vec3 toward_origin = {-start[0], -start[1], -start[2]};
    glm_vec3_normalize(toward_origin);
    assert(glm_vec3_dot(cam.front, toward_origin) > 0.9999f);
    assert(cam.up[1] > 0.0f);
    assert(fabsf(glm_vec3_dot(cam.front, cam.up)) < 1.0e-4f);

    vec4 origin_view;
    glm_mat4_mulv(cam.view_matrix, (vec4){0.0f, 0.0f, 0.0f, 1.0f},
                  origin_view);
    close_to(origin_view[0], 0.0f);
    close_to(origin_view[1], 0.0f);
    assert(origin_view[2] < 0.0f);
    assert(cam.projection_matrix[1][1] < 0.0f);

    vec4 world_up_point = {cam.up[0], cam.up[1], cam.up[2], 1.0f};
    vec4 view_up_point;
    vec4 clip_up_point;
    glm_mat4_mulv(cam.view_matrix, world_up_point, view_up_point);
    assert(view_up_point[1] > 0.0f);
    glm_mat4_mulv(cam.projection_matrix, view_up_point, clip_up_point);
    assert(clip_up_point[1] < 0.0f && clip_up_point[3] > 0.0f);

    float starting_height = cam.position[1];
    camera_orbit_around_point(&cam, (vec3){0.0f, 0.0f, 0.0f},
                              0.0f, 0.1f);
    assert(cam.pitch < -30.0f);
    assert(cam.position[1] > starting_height);
    assert(cam.up[1] > 0.0f);

    camera_orbit_around_point(&cam, (vec3){0.0f, 0.0f, 0.0f},
                              0.0f, 10.0f);
    close_to(cam.pitch, -89.9f);
    assert(cam.up[1] > 0.0f);
    camera_orbit_around_point(&cam, (vec3){0.0f, 0.0f, 0.0f},
                              0.0f, -10.0f);
    close_to(cam.pitch, 89.9f);
    assert(cam.up[1] > 0.0f);

    puts("camera startup: requested pose honored, upright orbit, origin centered");
    return 0;
}
