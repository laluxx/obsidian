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
#define GIZMO_PART_RS         128      // Screen-space rotation ring
#define GIZMO_LINE_THICK      4.0f     // Inner 3D rotation arcs
#define GIZMO_LINE_THIN       2.0f     // Outer white 2D ring and defaults
#define GIZMO_PIE_ALPHA       0.5f     // Transparency of the active rotation slice

GizmoState gizmo = {0};

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

static vec3   s_rot_v_start;
static float  s_rot_angle;
static double s_last_angle_2d;
static float  s_rot_sign;
static double s_curr_mx;
static double s_curr_my;
static vec3   s_rot_v_prev;
static mat4   s_drag_start_local;
static mat4   s_drag_start_bone_override;

extern bool editor_show_bones;
extern void* editor_selected_bone;
void* editor_hovered_bone = NULL;

static mat4* get_bone_world_matrix(int mesh_index, OmdlSceneGraph** out_osg, int32_t* out_bnode_idx) {
    if (!editor_show_bones || !editor_selected_bone) return NULL;
    for (size_t i = 0; i < scene.gltf_instance_count; i++) {
        if (mesh_index >= (int)scene.gltf_instances[i].mesh_start_index &&
            mesh_index < (int)(scene.gltf_instances[i].mesh_start_index + scene.gltf_instances[i].mesh_count)) {
            GLTFInstance* inst = &scene.gltf_instances[i];
            if (inst->gltf_data) {
                OmdlSceneGraph* osg = (OmdlSceneGraph*)inst->gltf_data;
                OmdlNode* bnode = (OmdlNode*)editor_selected_bone;
                int32_t bnode_idx = (int32_t)(bnode - osg->nodes);
                if (bnode_idx >= 0 && bnode_idx < (int32_t)osg->node_count) {
                    if (out_osg) *out_osg = osg;
                    if (out_bnode_idx) *out_bnode_idx = bnode_idx;
                    return &osg->world_transforms[bnode_idx];
                }
            }
        }
    }
    return NULL;
}

static void draw_full_ring(vec3 origin, vec3 normal, float scale, Color col) {
    vec3 u, v;
    vec3 ref = {0,1,0};
    if (fabsf(normal[1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
    glm_vec3_cross(normal, ref, u); glm_vec3_normalize(u);
    glm_vec3_cross(normal, u, v);   glm_vec3_normalize(v);

    vec3 prev;
    for (int i = 0; i <= 64; i++) {
        float ang = (float)i / 64.0f * GLM_PI * 2.0f;
        vec3 pt;
        for (int k=0;k<3;k++) pt[k] = origin[k] + (cosf(ang)*u[k] + sinf(ang)*v[k]) * scale;
        if (i > 0) line(prev, pt, col);
        glm_vec3_copy(pt, prev);
    }
}

static void draw_pie(vec3 origin, vec3 normal, vec3 v_start, float angle, float scale, Color col) {
    if (fabsf(angle) < 0.01f) return;

    vec3 u; glm_vec3_copy(v_start, u);
    vec3 v; glm_vec3_cross(normal, u, v); glm_vec3_normalize(v);

    int segments = (int)(fabsf(angle) / (GLM_PI * 2.0f) * 64.0f) + 1;
    if (segments > 64) segments = 64;

    vec3 prev_pt;
    glm_vec3_copy(origin, prev_pt);
    glm_vec3_muladds(u, scale, prev_pt);

    for (int i = 1; i <= segments; i++) {
        float t = (float)i / segments;
        float ang = angle * t;
        vec3 pt;
        for (int k=0;k<3;k++) pt[k] = origin[k] + (cosf(ang)*u[k] + sinf(ang)*v[k]) * scale;

        // Double-sided rendering so it never disappears based on viewing angle
        triangle(origin, prev_pt, pt, col);
        triangle(origin, pt, prev_pt, col);
        glm_vec3_copy(pt, prev_pt);
    }
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

    // Rotate start so the arc faces away from the camera (convex dome look)
    vec3 to_cam;
    glm_vec3_sub(origin, camera.position, to_cam);
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
    // 12 triangles (6 faces) with CCW winding for outward normals
    int indices[36] = {
        0, 1, 5, 0, 5, 4, // Bottom (y-)
        2, 6, 7, 2, 7, 3, // Top (y+)
        0, 4, 6, 0, 6, 2, // Left (x-)
        1, 3, 7, 1, 7, 5, // Right (x+)
        0, 2, 3, 0, 3, 1, // Front (z-)
        4, 5, 7, 4, 7, 6  // Back (z+)
    };
    for (int i=0; i<36; i+=3) {
        triangle(c[indices[i]], c[indices[i+1]], c[indices[i+2]], col);
    }
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

static float hit_full_ring(vec3 origin, vec3 normal, float scale, float mx, float my) {
    vec3 u, v;
    vec3 ref = {0,1,0};
    if (fabsf(normal[1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
    glm_vec3_cross(normal, ref, u); glm_vec3_normalize(u);
    glm_vec3_cross(normal, u,   v); glm_vec3_normalize(v);

    float min_dist = 1e9f;
    vec2 prev_s;
    bool prev_valid = false;
    for (int i = 0; i <= 64; i++) {
        float ang = (float)i / 64.0f * GLM_PI * 2.0f;
        vec3 pt;
        for (int k=0;k<3;k++) pt[k] = origin[k] + (cosf(ang)*u[k] + sinf(ang)*v[k]) * scale;
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

static float hit_arc(vec3 origin, vec3 normal, float scale, float mx, float my) {
    // Sample the arc points and find closest to mouse
    vec3 u, v;
    vec3 ref = {0,1,0};
    if (fabsf(normal[1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
    glm_vec3_cross(normal, ref, u); glm_vec3_normalize(u);
    glm_vec3_cross(normal, u,   v); glm_vec3_normalize(v);

    vec3 to_cam;
    glm_vec3_sub(origin, camera.position, to_cam);
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

    if (gizmo.mode == GIZMO_MODE_TRANSLATE || gizmo.mode == GIZMO_MODE_SCALE) {
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

        vec3 to_cam;
        glm_vec3_sub(camera.position, origin, to_cam);
        glm_vec3_normalize(to_cam);
        float rs = hit_full_ring(origin, to_cam, scale * 1.2f, mx, my);

        if (rx < GIZMO_ARC_HIT_PX && rx < best_dist) { best_dist=rx; best=GIZMO_PART_RX; }
        if (ry < GIZMO_ARC_HIT_PX && ry < best_dist) { best_dist=ry; best=GIZMO_PART_RY; }
        if (rz < GIZMO_ARC_HIT_PX && rz < best_dist) { best_dist=rz; best=GIZMO_PART_RZ; }
        if (rs < GIZMO_ARC_HIT_PX && rs < best_dist) { best_dist=rs; best=GIZMO_PART_RS; }
    }

    gizmo.hovered = best;
}

// Apply drag delta to the selected mesh transform
static void apply_drag(int mesh_index, double mx, double my) {
    if (mesh_index < 0 || mesh_index >= (int)scene.meshes.count) return;
    Mesh* m = &scene.meshes.items[mesh_index];

    OmdlSceneGraph* osg = NULL;
    int32_t bnode_idx = -1;
    mat4* bone_mat = get_bone_world_matrix(mesh_index, &osg, &bnode_idx);

    mat4 new_world;
    glm_mat4_copy(gizmo.drag_start_model, new_world);
    mat4 new_local;
    glm_mat4_copy(s_drag_start_local, new_local);

    float dx_px = (float)(mx - gizmo.drag_start_x);
    float dy_px = (float)(my - gizmo.drag_start_y);
    GizmoPart part = gizmo.dragging;

    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        vec3 delta = {0,0,0};

        if (part >= GIZMO_PART_X && part <= GIZMO_PART_Z) {
            vec3 axis_dir = {0,0,0};
            if (part == GIZMO_PART_X) axis_dir[0] = 1.0f;
            if (part == GIZMO_PART_Y) axis_dir[1] = 1.0f;
            if (part == GIZMO_PART_Z) axis_dir[2] = 1.0f;

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

        new_world[3][0] += delta[0];
        new_world[3][1] += delta[1];
        new_world[3][2] += delta[2];

        mat4 T; glm_mat4_identity(T);
        glm_translate(T, delta);
        glm_mat4_mul(T, s_drag_start_local, new_local);
    }
    else if (gizmo.mode == GIZMO_MODE_ROTATE) {
        vec3 n = {0,0,0};
        if      (part == GIZMO_PART_RX) n[0] = 1.0f;
        else if (part == GIZMO_PART_RY) n[1] = 1.0f;
        else if (part == GIZMO_PART_RZ) n[2] = 1.0f;
        else if (part == GIZMO_PART_RS) {
            glm_vec3_sub(camera.position, gizmo.drag_start_pivot, n);
            glm_vec3_normalize(n);
        }

        vec2 sc;
        if (world_to_screen(gizmo.drag_start_pivot, sc)) {
            double curr_angle_2d = atan2(my - sc[1], mx - sc[0]);
            double d_ang = curr_angle_2d - s_last_angle_2d;

            while (d_ang >  GLM_PI) d_ang -= 2.0 * GLM_PI;
            while (d_ang < -GLM_PI) d_ang += 2.0 * GLM_PI;

            if (gizmo.dragging == GIZMO_PART_RS) {
                s_rot_angle -= (float)(d_ang * s_rot_sign);
            } else {
                s_rot_angle += (float)(d_ang * s_rot_sign);
            }
            s_last_angle_2d = curr_angle_2d;

            if (s_rot_angle >  GLM_PI * 2.0f) s_rot_angle -= GLM_PI * 2.0f;
            if (s_rot_angle < -GLM_PI * 2.0f) s_rot_angle += GLM_PI * 2.0f;
        }

        mat4 T, Tinv, R, result;
        glm_mat4_identity(T);
        glm_translate(T, gizmo.drag_start_pivot);
        glm_mat4_identity(Tinv);
        vec3 neg_pivot = {-gizmo.drag_start_pivot[0], -gizmo.drag_start_pivot[1], -gizmo.drag_start_pivot[2]};
        glm_translate(Tinv, neg_pivot);
        glm_mat4_identity(R);

        float mesh_angle = (part == GIZMO_PART_RS) ? -s_rot_angle : s_rot_angle;
        glm_rotate(R, mesh_angle, n);

        glm_mat4_mul(T, R, result);
        glm_mat4_mul(result, Tinv, result);
        glm_mat4_mul(result, gizmo.drag_start_model, new_world);
        glm_mat4_mul(result, s_drag_start_local, new_local);
    }
    else if (gizmo.mode == GIZMO_MODE_SCALE) {
        vec3 delta = {0,0,0};

        if (part >= GIZMO_PART_X && part <= GIZMO_PART_Z) {
            vec3 axis_dir = {0,0,0};
            if (part == GIZMO_PART_X) axis_dir[0] = 1.0f;
            if (part == GIZMO_PART_Y) axis_dir[1] = 1.0f;
            if (part == GIZMO_PART_Z) axis_dir[2] = 1.0f;

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

        float gizmo_scale = gizmo_world_scale(gizmo.drag_start_pivot);
        vec3 scale_v = {
            1.0f + (delta[0] / gizmo_scale),
            1.0f - (delta[1] / gizmo_scale),
            1.0f + (delta[2] / gizmo_scale)
        };

        if (part == GIZMO_PART_X) { scale_v[1] = 1.0f; scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_Y) { scale_v[0] = 1.0f; scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_Z) { scale_v[0] = 1.0f; scale_v[1] = 1.0f; }
        else if (part == GIZMO_PART_XY) { scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_XZ) { scale_v[1] = 1.0f; }
        else if (part == GIZMO_PART_YZ) { scale_v[0] = 1.0f; }

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
        glm_mat4_mul(result, gizmo.drag_start_model, new_world);
        glm_mat4_mul(result, s_drag_start_local, new_local);
    }

    if (bone_mat) {
        int local_j = -1;
        uint32_t mesh_node_idx = (uint32_t)(uintptr_t)m->node;
        int32_t skin_idx = osg->nodes[mesh_node_idx].skin_idx;
        if (skin_idx >= 0) {
            OmdlSkin* skin = &osg->skins[skin_idx];
            for (uint32_t j = 0; j < skin->joints_count; j++) {
                if (osg->skin_joints[skin->joints_offset + j] == (uint32_t)bnode_idx) {
                    local_j = j; break;
                }
            }
        }

        if (local_j >= 0) {
            mat4 T;
            mat4 inv_start;
            glm_mat4_inv(gizmo.drag_start_model, inv_start);
            glm_mat4_mul(new_world, inv_start, T);

            if (!m->bone_overrides[local_j].active) {
                glm_mat4_identity(m->bone_overrides[local_j].world_offset);
                m->bone_overrides[local_j].active = true;
            }
            // Apply the absolute delta T to the INITIAL bone override for perfect 1:1 mouse tracking!
            glm_mat4_mul(T, s_drag_start_bone_override, m->bone_overrides[local_j].world_offset);

            // Visual update for zero-latency feedback without recursive accumulation
            glm_mat4_copy(new_world, osg->world_transforms[bnode_idx]);
        }
    } else {
        // If moving a mesh that is part of a glTF instance, apply the transform to the ROOT mesh of that instance so the whole character moves!
        int target_idx = mesh_index;
        bool is_gltf = false;
        for (size_t i = 0; i < scene.gltf_instance_count; i++) {
            if (mesh_index >= (int)scene.gltf_instances[i].mesh_start_index &&
                mesh_index < (int)(scene.gltf_instances[i].mesh_start_index + scene.gltf_instances[i].mesh_count)) {
                target_idx = (int)scene.gltf_instances[i].mesh_start_index;
                is_gltf = true;
                break;
            }
        }

        if (is_gltf) {
            Mesh* root_m = &scene.meshes.items[target_idx];

            vec3 delta_pos;
            delta_pos[0] = new_world[3][0] - m->model[3][0];
            delta_pos[1] = new_world[3][1] - m->model[3][1];
            delta_pos[2] = new_world[3][2] - m->model[3][2];

            root_m->local_transform[3][0] += delta_pos[0];
            root_m->local_transform[3][1] += delta_pos[1];
            root_m->local_transform[3][2] += delta_pos[2];

            glm_mat4_copy(new_world, m->model);
            glm_mat4_copy(new_local, m->local_transform);
        } else {
            glm_mat4_copy(new_world, m->model);
            glm_mat4_copy(new_local, m->local_transform);
        }
    }

    markMeshesSSBODirty(&context);
}

/// API

static void cb_mode_translate(void) { gizmo.mode = GIZMO_MODE_TRANSLATE; }
static void cb_mode_rotate(void)    { gizmo.mode = GIZMO_MODE_ROTATE;    }
static void cb_mode_scale(void)     { gizmo.mode = GIZMO_MODE_SCALE;     }
static void cb_mode_select(void)    { inspector_deselect();              }

static void cb_mode_translate_scale_toggle(void) {
    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        gizmo.mode = GIZMO_MODE_SCALE;
    } else {
        gizmo.mode = GIZMO_MODE_TRANSLATE;
    }
}

void gizmo_init(void) {
    memset(&gizmo, 0, sizeof(GizmoState));
    gizmo.mode     = GIZMO_MODE_TRANSLATE;
    gizmo.hovered  = GIZMO_PART_NONE;
    gizmo.dragging = GIZMO_PART_NONE;
    gizmo.active   = false;

    keychord_bind(&keymap, "q", cb_mode_rotate,                 "Gizmo: Rotate",          PRESS);
    keychord_bind(&keymap, "w", cb_mode_translate_scale_toggle, "Gizmo: Translate/Scale", PRESS);
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

        // Dynamically expand AABB to encapsulate live bone positions for animated meshes
        if (m->jointCount > 0 && m->node) {
            for (size_t g = 0; g < scene.gltf_instance_count; g++) {
                if (i >= (int)scene.gltf_instances[g].mesh_start_index &&
                    i < (int)(scene.gltf_instances[g].mesh_start_index + scene.gltf_instances[g].mesh_count)) {
                    GLTFInstance* inst = &scene.gltf_instances[g];
                    if (inst->gltf_data) {
                        OmdlSceneGraph* osg = (OmdlSceneGraph*)inst->gltf_data;
                        uint32_t mesh_node_idx = (uint32_t)(uintptr_t)m->node;
                        int32_t skin_idx = osg->nodes[mesh_node_idx].skin_idx;
                        if (skin_idx >= 0) {
                            OmdlSkin* skin = &osg->skins[skin_idx];
                            for (uint32_t j = 0; j < skin->joints_count; j++) {
                                uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                                vec3 j_pos;
                                glm_vec3_copy(osg->world_transforms[j_node][3], j_pos);
                                // Pad the bone by 1.0 unit (1 meter) to encompass the skin radius
                                float pad = 1.0f;
                                for (int k = 0; k < 3; k++) {
                                    if (j_pos[k] - pad < world_min[k]) world_min[k] = j_pos[k] - pad;
                                    if (j_pos[k] + pad > world_max[k]) world_max[k] = j_pos[k] + pad;
                                }
                            }
                        }
                    }
                    break;
                }
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

    if (editor_show_bones && m->jointCount > 0) {
        GLTFInstance* inst = NULL;
        for (size_t i = 0; i < scene.gltf_instance_count; i++) {
            if (mesh_index >= (int)scene.gltf_instances[i].mesh_start_index &&
                mesh_index < (int)(scene.gltf_instances[i].mesh_start_index + scene.gltf_instances[i].mesh_count)) {
                inst = &scene.gltf_instances[i];
                break;
            }
        }

        if (inst && inst->gltf_data && m->node) {
            OmdlSceneGraph* osg = (OmdlSceneGraph*)inst->gltf_data;
            uint32_t node_idx = (uint32_t)(uintptr_t)m->node;
            int32_t skin_idx = osg->nodes[node_idx].skin_idx;

            if (skin_idx >= 0) {
                OmdlSkin* skin = &osg->skins[skin_idx];

                Material boneMat;
                memset(&boneMat, 0, sizeof(Material));
                boneMat.baseColorFactor[0] = 1.0f; boneMat.baseColorFactor[1] = 1.0f; boneMat.baseColorFactor[2] = 1.0f; boneMat.baseColorFactor[3] = 1.0f;
                boneMat.isUnlit = 1;
                boneMat.isWireframe = 1;
                boneMat.alphaMode = 0;
                boneMat.albedoIndex = -1;
                boneMat.normalMapIndex = -1;
                boneMat.metallicRoughIndex = -1;
                boneMat.aoIndex = -1;
                boneMat.emissiveIndex = -1;
                boneMat.transmissionIndex = -1;
                boneMat.thicknessIndex = -1;
                set_material(&boneMat);

                bool is_joint[4096] = {false};
                for (uint32_t j = 0; j < skin->joints_count; j++) {
                    uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                    if (j_node < 4096) is_joint[j_node] = true;
                }

                for (uint32_t j = 0; j < skin->joints_count; j++) {
                    uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                    OmdlNode* node = &osg->nodes[j_node];

                    Color c = (node == editor_selected_bone) ? CT.bone_selected :
                              (node == editor_hovered_bone) ? CT.bone_hover : CT.bone;
                    vec3 start_pos;
                    glm_vec3_copy(osg->world_transforms[j_node][3], start_pos);

                    bool has_child_joint = false;
                    for (uint32_t c_idx = 0; c_idx < osg->node_count; c_idx++) {
                        if (osg->nodes[c_idx].parent == (int32_t)j_node && is_joint[c_idx]) {
                            vec3 end_pos;
                            glm_vec3_copy(osg->world_transforms[c_idx][3], end_pos);
                            bone(start_pos, end_pos, c);
                            has_child_joint = true;
                        }
                    }

                    // If no child joint, draw a small nub
                    if (!has_child_joint) {
                        vec3 end_pos;
                        int32_t p_idx = osg->nodes[j_node].parent;
                        if (p_idx >= 0 && p_idx < 4096 && is_joint[p_idx]) {
                            vec3 p_pos;
                            glm_vec3_copy(osg->world_transforms[p_idx][3], p_pos);
                            vec3 dir;
                            glm_vec3_sub(start_pos, p_pos, dir);
                            if (glm_vec3_norm2(dir) > 0.00001f) {
                                glm_vec3_scale(dir, 0.5f, dir);
                                glm_vec3_add(start_pos, dir, end_pos);
                            } else {
                                goto fallback_nub;
                            }
                        } else {
                        fallback_nub:;
                            vec3 local_up = {0.0f, 0.1f, 0.0f}; // 10cm default length
                            mat3 rot;
                            glm_mat4_pick3(osg->world_transforms[j_node], rot);
                            glm_mat3_mulv(rot, local_up, local_up);
                            glm_vec3_add(start_pos, local_up, end_pos);
                        }
                        bone(start_pos, end_pos, c);
                    }
                }

                reset_material();
            }
        }
    }

    OmdlSceneGraph* osg = NULL;
    int32_t bnode_idx = -1;
    mat4* bone_mat = get_bone_world_matrix(mesh_index, &osg, &bnode_idx);

    vec3 origin;
    if (bone_mat) {
        origin[0] = (*bone_mat)[3][0];
        origin[1] = (*bone_mat)[3][1];
        origin[2] = (*bone_mat)[3][2];
    } else {
        origin[0] = m->model[3][0];
        origin[1] = m->model[3][1];
        origin[2] = m->model[3][2];
    }

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

    vec3 ax = {1,0,0}, ay = {0,1,0}, az = {0,0,1};

    // --- Draw Infinite Guide Lines while dragging ---
    if (gizmo.dragging != GIZMO_PART_NONE) {
        float inf = 10000.0f;
        vec3 px1, px2, py1, py2, pz1, pz2;
        for(int k=0; k<3; k++) {
            px1[k] = origin[k] - ax[k] * inf; px2[k] = origin[k] + ax[k] * inf;
            py1[k] = origin[k] - ay[k] * inf; py2[k] = origin[k] + ay[k] * inf;
            pz1[k] = origin[k] - az[k] * inf; pz2[k] = origin[k] + az[k] * inf;
        }

        bool draw_x = (gizmo.dragging == GIZMO_PART_X || gizmo.dragging == GIZMO_PART_RX || gizmo.dragging == GIZMO_PART_XY || gizmo.dragging == GIZMO_PART_XZ);
        bool draw_y = (gizmo.dragging == GIZMO_PART_Y || gizmo.dragging == GIZMO_PART_RY || gizmo.dragging == GIZMO_PART_XY || gizmo.dragging == GIZMO_PART_YZ);
        bool draw_z = (gizmo.dragging == GIZMO_PART_Z || gizmo.dragging == GIZMO_PART_RZ || gizmo.dragging == GIZMO_PART_XZ || gizmo.dragging == GIZMO_PART_YZ);

        line_set_width(GIZMO_LINE_THIN);

        if (draw_x) line(px1, px2, CT.x_bright);
        if (draw_y) line(py1, py2, CT.y_bright);
        if (draw_z) line(pz1, pz2, CT.z_bright);

        if (gizmo.dragging == GIZMO_PART_RS) {
            vec3 prs1, prs2;
            for(int k=0; k<3; k++) {
                prs1[k] = origin[k] - to_cam[k] * inf;
                prs2[k] = origin[k] + to_cam[k] * inf;
            }
            line(prs1, prs2, CT.gizmo_outer_circle_bright);
        }
    }

    float scale = true_scale * (render_dist / true_dist);
    glm_vec3_copy(render_origin, origin); // Override origin for the drawing functions

    // --- Helper: choose colour (highlight if hovered/dragging) ---
    #define AXIS_COL(part, base_col, alt_col) \
        (gizmo.dragging == (part) ? (alt_col) : \
         (gizmo.hovered  == (part) ? (alt_col)  : (base_col)))

    if (gizmo.mode == GIZMO_MODE_TRANSLATE) {
        Color cx  = AXIS_COL(GIZMO_PART_X,  CT.x, CT.x_bright);
        Color cy  = AXIS_COL(GIZMO_PART_Y,  CT.y, CT.y_bright);
        Color cz  = AXIS_COL(GIZMO_PART_Z,  CT.z, CT.z_bright);
        Color cxy = AXIS_COL(GIZMO_PART_XY, CT.z, CT.z_bright); // XY plane: Z-color (blue)
        Color cxz = AXIS_COL(GIZMO_PART_XZ, CT.y, CT.y_bright); // XZ plane: Y-color (green)
        Color cyz = AXIS_COL(GIZMO_PART_YZ, CT.x, CT.x_bright); // YZ plane: X-color (red)

        draw_arrow(origin, ax, scale, cx);
        draw_arrow(origin, ay, scale, cy);
        draw_arrow(origin, az, scale, cz);
        draw_plane_square(origin, ax, ay, scale, cxy);
        draw_plane_square(origin, az, ax, scale, cxz); // Swapped az and ax to flip the face normal UP (+Y)
        draw_plane_square(origin, ay, az, scale, cyz);
    }
    else if (gizmo.mode == GIZMO_MODE_ROTATE) {
        if ((gizmo.dragging >= GIZMO_PART_RX && gizmo.dragging <= GIZMO_PART_RZ) || gizmo.dragging == GIZMO_PART_RS) {
            vec3 n = {0,0,0};
            Color alt_c = CT.x_bright;
            Color base_c = CT.x;

            if (gizmo.dragging == GIZMO_PART_RX) { n[0] = 1.0f; alt_c = CT.x_bright; base_c = CT.x; }
            else if (gizmo.dragging == GIZMO_PART_RY) { n[1] = 1.0f; alt_c = CT.y_bright; base_c = CT.y; }
            else if (gizmo.dragging == GIZMO_PART_RZ) { n[2] = 1.0f; alt_c = CT.z_bright; base_c = CT.z; }
            else if (gizmo.dragging == GIZMO_PART_RS) { glm_vec3_copy(to_cam, n); alt_c = CT.gizmo_outer_circle_bright; base_c = CT.gizmo_outer_circle_bright; }

            float active_scale = (gizmo.dragging == GIZMO_PART_RS) ? scale * 1.2f : scale;
            float active_width = (gizmo.dragging == GIZMO_PART_RS) ? GIZMO_LINE_THIN : GIZMO_LINE_THICK;

            line_set_width(active_width);

            // 1. Draw full closed background ring
            draw_full_ring(origin, n, active_scale, base_c);

            // 2. Fill rotated slice (starts exactly at the drag origin, sweeps to current angle)
            Color pie_c = {base_c.r, base_c.g, base_c.b, GIZMO_PIE_ALPHA};
            draw_pie(origin, n, s_rot_v_start, s_rot_angle, active_scale, pie_c);

            // 3. Draw lines at the two edges of the pie slice (extended slightly)
            Color line_c = (gizmo.dragging == GIZMO_PART_RS) ? CT.gizmo_outer_circle_bright : base_c;

            vec3 pt_start, pt_curr;
            mat4 rot; glm_mat4_identity(rot);
            glm_rotate(rot, s_rot_angle, n);
            vec4 v_start4 = {s_rot_v_start[0], s_rot_v_start[1], s_rot_v_start[2], 0.0f};
            vec4 v_curr4;
            glm_mat4_mulv(rot, v_start4, v_curr4);

            for(int k=0;k<3;k++) pt_start[k] = origin[k] + s_rot_v_start[k] * active_scale;
            for(int k=0;k<3;k++) pt_curr[k]  = origin[k] + v_curr4[k] * active_scale;

            line(origin, pt_start, line_c);
            if (gizmo.dragging != GIZMO_PART_RS) {
                line(origin, pt_curr, line_c);
            }

            // 4. Draw a 2D screen-space line from the center perfectly to the cursor
            line_set_width(GIZMO_LINE_THIN); // Force the tracking line to always be thin

            // Unproject cursor to the camera-facing plane passing through the gizmo origin
            vec3 ro, rd;
            screen_to_ray(s_curr_mx, s_curr_my, ro, rd);
            float denom = glm_vec3_dot(rd, to_cam);
            if (fabsf(denom) > 1e-4f) {
                vec3 p2ro;
                glm_vec3_sub(origin, ro, p2ro);
                float t = glm_vec3_dot(p2ro, to_cam) / denom;
                vec3 hit;
                glm_vec3_copy(ro, hit);
                glm_vec3_muladds(rd, t, hit);
                line(origin, hit, (gizmo.dragging == GIZMO_PART_RS) ? CT.gizmo_outer_circle_bright : alt_c);
            }

        } else {
            // Draw normal arcs when not actively dragging
            Color cx = AXIS_COL(GIZMO_PART_RX, CT.x, CT.x_bright);
            Color cy = AXIS_COL(GIZMO_PART_RY, CT.y, CT.y_bright);
            Color cz = AXIS_COL(GIZMO_PART_RZ, CT.z, CT.z_bright);
            Color crs = AXIS_COL(GIZMO_PART_RS, CT.gizmo_outer_circle, CT.gizmo_outer_circle_bright);

            line_set_width(GIZMO_LINE_THICK);
            draw_arc(origin, ax, scale, cx);
            draw_arc(origin, ay, scale, cy);
            draw_arc(origin, az, scale, cz);

            line_set_width(GIZMO_LINE_THIN);
            draw_full_ring(origin, to_cam, scale * 1.2f, crs);
        }
    }
    else if (gizmo.mode == GIZMO_MODE_SCALE) {
        Color cx = AXIS_COL(GIZMO_PART_X, CT.x, CT.x_bright);
        Color cy = AXIS_COL(GIZMO_PART_Y, CT.y, CT.y_bright);
        Color cz = AXIS_COL(GIZMO_PART_Z, CT.z, CT.z_bright);
        Color cxy = AXIS_COL(GIZMO_PART_XY, CT.z, CT.z_bright);
        Color cxz = AXIS_COL(GIZMO_PART_XZ, CT.y, CT.y_bright);
        Color cyz = AXIS_COL(GIZMO_PART_YZ, CT.x, CT.x_bright);

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

        draw_plane_square(origin, ax, ay, scale, cxy);
        draw_plane_square(origin, az, ax, scale, cxz);
        draw_plane_square(origin, ay, az, scale, cyz);
    }

    #undef AXIS_COL
}

extern void editor_mouse_move(double xpos, double ypos);

void gizmo_mouse_move(double xpos, double ypos) {
    editor_mouse_move(xpos, ypos);
    // Convert to bottom-left Y-up
    float sh  = (float)context.swapChainExtent.height;
    double my = sh - ypos;

    s_curr_mx = xpos;
    s_curr_my = my;

    if (gizmo.dragging != GIZMO_PART_NONE) {
        apply_drag(editor.inspector.selected_mesh_index, xpos, my);
        return;
    }

    if (!gizmo.active) return;
    int mi = editor.inspector.selected_mesh_index;
    if (mi < 0 || mi >= (int)scene.meshes.count) return;

    Mesh* m = &scene.meshes.items[mi];
    OmdlSceneGraph* osg = NULL;
    int32_t bnode_idx = -1;
    mat4* bone_mat = get_bone_world_matrix(mi, &osg, &bnode_idx);

    vec3 origin;
    if (bone_mat) {
        origin[0] = (*bone_mat)[3][0];
        origin[1] = (*bone_mat)[3][1];
        origin[2] = (*bone_mat)[3][2];
    } else {
        origin[0] = m->model[3][0];
        origin[1] = m->model[3][1];
        origin[2] = m->model[3][2];
    }
    float scale = gizmo_world_scale(origin);

    gizmo_update_hover(origin, scale, xpos, my);

    editor_hovered_bone = NULL;
    if (editor_show_bones && m->jointCount > 0 && gizmo.hovered == GIZMO_PART_NONE) {
        GLTFInstance* inst = NULL;
        for (size_t i = 0; i < scene.gltf_instance_count; i++) {
            if (mi >= (int)scene.gltf_instances[i].mesh_start_index &&
                mi < (int)(scene.gltf_instances[i].mesh_start_index + scene.gltf_instances[i].mesh_count)) {
                inst = &scene.gltf_instances[i];
                break;
            }
        }
        if (inst && inst->gltf_data && m->node) {
            OmdlSceneGraph* osg = (OmdlSceneGraph*)inst->gltf_data;
            uint32_t mesh_node_idx = (uint32_t)(uintptr_t)m->node;
            int32_t skin_idx = osg->nodes[mesh_node_idx].skin_idx;
            if (skin_idx >= 0) {
                OmdlSkin* skin = &osg->skins[skin_idx];
                bool is_joint[4096] = {false};
                for (uint32_t j = 0; j < skin->joints_count; j++) {
                    uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                    if (j_node < 4096) is_joint[j_node] = true;
                }

                float best_dist = 1e9f;
                void* best_node = NULL;

                for (uint32_t j = 0; j < skin->joints_count; j++) {
                    uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                    OmdlNode* node = &osg->nodes[j_node];
                    vec3 start_pos;
                    glm_vec3_copy(osg->world_transforms[j_node][3], start_pos);

                    bool has_child_joint = false;
                    for (uint32_t c_idx = 0; c_idx < osg->node_count; c_idx++) {
                        if (osg->nodes[c_idx].parent == (int32_t)j_node && is_joint[c_idx]) {
                            vec3 end_pos;
                            glm_vec3_copy(osg->world_transforms[c_idx][3], end_pos);
                            vec2 s1, s2;
                            if (world_to_screen(start_pos, s1) && world_to_screen(end_pos, s2)) {
                                float d = pt_seg_dist2d((vec2){(float)xpos, (float)my}, s1, s2);
                                float bone_len = sqrtf((s2[0]-s1[0])*(s2[0]-s1[0]) + (s2[1]-s1[1])*(s2[1]-s1[1]));
                                float dynamic_radius = fmaxf(18.0f, bone_len * 0.15f);
                                if (d < dynamic_radius && d < best_dist) {
                                    best_dist = d;
                                    best_node = node;
                                }
                            }
                            has_child_joint = true;
                        }
                    }

                    if (!has_child_joint) {
                        vec3 end_pos;
                        int32_t p_idx = osg->nodes[j_node].parent;
                        if (p_idx >= 0 && p_idx < 4096 && is_joint[p_idx]) {
                            vec3 p_pos;
                            glm_vec3_copy(osg->world_transforms[p_idx][3], p_pos);
                            vec3 dir;
                            glm_vec3_sub(start_pos, p_pos, dir);
                            if (glm_vec3_norm2(dir) > 0.00001f) {
                                glm_vec3_scale(dir, 0.5f, dir);
                                glm_vec3_add(start_pos, dir, end_pos);
                            } else {
                                goto fallback_nub_pick;
                            }
                        } else {
                        fallback_nub_pick:;
                            vec3 local_up = {0.0f, 0.1f, 0.0f};
                            mat3 rot;
                            glm_mat4_pick3(osg->world_transforms[j_node], rot);
                            glm_mat3_mulv(rot, local_up, local_up);
                            glm_vec3_add(start_pos, local_up, end_pos);
                        }
                        vec2 s1, s2;
                        if (world_to_screen(start_pos, s1) && world_to_screen(end_pos, s2)) {
                            float d = pt_seg_dist2d((vec2){(float)xpos, (float)my}, s1, s2);
                            float bone_len = sqrtf((s2[0]-s1[0])*(s2[0]-s1[0]) + (s2[1]-s1[1])*(s2[1]-s1[1]));
                            float dynamic_radius = fmaxf(18.0f, bone_len * 0.15f);
                            if (d < dynamic_radius && d < best_dist) {
                                best_dist = d;
                                best_node = node;
                            }
                        }
                    }
                }
                editor_hovered_bone = best_node;
            }
        }
    }
}

extern void editor_mouse_button(int button, int action);
extern bool editor_wants_mouse(void);

void gizmo_mouse_button(int button, int action, int mods, double xpos, double ypos) {
    (void)mods;
    editor_mouse_button(button, action);

    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    float sh  = (float)context.swapChainExtent.height;
    double my = sh - ypos;

    s_curr_mx = xpos;
    s_curr_my = my;

    if (action == GLFW_PRESS) {
        if (editor_wants_mouse()) return; // Block scene picking/gizmo click when interacting with UI panels

        // If over a gizmo part → start drag
        if (gizmo.active && gizmo.hovered != GIZMO_PART_NONE) {
            gizmo.dragging      = gizmo.hovered;
            gizmo.drag_start_x  = xpos;
            gizmo.drag_start_y  = my;

            int mi = editor.inspector.selected_mesh_index;
            if (mi >= 0 && mi < (int)scene.meshes.count) {
                Mesh* m = &scene.meshes.items[mi];

                OmdlSceneGraph* osg = NULL;
                int32_t bnode_idx = -1;
                mat4* bone_mat = get_bone_world_matrix(mi, &osg, &bnode_idx);

                if (bone_mat) {
                    glm_mat4_copy(*bone_mat, gizmo.drag_start_model);
                    // For bones, we store the local transform directly from the node
                    mat4 local; glm_mat4_identity(local);
                    glm_translate(local, osg->nodes[bnode_idx].translation);
                    mat4 rot; glm_quat_mat4(osg->nodes[bnode_idx].rotation, rot);
                    glm_mat4_mul(local, rot, local);
                    glm_scale(local, osg->nodes[bnode_idx].scale);
                    glm_mat4_copy(local, s_drag_start_local);

                    gizmo.drag_start_pivot[0] = (*bone_mat)[3][0];
                    gizmo.drag_start_pivot[1] = (*bone_mat)[3][1];
                    gizmo.drag_start_pivot[2] = (*bone_mat)[3][2];

                    int local_j = -1;
                    uint32_t mesh_node_idx = (uint32_t)(uintptr_t)m->node;
                    int32_t skin_idx = osg->nodes[mesh_node_idx].skin_idx;
                    if (skin_idx >= 0) {
                        OmdlSkin* skin = &osg->skins[skin_idx];
                        for (uint32_t j = 0; j < skin->joints_count; j++) {
                            if (osg->skin_joints[skin->joints_offset + j] == (uint32_t)bnode_idx) {
                                local_j = j; break;
                            }
                        }
                    }
                    if (local_j >= 0) {
                        if (!m->bone_overrides[local_j].active) {
                            glm_mat4_identity(m->bone_overrides[local_j].world_offset);
                            m->bone_overrides[local_j].active = true;
                        }
                        glm_mat4_copy(m->bone_overrides[local_j].world_offset, s_drag_start_bone_override);
                    } else {
                        glm_mat4_identity(s_drag_start_bone_override);
                    }
                } else {
                    glm_mat4_copy(m->model, gizmo.drag_start_model);
                    glm_mat4_copy(m->local_transform, s_drag_start_local);
                    gizmo.drag_start_pivot[0] = m->model[3][0];
                    gizmo.drag_start_pivot[1] = m->model[3][1];
                    gizmo.drag_start_pivot[2] = m->model[3][2];
                }

                if (gizmo.mode == GIZMO_MODE_ROTATE) {
                    s_rot_angle = 0.0f;
                    vec3 n = {0,0,0};
                    if      (gizmo.dragging == GIZMO_PART_RX) n[0] = 1.0f;
                    else if (gizmo.dragging == GIZMO_PART_RY) n[1] = 1.0f;
                    else if (gizmo.dragging == GIZMO_PART_RZ) n[2] = 1.0f;
                    else if (gizmo.dragging == GIZMO_PART_RS) {
                        glm_vec3_sub(camera.position, gizmo.drag_start_pivot, n);
                        glm_vec3_normalize(n);
                    }

                    // Initialize 2D screen tracking bounds for perfect mouse follow
                    vec2 sc;
                    world_to_screen(gizmo.drag_start_pivot, sc);
                    s_last_angle_2d = atan2(my - sc[1], xpos - sc[0]);

                    vec3 to_cam;
                    glm_vec3_sub(camera.position, gizmo.drag_start_pivot, to_cam);
                    s_rot_sign = glm_vec3_dot(to_cam, n) >= 0.0f ? 1.0f : -1.0f;

                    // Calculate optical illusion render_origin to fix parallax!
                    vec3 from_cam;
                    glm_vec3_sub(gizmo.drag_start_pivot, camera.position, from_cam);
                    float true_dist = glm_vec3_norm(from_cam);
                    glm_vec3_normalize(from_cam);

                    float render_dist = 0.5f;
                    if (true_dist < render_dist) render_dist = true_dist;

                    vec3 render_origin;
                    glm_vec3_copy(camera.position, render_origin);
                    glm_vec3_muladds(from_cam, render_dist, render_origin);

                    // Raycast against the rotation plane AT THE RENDER ORIGIN to find EXACT visual start point
                    vec3 ro, rd;
                    screen_to_ray(xpos, my, ro, rd);
                    float denom = glm_vec3_dot(rd, n);
                    if (fabsf(denom) > 1e-4f) {
                        vec3 p2ro;
                        glm_vec3_sub(render_origin, ro, p2ro);
                        float t = glm_vec3_dot(p2ro, n) / denom;
                        vec3 hit;
                        glm_vec3_copy(ro, hit);
                        glm_vec3_muladds(rd, t, hit);
                        glm_vec3_sub(render_origin, hit, s_rot_v_start);
                        glm_vec3_normalize(s_rot_v_start);
                    } else {
                        vec3 up = {0,1,0};
                        if (fabsf(n[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
                        glm_vec3_cross(n, up, s_rot_v_start);
                        glm_vec3_normalize(s_rot_v_start);
                    }
                    glm_vec3_copy(s_rot_v_start, s_rot_v_prev);
                }
            }
        } else if (editor_hovered_bone != NULL) {
            editor_selected_bone = editor_hovered_bone;
        } else {
            // Not over gizmo → pick mesh
            gizmo_pick_mesh(xpos, ypos); // ypos in screen-top coords (pick handles flip)
        }
    } else if (action == GLFW_RELEASE) {
        gizmo.dragging = GIZMO_PART_NONE;
    }
}
