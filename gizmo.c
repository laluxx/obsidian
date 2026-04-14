#include "gizmo.h"
#include "editor.h"
#include "camera.h"
#include "scene.h"
#include "context.h"
#include "renderer.h"
#include "keychords.h"
#include "theme.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

extern void markMeshesSSBODirty(void* ctx);

///  Tunables

#define GIZMO_SCREEN_SIZE     120.0f   // gizmo radius in screen pixels
#define GIZMO_ARROW_TIP       0.80f    // tip of arrow as fraction of radius
#define GIZMO_ARROW_HEAD_LEN  0.14f    // length of arrow head
#define GIZMO_ARROW_HEAD_W    0.07f    // half-width of arrow head
#define GIZMO_PLANE_OFFSET    0.28f    // where the plane square starts (fraction)
#define GIZMO_PLANE_SIZE      0.18f    // size of plane square (fraction)
#define GIZMO_ARC_SEGMENTS    48       // smoothness of rotation arc
#define GIZMO_ARC_HALF_DEG    110.0f   // arc spans ±110° (Godot: ~220° facing camera)
#define GIZMO_AXIS_HIT_PX     10.0f    // pixel tolerance for axis hit test
#define GIZMO_PLANE_HIT_PX    14.0f    // pixel tolerance for plane hit test
#define GIZMO_ARC_HIT_PX      12.0f    // pixel tolerance for arc hit test

GizmoState gizmo = {0};

//  Math helpers
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Project a 3-D world point to 2-D screen coordinates.
// Returns false if the point is behind the camera.
static bool world_to_screen(vec3 world, vec2 out_screen) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    mat4 vp;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, vp);

    vec4 clip;
    vec4 w4 = {world[0], world[1], world[2], 1.0f};
    glm_mat4_mulv(vp, w4, clip);

    if (clip[3] <= 0.0f) return false;

    float ndcX = clip[0] / clip[3];
    float ndcY = clip[1] / clip[3];

    // Vulkan NDC Y goes down (-1 top, 1 bottom). We want screen Y up (0 bottom, sh top).
    out_screen[0] = (ndcX * 0.5f + 0.5f) * sw;
    out_screen[1] = (0.5f - ndcY * 0.5f) * sh;
    return true;
}

// Unproject a screen pixel to a world-space ray.
static void screen_to_ray(double sx, double sy, vec3 out_origin, vec3 out_dir) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    float ndcX =  (2.0f * (float)sx / sw) - 1.0f;
    float ndcY =  1.0f - (2.0f * (float)sy / sh);

    mat4 inv_vp;
    mat4 vp;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, vp);
    glm_mat4_inv(vp, inv_vp);

    // If using standard cglm projection (glm_perspective), the clip depth is [-1, 1].
    // Unprojecting from 0.0 starts the ray halfway into the scene, missing front objects!
    vec4 near4 = {ndcX, ndcY, -1.0f, 1.0f};
    vec4 far4  = {ndcX, ndcY,  1.0f, 1.0f};
    vec4 near_w, far_w;
    glm_mat4_mulv(inv_vp, near4, near_w);
    glm_mat4_mulv(inv_vp, far4,  far_w);

    if (near_w[3] != 0.0f) glm_vec4_scale(near_w, 1.0f / near_w[3], near_w);
    if (far_w[3]  != 0.0f) glm_vec4_scale(far_w,  1.0f / far_w[3],  far_w);

    glm_vec3_copy((vec3){near_w[0], near_w[1], near_w[2]}, out_origin);
    vec3 dir;
    glm_vec3_sub((vec3){far_w[0], far_w[1], far_w[2]}, out_origin, dir);
    glm_vec3_normalize(dir);
    glm_vec3_copy(dir, out_dir);
}

// Ray vs AABB intersection. Returns t (distance) or -1 on miss.
static float ray_aabb(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {
    float tmin = -1e9f, tmax = 1e9f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(rd[i]) < 1e-7f) {
            if (ro[i] < bmin[i] || ro[i] > bmax[i]) return -1.0f;
        } else {
            float t1 = (bmin[i] - ro[i]) / rd[i];
            float t2 = (bmax[i] - ro[i]) / rd[i];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            tmin = t1 > tmin ? t1 : tmin;
            tmax = t2 < tmax ? t2 : tmax;
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin > 0.0f ? tmin : tmax;
}

// 2-D point-to-segment distance (screen space)
static float pt_seg_dist2d(vec2 p, vec2 a, vec2 b) {
    vec2 ab = {b[0]-a[0], b[1]-a[1]};
    float len2 = ab[0]*ab[0] + ab[1]*ab[1];
    if (len2 < 1e-6f) return sqrtf((p[0]-a[0])*(p[0]-a[0])+(p[1]-a[1])*(p[1]-a[1]));
    float t = clampf(((p[0]-a[0])*ab[0]+(p[1]-a[1])*ab[1])/len2, 0.0f, 1.0f);
    float qx = a[0]+t*ab[0]-p[0];
    float qy = a[1]+t*ab[1]-p[1];
    return sqrtf(qx*qx+qy*qy);
}

//  Compute gizmo scale so it stays constant on screen regardless of distance
static float gizmo_world_scale(vec3 origin) {
    // Distance from camera to gizmo origin
    vec3 diff;
    glm_vec3_sub(origin, camera.position, diff);
    float dist = glm_vec3_norm(diff);

    // How many world units correspond to GIZMO_SCREEN_SIZE pixels at this distance?
    float sh = (float)context.swapChainExtent.height;
    // fov_y from projection matrix: P[1][1] = 1/tan(fovy/2)
    float inv_tan_half_fov = camera.projection_matrix[1][1];
    float world_per_pixel = (2.0f * dist) / (sh * inv_tan_half_fov);
    return GIZMO_SCREEN_SIZE * world_per_pixel;
}

/// Draw

// Draw an arrow (shaft + arrowhead cone approximated as fan) in world space.
// axis: unit direction, origin: gizmo center, scale: world units for radius 1
static void draw_arrow(vec3 origin, vec3 axis, float scale, Color col) {
    float tip_t  = GIZMO_ARROW_TIP;
    float head_l = GIZMO_ARROW_HEAD_LEN;
    float head_w = GIZMO_ARROW_HEAD_W;

    vec3 tip, base_head;
    glm_vec3_copy(origin, tip);
    glm_vec3_muladds(axis, tip_t * scale, tip);
    glm_vec3_copy(origin, base_head);
    glm_vec3_muladds(axis, (tip_t - head_l) * scale, base_head);

    // Shaft
    line(origin, base_head, col);

    // Build two perpendicular vectors to axis for the arrowhead fan
    vec3 perp0, perp1;
    // Find a vector not parallel to axis
    vec3 up = {0,1,0};
    if (fabsf(axis[1]) > 0.9f) { up[0]=1; up[1]=0; }
    glm_vec3_cross(axis, up, perp0);
    glm_vec3_normalize(perp0);
    glm_vec3_cross(axis, perp0, perp1);
    glm_vec3_normalize(perp1);

    const int SEGMENTS = 8;
    for (int i = 0; i < SEGMENTS; i++) {
        float a0 = (float)i     / SEGMENTS * GLM_PI * 2.0f;
        float a1 = (float)(i+1) / SEGMENTS * GLM_PI * 2.0f;
        vec3 rim0, rim1;
        for (int k=0;k<3;k++) {
            rim0[k] = base_head[k] + (cosf(a0)*perp0[k] + sinf(a0)*perp1[k]) * head_w * scale;
            rim1[k] = base_head[k] + (cosf(a1)*perp0[k] + sinf(a1)*perp1[k]) * head_w * scale;
        }
        triangle(tip, rim0, rim1, col);
    }
}

// Draw a 3-D arc (rotation handle).  normal: plane normal, up: start direction.
// arc spans ±GIZMO_ARC_HALF_DEG around the side facing the camera.
static void draw_arc(vec3 origin, vec3 normal, float scale, Color col) {
    // Build two orthogonal vectors in the plane
    vec3 u, v;
    vec3 ref = {0,1,0};
    if (fabsf(normal[1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
    glm_vec3_cross(normal, ref, u); glm_vec3_normalize(u);
    glm_vec3_cross(normal, u,   v); glm_vec3_normalize(v);

    // Rotate start so the arc faces the camera
    vec3 to_cam;
    glm_vec3_sub(camera.position, origin, to_cam);
    // Project to_cam onto the arc plane (remove component along normal)
    float dot = glm_vec3_dot(to_cam, normal);
    vec3 in_plane;
    glm_vec3_copy(to_cam, in_plane);
    glm_vec3_muladds(normal, -dot, in_plane);
    glm_vec3_normalize(in_plane);

    // Start angle = direction of in_plane in the (u,v) frame
    float start_angle = atan2f(glm_vec3_dot(in_plane, v), glm_vec3_dot(in_plane, u));
    float half_rad    = GLM_PI * GIZMO_ARC_HALF_DEG / 180.0f;
    float angle_start = start_angle - half_rad;
    float angle_end   = start_angle + half_rad;

    vec3 prev;
    for (int i = 0; i <= GIZMO_ARC_SEGMENTS; i++) {
        float t   = (float)i / GIZMO_ARC_SEGMENTS;
        float ang = lerpf(angle_start, angle_end, t);
        vec3 pt;
        for (int k=0;k<3;k++)
            pt[k] = origin[k] + (cosf(ang)*u[k] + sinf(ang)*v[k]) * scale;
        if (i > 0) line(prev, pt, col);
        glm_vec3_copy(pt, prev);
    }
}

// Draw a small square plane handle (for XY / XZ / YZ panning)
static void draw_plane_square(vec3 origin, vec3 axis_a, vec3 axis_b, float scale, Color col) {
    float o = GIZMO_PLANE_OFFSET * scale;
    float s = GIZMO_PLANE_SIZE   * scale;

    vec3 corners[4];
    for (int k=0;k<3;k++) {
        corners[0][k] = origin[k] + axis_a[k]*o          + axis_b[k]*o;
        corners[1][k] = origin[k] + axis_a[k]*(o+s)      + axis_b[k]*o;
        corners[2][k] = origin[k] + axis_a[k]*(o+s)      + axis_b[k]*(o+s);
        corners[3][k] = origin[k] + axis_a[k]*o           + axis_b[k]*(o+s);
    }
    // Two triangles = quad
    Color fc = {col.r, col.g, col.b, 0.35f}; // semi-transparent fill
    triangle(corners[0], corners[1], corners[2], fc);
    triangle(corners[0], corners[2], corners[3], fc);
    // Outline
    line(corners[0], corners[1], col);
    line(corners[1], corners[2], col);
    line(corners[2], corners[3], col);
    line(corners[3], corners[0], col);
}

// Draw a small cube at tip (for Scale mode)
static void draw_cube_tip(vec3 tip, float half, Color col) {
    // 8 corners
    vec3 c[8];
    for (int i=0;i<8;i++) {
        c[i][0] = tip[0] + ((i&1)?half:-half);
        c[i][1] = tip[1] + ((i&2)?half:-half);
        c[i][2] = tip[2] + ((i&4)?half:-half);
    }
    // 12 edges
    int edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},
        {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (int e=0;e<12;e++) line(c[edges[e][0]], c[edges[e][1]], col);
}

/// Hit-test

// Check if mouse (mx,my) is close to the axis arrow projected to screen.
// Returns distance in pixels, or 1e9 if behind camera.
static float hit_axis(vec3 origin, vec3 axis_end, float mx, float my) {
    vec2 sa, sb;
    if (!world_to_screen(origin,   sa)) return 1e9f;
    if (!world_to_screen(axis_end, sb)) return 1e9f;
    vec2 mouse = {(float)mx, (float)my};
    return pt_seg_dist2d(mouse, sa, sb);
}

static float hit_plane_square(vec3 origin, vec3 aa, vec3 ab, float scale,
                               float mx, float my) {
    // Just check proximity to center of the square
    float o = GIZMO_PLANE_OFFSET * scale + GIZMO_PLANE_SIZE * scale * 0.5f;
    vec3 center;
    for (int k=0;k<3;k++)
        center[k] = origin[k] + aa[k]*o + ab[k]*o;
    vec2 sc;
    if (!world_to_screen(center, sc)) return 1e9f;
    float dx = sc[0]-(float)mx, dy = sc[1]-(float)my;
    return sqrtf(dx*dx+dy*dy);
}

static float hit_arc(vec3 origin, vec3 normal, float scale, float mx, float my) {
    // Sample the arc points and find closest to mouse
    vec3 u, v;
    vec3 ref = {0,1,0};
    if (fabsf(normal[1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
    glm_vec3_cross(normal, ref, u); glm_vec3_normalize(u);
    glm_vec3_cross(normal, u,   v); glm_vec3_normalize(v);

    vec3 to_cam;
    glm_vec3_sub(camera.position, origin, to_cam);
    float dot = glm_vec3_dot(to_cam, normal);
    vec3 in_plane;
    glm_vec3_copy(to_cam, in_plane);
    glm_vec3_muladds(normal, -dot, in_plane);
    glm_vec3_normalize(in_plane);

    float start_angle = atan2f(glm_vec3_dot(in_plane, v), glm_vec3_dot(in_plane, u));
    float half_rad    = GLM_PI * GIZMO_ARC_HALF_DEG / 180.0f;
    float angle_start = start_angle - half_rad;
    float angle_end   = start_angle + half_rad;

    float min_dist = 1e9f;
    vec2 prev_s;
    bool prev_valid = false;
    for (int i = 0; i <= GIZMO_ARC_SEGMENTS; i++) {
        float t   = (float)i / GIZMO_ARC_SEGMENTS;
        float ang = lerpf(angle_start, angle_end, t);
        vec3 pt;
        for (int k=0;k<3;k++)
            pt[k] = origin[k] + (cosf(ang)*u[k] + sinf(ang)*v[k]) * scale;
        vec2 sc;
        if (world_to_screen(pt, sc)) {
            if (prev_valid) {
                vec2 mouse = {(float)mx, (float)my};
                float d = pt_seg_dist2d(mouse, prev_s, sc);
                if (d < min_dist) min_dist = d;
            }
            glm_vec2_copy(sc, prev_s);
            prev_valid = true;
        } else {
            prev_valid = false;
        }
    }
    return min_dist;
}

//  Update hover state from mouse position
static void gizmo_update_hover(vec3 origin, float scale, double mx, double my) {
    float best_dist = 1e9f;
    GizmoPart best  = GIZMO_PART_NONE;

    vec3 ax = {1,0,0}, ay = {0,1,0}, az = {0,0,1};
    vec3 tip_x, tip_y, tip_z;
    glm_vec3_copy(origin, tip_x);
    glm_vec3_muladds(ax, scale * GIZMO_ARROW_TIP, tip_x);
    glm_vec3_copy(origin, tip_y);
    glm_vec3_muladds(ay, scale * GIZMO_ARROW_TIP, tip_y);
    glm_vec3_copy(origin, tip_z);
    glm_vec3_muladds(az, scale * GIZMO_ARROW_TIP, tip_z);

    if (gizmo.mode == GIZMO_MODE_TRANSLATE || gizmo.mode == GIZMO_MODE_SCALE) {
        float dx = hit_axis(origin, tip_x, mx, my);
        float dy = hit_axis(origin, tip_y, mx, my);
        float dz = hit_axis(origin, tip_z, mx, my);
        if (dx < GIZMO_AXIS_HIT_PX && dx < best_dist) { best_dist=dx; best=GIZMO_PART_X; }
        if (dy < GIZMO_AXIS_HIT_PX && dy < best_dist) { best_dist=dy; best=GIZMO_PART_Y; }
        if (dz < GIZMO_AXIS_HIT_PX && dz < best_dist) { best_dist=dz; best=GIZMO_PART_Z; }
    }

    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        float dxy = hit_plane_square(origin, ax, ay, scale, mx, my);
        float dxz = hit_plane_square(origin, ax, az, scale, mx, my);
        float dyz = hit_plane_square(origin, ay, az, scale, mx, my);
        if (dxy < GIZMO_PLANE_HIT_PX && dxy < best_dist) { best_dist=dxy; best=GIZMO_PART_XY; }
        if (dxz < GIZMO_PLANE_HIT_PX && dxz < best_dist) { best_dist=dxz; best=GIZMO_PART_XZ; }
        if (dyz < GIZMO_PLANE_HIT_PX && dyz < best_dist) { best_dist=dyz; best=GIZMO_PART_YZ; }
    }

    if (gizmo.mode == GIZMO_MODE_ROTATE) {
        float rx = hit_arc(origin, ax, scale, mx, my);
        float ry = hit_arc(origin, ay, scale, mx, my);
        float rz = hit_arc(origin, az, scale, mx, my);
        if (rx < GIZMO_ARC_HIT_PX && rx < best_dist) { best_dist=rx; best=GIZMO_PART_RX; }
        if (ry < GIZMO_ARC_HIT_PX && ry < best_dist) { best_dist=ry; best=GIZMO_PART_RY; }
        if (rz < GIZMO_ARC_HIT_PX && rz < best_dist) { best_dist=rz; best=GIZMO_PART_RZ; }
    }

    gizmo.hovered = best;
}

//  Apply drag delta to the selected mesh transform
static void apply_drag(int mesh_index, double mx, double my) {
    if (mesh_index < 0 || mesh_index >= (int)scene.meshes.count) return;
    Mesh* m = &scene.meshes.items[mesh_index];

    // Delta in screen pixels
    float dx_px = (float)(mx - gizmo.drag_start_x);
    float dy_px = (float)(my - gizmo.drag_start_y);

    // Scale: 1 pixel = how many world units?
    float sh    = (float)context.swapChainExtent.height;
    // Use fabsf to prevent a negative world_per_px multiplier due to Vulkan's Y-flip
    float inv_t = fabsf(camera.projection_matrix[1][1]);
    vec3 diff;
    glm_vec3_sub(gizmo.drag_start_pivot, camera.position, diff);
    float dist          = glm_vec3_norm(diff);
    if (dist < 0.01f) dist = 0.01f;
    float world_per_px  = (2.0f * dist) / (sh * inv_t);

    GizmoPart part = gizmo.dragging;

    // Restore mesh to its state at drag start, then apply accumulated delta
    glm_mat4_copy(gizmo.drag_start_model, m->model);

    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        vec3 delta = {0,0,0};

        if (part >= GIZMO_PART_X && part <= GIZMO_PART_Z) {
            vec3 axis_dir = {0,0,0};
            if (part == GIZMO_PART_X) axis_dir[0] = 1.0f;
            if (part == GIZMO_PART_Y) axis_dir[1] = 1.0f;
            if (part == GIZMO_PART_Z) axis_dir[2] = 1.0f;

            // Create a virtual plane containing the axis and facing the camera
            vec3 perp, plane_n;
            glm_vec3_cross(camera.front, axis_dir, perp);
            glm_vec3_cross(perp, axis_dir, plane_n);
            glm_vec3_normalize(plane_n);

            vec3 ro_start, rd_start, ro_curr, rd_curr;
            screen_to_ray(gizmo.drag_start_x, gizmo.drag_start_y, ro_start, rd_start);
            screen_to_ray(mx, my, ro_curr, rd_curr);

            float denom_start = glm_vec3_dot(rd_start, plane_n);
            float denom_curr  = glm_vec3_dot(rd_curr, plane_n);

            if (fabsf(denom_start) > 1e-5f && fabsf(denom_curr) > 1e-5f) {
                vec3 p2ro_s, p2ro_c;
                glm_vec3_sub(gizmo.drag_start_pivot, ro_start, p2ro_s);
                glm_vec3_sub(gizmo.drag_start_pivot, ro_curr, p2ro_c);

                float t_start = glm_vec3_dot(p2ro_s, plane_n) / denom_start;
                float t_curr  = glm_vec3_dot(p2ro_c, plane_n) / denom_curr;

                vec3 hit_start, hit_curr, hit_diff;
                glm_vec3_copy(ro_start, hit_start);
                glm_vec3_muladds(rd_start, t_start, hit_start);

                glm_vec3_copy(ro_curr, hit_curr);
                glm_vec3_muladds(rd_curr, t_curr, hit_curr);

                glm_vec3_sub(hit_curr, hit_start, hit_diff);

                // Project the 3D difference strictly onto the axis line
                float proj = glm_vec3_dot(hit_diff, axis_dir);
                glm_vec3_scale(axis_dir, proj, delta);
            }
        } else if (part >= GIZMO_PART_XY && part <= GIZMO_PART_YZ) {
            vec3 n = {0,0,0};
            if (part == GIZMO_PART_XY) n[2] = 1.0f;
            if (part == GIZMO_PART_XZ) n[1] = 1.0f;
            if (part == GIZMO_PART_YZ) n[0] = 1.0f;

            vec3 ro_start, rd_start, ro_curr, rd_curr;
            screen_to_ray(gizmo.drag_start_x, gizmo.drag_start_y, ro_start, rd_start);
            screen_to_ray(mx, my, ro_curr, rd_curr);

            float denom_start = glm_vec3_dot(rd_start, n);
            float denom_curr  = glm_vec3_dot(rd_curr, n);

            if (fabsf(denom_start) > 1e-5f && fabsf(denom_curr) > 1e-5f) {
                vec3 p2ro_s, p2ro_c;
                glm_vec3_sub(gizmo.drag_start_pivot, ro_start, p2ro_s);
                glm_vec3_sub(gizmo.drag_start_pivot, ro_curr, p2ro_c);

                float t_start = glm_vec3_dot(p2ro_s, n) / denom_start;
                float t_curr  = glm_vec3_dot(p2ro_c, n) / denom_curr;

                vec3 hit_start, hit_curr;
                glm_vec3_copy(ro_start, hit_start);
                glm_vec3_muladds(rd_start, t_start, hit_start);

                glm_vec3_copy(ro_curr, hit_curr);
                glm_vec3_muladds(rd_curr, t_curr, hit_curr);

                glm_vec3_sub(hit_curr, hit_start, delta);
            }
        }

        // Add delta directly to the world-space translation column
        // Bypasses local scale/rotation which glm_translate would multiply against
        m->model[3][0] += delta[0];
        m->model[3][1] += delta[1];
        m->model[3][2] += delta[2];
    }

    else if (gizmo.mode == GIZMO_MODE_ROTATE) {
        float angle = (dx_px + dy_px) * 0.01f; // radians per pixel
        vec3 rot_axis = {0,0,0};
        if      (part == GIZMO_PART_RX) rot_axis[0] = 1;
        else if (part == GIZMO_PART_RY) rot_axis[1] = 1;
        else if (part == GIZMO_PART_RZ) rot_axis[2] = 1;

        // Rotate around the gizmo pivot (mesh position)
        mat4 T, Tinv, R, result;
        glm_mat4_identity(T);
        glm_translate(T, gizmo.drag_start_pivot);
        glm_mat4_identity(Tinv);
        vec3 neg_pivot = {-gizmo.drag_start_pivot[0], -gizmo.drag_start_pivot[1], -gizmo.drag_start_pivot[2]};
        glm_translate(Tinv, neg_pivot);
        glm_mat4_identity(R);
        glm_rotate(R, angle, rot_axis);

        // result = T * R * Tinv * drag_start_model
        glm_mat4_mul(T, R, result);
        glm_mat4_mul(result, Tinv, result);
        glm_mat4_mul(result, gizmo.drag_start_model, m->model);
    }

    else if (gizmo.mode == GIZMO_MODE_SCALE) {
        float scale_delta = 1.0f + (dx_px - dy_px) * 0.005f;
        vec3 scale_v = {1,1,1};
        if      (part == GIZMO_PART_X) scale_v[0] = scale_delta;
        else if (part == GIZMO_PART_Y) scale_v[1] = scale_delta;
        else if (part == GIZMO_PART_Z) scale_v[2] = scale_delta;
        else    { scale_v[0] = scale_v[1] = scale_v[2] = scale_delta; } // all axes

        mat4 T, Tinv, S, result;
        glm_mat4_identity(T);
        glm_translate(T, gizmo.drag_start_pivot);
        glm_mat4_identity(Tinv);
        vec3 neg_pivot = {-gizmo.drag_start_pivot[0], -gizmo.drag_start_pivot[1], -gizmo.drag_start_pivot[2]};
        glm_translate(Tinv, neg_pivot);
        glm_mat4_identity(S);
        glm_scale(S, scale_v);

        glm_mat4_mul(T, S, result);
        glm_mat4_mul(result, Tinv, result);
        glm_mat4_mul(result, gizmo.drag_start_model, m->model);
    }

    markMeshesSSBODirty(&context);
}

/// API

static void cb_cycle_mode(void) { gizmo_cycle_mode(); }

void gizmo_init(void) {
    memset(&gizmo, 0, sizeof(GizmoState));
    gizmo.mode     = GIZMO_MODE_TRANSLATE;
    gizmo.hovered  = GIZMO_PART_NONE;
    gizmo.dragging = GIZMO_PART_NONE;
    gizmo.active   = false;

    // Bind R key to cycle gizmo mode (matches Godot's R for rotate etc.)
    keychord_bind(&keymap, "g", cb_cycle_mode, "Cycle gizmo mode (translate/rotate/scale)", PRESS);
}

void gizmo_cycle_mode(void) {
    gizmo.mode = (GizmoMode)((gizmo.mode + 1) % GIZMO_MODE_COUNT);
    const char* names[] = {"Translate", "Rotate", "Scale"};
    printf("[Gizmo] Mode: %s\n", names[gizmo.mode]);
}

extern float renderer_read_depth_at(VulkanContext* ctx, uint32_t x, uint32_t y);

int gizmo_pick_mesh(double mouse_x, double mouse_y) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    if (mouse_x < 0 || mouse_y < 0 || mouse_x >= sw || mouse_y >= sh) {
        inspector_deselect();
        gizmo.active = false;
        gizmo.dragging = GIZMO_PART_NONE;
        return -1;
    }

    // 1. Read the final rasterized Z-order from the GPU
    float depth = renderer_read_depth_at(&context, (uint32_t)mouse_x, (uint32_t)mouse_y);

    // 2. If depth is exactly 1.0f, the ray hit the background sky
    if (depth >= 1.0f) {
        inspector_deselect();
        gizmo.active   = false;
        gizmo.dragging = GIZMO_PART_NONE;
        return -1;
    }

    // 3. Unproject depth buffer pixel into an exact 3D world coordinate
    float ndcX = (2.0f * (float)mouse_x / sw) - 1.0f;
    float ndcY = (2.0f * (float)mouse_y / sh) - 1.0f;

    mat4 inv_vp, vp;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, vp);
    glm_mat4_inv(vp, inv_vp);

    vec4 clip = {ndcX, ndcY, depth, 1.0f};
    vec4 world;
    glm_mat4_mulv(inv_vp, clip, world);
    vec3 W = { world[0] / world[3], world[1] / world[3], world[2] / world[3] };

    int   best_idx  = -1;
    float best_vol  = 1e30f;

    for (int i = 0; i < (int)scene.meshes.count; i++) {
        Mesh* m = &scene.meshes.items[i];

        // Compute world-space AABB from the mesh's actual vertex bounds.
        vec3 bmin_l, bmax_l;
        glm_vec3_copy(m->aabbMin, bmin_l);
        glm_vec3_copy(m->aabbMax, bmax_l);

        vec3 world_min = { 1e30f, 1e30f, 1e30f};
        vec3 world_max = {-1e30f,-1e30f,-1e30f};
        for (int c = 0; c < 8; c++) {
            vec4 lp = {
                (c&1) ? bmax_l[0] : bmin_l[0],
                (c&2) ? bmax_l[1] : bmin_l[1],
                (c&4) ? bmax_l[2] : bmin_l[2],
                1.0f
            };
            vec4 wp;
            glm_mat4_mulv(m->model, lp, wp);
            for (int k=0;k<3;k++) {
                if (wp[k] < world_min[k]) world_min[k] = wp[k];
                if (wp[k] > world_max[k]) world_max[k] = wp[k];
            }
        }

        // 4. Check if the exact 3D surface point is inside this mesh
        float eps = 0.001f;
        if (W[0] >= world_min[0] - eps && W[0] <= world_max[0] + eps &&
            W[1] >= world_min[1] - eps && W[1] <= world_max[1] + eps &&
            W[2] >= world_min[2] - eps && W[2] <= world_max[2] + eps) {

            // Tie-breaker for overlapping AABBs: smaller volume wins (e.g. nested objects)
            float vol = (world_max[0] - world_min[0]) * (world_max[1] - world_min[1]) * (world_max[2] - world_min[2]);
            if (vol < best_vol) {
                best_vol = vol;
                best_idx = i;
            }
        }
    }

    if (best_idx >= 0) {
        inspector_select_mesh(best_idx);
        gizmo.active = true;
        printf("[Gizmo] Selected mesh %d\n", best_idx);
    } else {
        inspector_deselect();
        gizmo.active   = false;
        gizmo.dragging = GIZMO_PART_NONE;
    }
    return best_idx;
}

void gizmo_render(int mesh_index) {
    if (!gizmo.active) return;
    if (mesh_index < 0 || mesh_index >= (int)scene.meshes.count) return;

    Mesh* m = &scene.meshes.items[mesh_index];

    // Gizmo origin = mesh translation (column 3 of model matrix)
    vec3 origin = {m->model[3][0], m->model[3][1], m->model[3][2]};

    float true_scale = gizmo_world_scale(origin);

    // OPTICAL ILLUSION: Pull the gizmo completely in front of the near plane to bypass depth testing.
    // We scale it down proportionally so it retains the exact same visual footprint on screen.
    vec3 to_cam;
    glm_vec3_sub(origin, camera.position, to_cam);
    float true_dist = glm_vec3_norm(to_cam);
    glm_vec3_normalize(to_cam);

    float render_dist = 0.5f; // Must be safely > near_plane (e.g. 0.1f) to avoid clipping
    if (true_dist < render_dist) render_dist = true_dist;

    vec3 render_origin;
    glm_vec3_copy(camera.position, render_origin);
    glm_vec3_muladds(to_cam, render_dist, render_origin);

    float scale = true_scale * (render_dist / true_dist);
    glm_vec3_copy(render_origin, origin); // Override origin for the drawing functions

    vec3 ax = {1,0,0}, ay = {0,1,0}, az = {0,0,1};

    // --- Helper: choose colour (highlight if hovered/dragging) ---
    #define AXIS_COL(part, base_col, alt_col) \
        (gizmo.dragging == (part) ? (alt_col) : \
         (gizmo.hovered  == (part) ? (alt_col)  : (base_col)))

    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        Color cx  = AXIS_COL(GIZMO_PART_X,  CT.x, CT.x_alt);
        Color cy  = AXIS_COL(GIZMO_PART_Y,  CT.y, CT.y_alt);
        Color cz  = AXIS_COL(GIZMO_PART_Z,  CT.z, CT.z_alt);
        Color cxy = AXIS_COL(GIZMO_PART_XY, CT.z, CT.z_alt); // XY plane: Z-color (blue)
        Color cxz = AXIS_COL(GIZMO_PART_XZ, CT.y, CT.y_alt); // XZ plane: Y-color (green)
        Color cyz = AXIS_COL(GIZMO_PART_YZ, CT.x, CT.x_alt); // YZ plane: X-color (red)

        draw_arrow(origin, ax, scale, cx);
        draw_arrow(origin, ay, scale, cy);
        draw_arrow(origin, az, scale, cz);
        draw_plane_square(origin, ax, ay, scale, cxy);
        draw_plane_square(origin, az, ax, scale, cxz); // Swapped az and ax to flip the face normal UP (+Y)
        draw_plane_square(origin, ay, az, scale, cyz);
    }
    else if (gizmo.mode == GIZMO_MODE_ROTATE) {
        Color cx = AXIS_COL(GIZMO_PART_RX, CT.x, CT.x_alt);
        Color cy = AXIS_COL(GIZMO_PART_RY, CT.y, CT.y_alt);
        Color cz = AXIS_COL(GIZMO_PART_RZ, CT.z, CT.z_alt);
        draw_arc(origin, ax, scale, cx);
        draw_arc(origin, ay, scale, cy);
        draw_arc(origin, az, scale, cz);
    }
    else if (gizmo.mode == GIZMO_MODE_SCALE) {
        Color cx = AXIS_COL(GIZMO_PART_X, CT.x, CT.x_alt);
        Color cy = AXIS_COL(GIZMO_PART_Y, CT.y, CT.y_alt);
        Color cz = AXIS_COL(GIZMO_PART_Z, CT.z, CT.z_alt);

        float tip_t = GIZMO_ARROW_TIP * scale;
        float cube_h = scale * 0.06f;

        vec3 tip_x, tip_y, tip_z;
        glm_vec3_copy(origin, tip_x);
        glm_vec3_muladds(ax, tip_t, tip_x);
        glm_vec3_copy(origin, tip_y);
        glm_vec3_muladds(ay, tip_t, tip_y);
        glm_vec3_copy(origin, tip_z);
        glm_vec3_muladds(az, tip_t, tip_z);

        line(origin, tip_x, cx); draw_cube_tip(tip_x, cube_h, cx);
        line(origin, tip_y, cy); draw_cube_tip(tip_y, cube_h, cy);
        line(origin, tip_z, cz); draw_cube_tip(tip_z, cube_h, cz);
    }

    #undef AXIS_COL

    // Update hover from the last mouse position (stored in gizmo state)
    // We re-compute every frame since camera may have moved.
    // The actual mouse coords are fed from gizmo_mouse_move().
}

void gizmo_mouse_move(double xpos, double ypos) {
    // Convert to bottom-left Y-up
    float sh  = (float)context.swapChainExtent.height;
    double my = sh - ypos;

    if (gizmo.dragging != GIZMO_PART_NONE) {
        apply_drag(editor.inspector.selected_mesh_index, xpos, my);
        return;
    }

    if (!gizmo.active) return;
    int mi = editor.inspector.selected_mesh_index;
    if (mi < 0 || mi >= (int)scene.meshes.count) return;

    Mesh* m = &scene.meshes.items[mi];
    vec3 origin = {m->model[3][0], m->model[3][1], m->model[3][2]};
    float scale = gizmo_world_scale(origin);

    gizmo_update_hover(origin, scale, xpos, my);
}

void gizmo_mouse_button(int button, int action, int mods, double xpos, double ypos) {
    (void)mods;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    float sh  = (float)context.swapChainExtent.height;
    double my = sh - ypos;

    if (action == GLFW_PRESS) {
        // If over a gizmo part → start drag
        if (gizmo.active && gizmo.hovered != GIZMO_PART_NONE) {
            gizmo.dragging      = gizmo.hovered;
            gizmo.drag_start_x  = xpos;
            gizmo.drag_start_y  = my;

            int mi = editor.inspector.selected_mesh_index;
            if (mi >= 0 && mi < (int)scene.meshes.count) {
                Mesh* m = &scene.meshes.items[mi];
                glm_mat4_copy(m->model, gizmo.drag_start_model);
                gizmo.drag_start_pivot[0] = m->model[3][0];
                gizmo.drag_start_pivot[1] = m->model[3][1];
                gizmo.drag_start_pivot[2] = m->model[3][2];
            }
        } else {
            // Not over gizmo → pick mesh
            gizmo_pick_mesh(xpos, ypos); // ypos in screen-top coords (pick handles flip)
        }
    } else if (action == GLFW_RELEASE) {
        gizmo.dragging = GIZMO_PART_NONE;
    }
}
