#include "gizmo.h"
#include "editor.h"
#include "camera.h"
#include "scene.h"
#include "context.h"
#include "physics.h"
#include "renderer.h"
#include "keychords.h"
#include "theme.h"
#include "easing.h"
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
static vec3   s_curr_scale_v;  // live scale vector for shaft stretch visual

// Trackball sphere state
static vec3   s_sphere_v_prev;          // sphere vector from PREVIOUS frame (for incremental delta)
static versor s_sphere_accum_q;         // accumulated rotation quaternion (grows every frame)
static mat4   s_sphere_drag_rot;        // matrix form of accum_q for mesh application
static versor s_sphere_display_q;       // quaternion driving arc display orientation (current)
static versor s_sphere_spring_from_q;   // snapshot of display_q at moment of release
static bool   s_sphere_springing_back;  // true when releasing and easing back
static float  s_sphere_spring_t;        // [0..1] spring-back progress

#define MAX_GLTF_MESHES 256
static mat4 s_gltf_initial_models[MAX_GLTF_MESHES];
static mat4 s_gltf_initial_locals[MAX_GLTF_MESHES];
static int s_gltf_mesh_start = -1;
static int s_gltf_mesh_count = 0;

extern bool editor_show_bones;
extern void* editor_selected_bone;
extern void inspector_select_bone(void* bone);
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


// Map a screen-space mouse position to a unit vector on the virtual trackball sphere.
// center_s: screen-space center of gizmo, radius_px: screen radius of the outer ring.
// Returns true and fills out_v with the sphere vector (z > 0 = front hemisphere).
static bool screen_to_sphere_vec(float mx, float my, vec2 center_s, float radius_px, vec3 out_v) {
    // Work entirely in radius-normalized units: nx,ny in [-inf,+inf], unit sphere = radius 1
    float nx = (mx - center_s[0]) / radius_px;
    float ny = (my - center_s[1]) / radius_px;
    float d2 = nx * nx + ny * ny;  // squared distance from center in normalized units

    out_v[0] = nx;
    out_v[1] = ny;

    if (d2 <= 0.5f) {
        // Inside: standard sphere — z from unit sphere equation x^2+y^2+z^2=1
        out_v[2] = sqrtf(1.0f - d2);
    } else {
        // Outside: hyperbolic sheet z = 1/(2*sqrt(d2))
        // This is the exact continuation where sphere and hyperboloid meet tangentially at d2=0.5
        // At d2=0.5: sphere gives z=sqrt(0.5), hyperboloid gives z=1/(2*sqrt(0.5)) = sqrt(0.5) — CONTINUOUS
        // Derivative also matches — no jump, no snap, no speed discontinuity
        out_v[2] = 0.5f / sqrtf(d2);
    }

    glm_vec3_normalize(out_v);
    return true;
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
// Draw a small cube at tip (for Scale mode) — winding matches renderer.c cube()
// Draw a solid cube — camera-facing billboard so it always looks correct regardless of winding
// This sidesteps the backface culling issue entirely: we always emit double-sided triangles
// by drawing both a face AND its mirror, guaranteeing every face is visible from any angle.
static void draw_cube_tip(vec3 o, float half, Color col) {
    float s = half;
    // 8 corners of the cube
    vec3 c[8] = {
        {o[0]-s, o[1]-s, o[2]-s}, // 0: ---
        {o[0]+s, o[1]-s, o[2]-s}, // 1: +--
        {o[0]+s, o[1]+s, o[2]-s}, // 2: ++-
        {o[0]-s, o[1]+s, o[2]-s}, // 3: -+-
        {o[0]-s, o[1]-s, o[2]+s}, // 4: --+
        {o[0]+s, o[1]-s, o[2]+s}, // 5: +-+
        {o[0]+s, o[1]+s, o[2]+s}, // 6: +++
        {o[0]-s, o[1]+s, o[2]+s}, // 7: -++
    };
    // 6 faces, each as 2 triangles, emitted double-sided
    // Face winding chosen so outward normal is CCW from outside — then doubled for safety
    // Front  (z-): 0,1,2,3
    triangle(c[0], c[1], c[2], col); triangle(c[0], c[2], c[3], col);
    triangle(c[2], c[1], c[0], col); triangle(c[3], c[2], c[0], col);
    // Back   (z+): 5,4,7,6
    triangle(c[5], c[4], c[7], col); triangle(c[5], c[7], c[6], col);
    triangle(c[7], c[4], c[5], col); triangle(c[6], c[7], c[5], col);
    // Left   (x-): 4,0,3,7
    triangle(c[4], c[0], c[3], col); triangle(c[4], c[3], c[7], col);
    triangle(c[3], c[0], c[4], col); triangle(c[7], c[3], c[4], col);
    // Right  (x+): 1,5,6,2
    triangle(c[1], c[5], c[6], col); triangle(c[1], c[6], c[2], col);
    triangle(c[6], c[5], c[1], col); triangle(c[2], c[6], c[1], col);
    // Top    (y+): 3,2,6,7
    triangle(c[3], c[2], c[6], col); triangle(c[3], c[6], c[7], col);
    triangle(c[6], c[2], c[3], col); triangle(c[7], c[6], c[3], col);
    // Bottom (y-): 4,5,1,0
    triangle(c[4], c[5], c[1], col); triangle(c[4], c[1], c[0], col);
    triangle(c[1], c[5], c[4], col); triangle(c[0], c[1], c[4], col);
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

    if (gizmo.mode == GIZMO_MODE_SCALE) {
        // Center cube hit: check screen-space distance from gizmo origin
        vec2 sc;
        if (world_to_screen(origin, sc)) {
            float dx = (float)mx - sc[0];
            float dy = (float)my - sc[1];
            float d  = sqrtf(dx*dx + dy*dy);
            float center_px = GIZMO_SCREEN_SIZE * 0.08f * 1.5f; // generous hit radius
            if (d < center_px && d < best_dist) { best_dist = d; best = GIZMO_PART_XYZ; }
        }
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

        // Sphere: hit when mouse is inside the outer ring projected to screen
        vec2 origin_s;
        float sphere_dist = 1e9f;
        if (world_to_screen(origin, origin_s)) {
            float dx = (float)mx - origin_s[0];
            float dy = (float)my - origin_s[1];
            float dist_from_center = sqrtf(dx * dx + dy * dy);
            float ring_radius_px = scale * 1.2f; // approximate screen radius of outer ring
            // Compute actual screen radius of the outer ring at this distance
            vec3 ring_edge_world;
            glm_vec3_copy(origin, ring_edge_world);
            ring_edge_world[0] += scale * 1.2f;
            vec2 ring_edge_s;
            if (world_to_screen(ring_edge_world, ring_edge_s)) {
                ring_radius_px = fabsf(ring_edge_s[0] - origin_s[0]);
            }
            if (dist_from_center < ring_radius_px - GIZMO_ARC_HIT_PX) {
                sphere_dist = dist_from_center; // inside sphere region
            }
        }

        if (rx < GIZMO_ARC_HIT_PX && rx < best_dist) { best_dist=rx; best=GIZMO_PART_RX; }
        if (ry < GIZMO_ARC_HIT_PX && ry < best_dist) { best_dist=ry; best=GIZMO_PART_RY; }
        if (rz < GIZMO_ARC_HIT_PX && rz < best_dist) { best_dist=rz; best=GIZMO_PART_RZ; }
        if (rs < GIZMO_ARC_HIT_PX && rs < best_dist) { best_dist=rs; best=GIZMO_PART_RS; }
        // Sphere is lowest priority — only wins if nothing else is hovered
        if (sphere_dist < 1e8f && best == GIZMO_PART_NONE) { best_dist=sphere_dist; best=GIZMO_PART_SPHERE; }
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

    (void)(mx - gizmo.drag_start_x); // drag_start used by sphere and rotate sub-paths
    (void)(my - gizmo.drag_start_y);
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

        if (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
            delta[0] = roundf(delta[0]);
            delta[1] = roundf(delta[1]);
            delta[2] = roundf(delta[2]);
        }

        new_world[3][0] += delta[0];
        new_world[3][1] += delta[1];
        new_world[3][2] += delta[2];

        mat4 T; glm_mat4_identity(T);
        glm_translate(T, delta);
        glm_mat4_mul(T, s_drag_start_local, new_local);
    }
    else if (gizmo.mode == GIZMO_MODE_ROTATE) {
        // Incremental trackball: accumulate per-frame deltas — never stops at edges
        if (part == GIZMO_PART_SPHERE) {
            vec2 sc;
            if (world_to_screen(gizmo.drag_start_pivot, sc)) {
                // Compute screen radius of the arc ring at pivot
                vec3 ring_edge_world;
                glm_vec3_copy(gizmo.drag_start_pivot, ring_edge_world);
                ring_edge_world[0] += gizmo_world_scale(gizmo.drag_start_pivot) * 1.0f;
                vec2 ring_edge_s;
                float ring_radius_px = GIZMO_SCREEN_SIZE;
                if (world_to_screen(ring_edge_world, ring_edge_s)) {
                    ring_radius_px = fabsf(ring_edge_s[0] - sc[0]);
                }

                vec3 v_curr;
                screen_to_sphere_vec((float)mx, (float)my, sc, ring_radius_px, v_curr);

                // Build camera tangent frame to lift screen vectors into world space
                vec3 cam_right;
                glm_vec3_cross(camera.front, camera.up, cam_right);
                glm_vec3_normalize(cam_right);
                vec3 cam_up_n;
                glm_vec3_copy(camera.up, cam_up_n);
                glm_vec3_normalize(cam_up_n);

                // Lift PREVIOUS and CURRENT sphere vectors into world space
                vec3 w_prev, w_curr;
                for (int k = 0; k < 3; k++) {
                    w_prev[k] = s_sphere_v_prev[0]*cam_right[k]
                               + s_sphere_v_prev[1]*cam_up_n[k]
                               - s_sphere_v_prev[2]*camera.front[k];
                    w_curr[k] = v_curr[0]*cam_right[k]
                               + v_curr[1]*cam_up_n[k]
                               - v_curr[2]*camera.front[k];
                }
                glm_vec3_normalize(w_prev);
                glm_vec3_normalize(w_curr);

                // Delta rotation this frame: prev -> curr
                vec3 delta_axis;
                glm_vec3_cross(w_prev, w_curr, delta_axis);
                float axis_len = glm_vec3_norm(delta_axis);

                if (axis_len > 1e-7f) {
                    glm_vec3_scale(delta_axis, 1.0f / axis_len, delta_axis);
                    float cos_a = glm_vec3_dot(w_prev, w_curr);
                    cos_a = cos_a > 1.0f ? 1.0f : (cos_a < -1.0f ? -1.0f : cos_a);
                    float angle = acosf(cos_a);

                    // Build delta quaternion and compose into accumulator
                    versor delta_q;
                    glm_quatv(delta_q, angle, delta_axis);
                    glm_quat_normalize(delta_q);

                    // Compose: new_accum = delta * old_accum (left-multiply = world-space rotation)
                    versor new_accum;
                    glm_quat_mul(delta_q, s_sphere_accum_q, new_accum);
                    glm_quat_normalize(new_accum);
                    glm_quat_copy(new_accum, s_sphere_accum_q);
                }

                // Always advance prev to curr so next frame gets a fresh delta
                glm_vec3_copy(v_curr, s_sphere_v_prev);

                // Build pivot-relative transform matrix from accumulated quaternion
                mat4 R;
                glm_quat_mat4(s_sphere_accum_q, R);

                mat4 T, Tinv;
                glm_mat4_identity(T);
                glm_translate(T, gizmo.drag_start_pivot);
                glm_mat4_identity(Tinv);
                vec3 neg_pivot = {-gizmo.drag_start_pivot[0],
                                  -gizmo.drag_start_pivot[1],
                                  -gizmo.drag_start_pivot[2]};
                glm_translate(Tinv, neg_pivot);

                glm_mat4_mul(T, R, s_sphere_drag_rot);
                glm_mat4_mul(s_sphere_drag_rot, Tinv, s_sphere_drag_rot);

                // Apply to frozen drag-start — clean, no drift
                glm_mat4_mul(s_sphere_drag_rot, gizmo.drag_start_model, new_world);
                glm_mat4_mul(s_sphere_drag_rot, s_drag_start_local, new_local);

                // Drive display quaternion for arc follow
                glm_quat_copy(s_sphere_accum_q, s_sphere_display_q);
            }

            // Apply and return early (same bone/mesh logic as other parts)
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
                    mat4 T2, inv_start;
                    glm_mat4_inv(gizmo.drag_start_model, inv_start);
                    glm_mat4_mul(new_world, inv_start, T2);
                    if (!m->bone_overrides[local_j].active) {
                        glm_mat4_identity(m->bone_overrides[local_j].world_offset);
                        m->bone_overrides[local_j].active = true;
                    }
                    glm_mat4_mul(T2, s_drag_start_bone_override, m->bone_overrides[local_j].world_offset);
                    glm_mat4_copy(new_world, osg->world_transforms[bnode_idx]);
                }
            } else {
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
            physics_set_transform(m);
            markMeshesSSBODirty(&context);
            return;
        }

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

        if (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
            float snap_step = GLM_PI / 12.0f; // 15 degrees snap
            mesh_angle = roundf(mesh_angle / snap_step) * snap_step;
        }

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
        // Uniform scale: project mouse delta onto screen-space diagonal, map to all axes equally
        if (part == GIZMO_PART_XYZ) {
            vec3 ro_start, rd_start, ro_curr, rd_curr;
            screen_to_ray(gizmo.drag_start_x, gizmo.drag_start_y, ro_start, rd_start);
            screen_to_ray(mx, my, ro_curr, rd_curr);
            vec3 n; glm_vec3_copy(camera.front, n); glm_vec3_negate(n);
            float denom_s = glm_vec3_dot(rd_start, n);
            float denom_c = glm_vec3_dot(rd_curr,  n);
            float uniform = 1.0f;
            if (fabsf(denom_s) > 1e-5f && fabsf(denom_c) > 1e-5f) {
                vec3 p2ro_s, p2ro_c;
                glm_vec3_sub(gizmo.drag_start_pivot, ro_start, p2ro_s);
                glm_vec3_sub(gizmo.drag_start_pivot, ro_curr,  p2ro_c);
                float t_s = glm_vec3_dot(p2ro_s, n) / denom_s;
                float t_c = glm_vec3_dot(p2ro_c, n) / denom_c;
                vec3 hit_s, hit_c, diff;
                glm_vec3_copy(ro_start, hit_s); glm_vec3_muladds(rd_start, t_s, hit_s);
                glm_vec3_copy(ro_curr,  hit_c); glm_vec3_muladds(rd_curr,  t_c, hit_c);
                glm_vec3_sub(hit_c, hit_s, diff);
                // Project onto camera right+up diagonal: drag right/up = grow, left/down = shrink
                vec3 cam_right; glm_vec3_cross(camera.front, camera.up, cam_right);
                glm_vec3_normalize(cam_right);
                vec3 diag; glm_vec3_add(cam_right, camera.up, diag); glm_vec3_normalize(diag);
                float proj = glm_vec3_dot(diff, diag);
                uniform = 1.0f + proj / gizmo_scale;
            }
            if (uniform < 0.001f) uniform = 0.001f;
            scale_v[0] = uniform;
            scale_v[1] = uniform;
            scale_v[2] = uniform;
        }
        glm_vec3_copy(scale_v, s_curr_scale_v);

        if (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
            // Godot scale snap defaults to 0.1 increments
            scale_v[0] = roundf(scale_v[0] * 10.0f) / 10.0f;
            scale_v[1] = roundf(scale_v[1] * 10.0f) / 10.0f;
            scale_v[2] = roundf(scale_v[2] * 10.0f) / 10.0f;
        }

        if (part == GIZMO_PART_X) { scale_v[1] = 1.0f; scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_Y) { scale_v[0] = 1.0f; scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_Z) { scale_v[0] = 1.0f; scale_v[1] = 1.0f; }
        else if (part == GIZMO_PART_XY) { scale_v[2] = 1.0f; }
        else if (part == GIZMO_PART_XZ) { scale_v[1] = 1.0f; }
        else if (part == GIZMO_PART_YZ) { scale_v[0] = 1.0f; }
        // XYZ: all axes already set uniformly above, nothing to mask

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

    physics_set_transform(m);
    markMeshesSSBODirty(&context);
}

/// API

bool gizmo_is_dragging_mesh(int mesh_index) {
    if (gizmo.dragging == GIZMO_PART_NONE) return false;
    extern Editor editor;
    return (editor.inspector.selected_mesh_index == mesh_index);
}

static void cb_mode_translate(void) { gizmo.mode = GIZMO_MODE_TRANSLATE; }
static void cb_mode_rotate(void)    { gizmo.mode = GIZMO_MODE_ROTATE;    }
static void cb_mode_scale(void)     { gizmo.mode = GIZMO_MODE_SCALE;     }
static void cb_mode_select(void)    { inspector_deselect();              }

void gizmo_init(void) {
    memset(&gizmo, 0, sizeof(GizmoState));
    gizmo.mode     = GIZMO_MODE_TRANSLATE;
    gizmo.hovered  = GIZMO_PART_NONE;
    gizmo.dragging = GIZMO_PART_NONE;
    gizmo.active   = false;

    keychord_bind(&keymap, "q", cb_mode_rotate,    "Gizmo: Rotate",    PRESS);
    keychord_bind(&keymap, "w", cb_mode_translate, "Gizmo: Translate", PRESS);
    keychord_bind(&keymap, "e", cb_mode_scale,     "Gizmo: Scale",     PRESS);
}

void gizmo_cycle_mode(void) {
    gizmo.mode = (GizmoMode)((gizmo.mode + 1) % GIZMO_MODE_COUNT);
    (void)0; // mode names removed, use message() if needed
}

bool gizmo_get_y0_intersection(double mx, double my, vec3 out_pos) {
    vec3 ro, rd;
    screen_to_ray(mx, my, ro, rd);
    if (fabsf(rd[1]) < 1e-5f) return false;
    float t = -ro[1] / rd[1];
    if (t < 0.0f) return false;

    out_pos[0] = ro[0] + rd[0] * t;
    out_pos[1] = 0.0f;
    out_pos[2] = ro[2] + rd[2] * t;

    if (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) {
        out_pos[0] = roundf(out_pos[0]);
        out_pos[2] = roundf(out_pos[2]);
    }
    return true;
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

// dt for spring-back animation — updated from gizmo_render each frame
static float s_gizmo_dt = 0.016f;
static double s_gizmo_last_time = 0.0;

void gizmo_render(int mesh_index) {
    if (!gizmo.active) return;
    if (mesh_index < 0 || mesh_index >= (int)scene.meshes.count) return;

    // Track dt for spring-back
    double now = glfwGetTime();
    s_gizmo_dt = (float)(now - s_gizmo_last_time);
    if (s_gizmo_dt > 0.05f) s_gizmo_dt = 0.05f;
    s_gizmo_last_time = now;

    // Animate arc spring-back after sphere drag release
    if (s_sphere_springing_back) {
        s_sphere_spring_t += s_gizmo_dt * 3.5f; // ~285ms total feels weighty and satisfying
        if (s_sphere_spring_t >= 1.0f) {
            s_sphere_spring_t = 1.0f;
            s_sphere_springing_back = false;
            glm_quat_identity(s_sphere_display_q);
        } else {
            // ease_spring: overshoots slightly then settles — gorgeous on arc snap-back
            float e = ease_spring(s_sphere_spring_t);
            versor identity_q;
            glm_quat_identity(identity_q);
            // Slerp FROM the frozen snapshot TOWARD identity — correct, no drift
            glm_quat_slerp(s_sphere_spring_from_q, identity_q, e, s_sphere_display_q);
        }
    }

    Mesh* m = &scene.meshes.items[mesh_index];

    if (editor.inspector.show_aabb) {
        vec3 bmin_l, bmax_l;
        glm_vec3_copy(m->aabbMin, bmin_l);
        glm_vec3_copy(m->aabbMax, bmax_l);
        line_set_width(2.0f);

        Color aabb_col;
        if (editor.inspector.aabb_use_mesh_color) {
            bool use_attenuation = (m->attenuationColor[0] < 0.999f || m->attenuationColor[1] < 0.999f || m->attenuationColor[2] < 0.999f);
            if (use_attenuation) {
                aabb_col.r = m->attenuationColor[0] * 0.6f;
                aabb_col.g = m->attenuationColor[1] * 0.6f;
                aabb_col.b = m->attenuationColor[2] * 0.6f;
            } else {
                aabb_col.r = m->baseColorFactor[0] * 0.6f;
                aabb_col.g = m->baseColorFactor[1] * 0.6f;
                aabb_col.b = m->baseColorFactor[2] * 0.6f;
            }
            aabb_col.a = 1.0f;
        } else {
            aabb_col.r = editor.inspector.aabb_color[0];
            aabb_col.g = editor.inspector.aabb_color[1];
            aabb_col.b = editor.inspector.aabb_color[2];
            aabb_col.a = editor.inspector.aabb_color[3];
        }
        float frac = editor.inspector.aabb_full_lines ? 1.0f : 0.25f;
        for (int c = 0; c < 8; c++) {
            vec4 lp0 = {
                (c & 1) ? bmax_l[0] : bmin_l[0],
                (c & 2) ? bmax_l[1] : bmin_l[1],
                (c & 4) ? bmax_l[2] : bmin_l[2],
                1.0f
            };
            vec4 wp0;
            glm_mat4_mulv(m->model, lp0, wp0);
            vec3 pt0 = {wp0[0], wp0[1], wp0[2]};

            for (int axis = 0; axis < 3; axis++) {
                int neighbor = c ^ (1 << axis);
                if (editor.inspector.aabb_full_lines && neighbor < c) continue; // Prevent double-drawing lines!

                vec4 lp1 = {
                    (neighbor & 1) ? bmax_l[0] : bmin_l[0],
                    (neighbor & 2) ? bmax_l[1] : bmin_l[1],
                    (neighbor & 4) ? bmax_l[2] : bmin_l[2],
                    1.0f
                };
                vec4 wp1;
                glm_mat4_mulv(m->model, lp1, wp1);

                vec3 pt1 = {wp1[0], wp1[1], wp1[2]};
                vec3 end;
                for (int k = 0; k < 3; k++) end[k] = pt0[k] + (pt1[k] - pt0[k]) * frac;
                line(pt0, end, aabb_col);
            }
        }
    }

    if (m->collider_type > 0) {
        line_set_width(2.0f);

        vec3 size;
        glm_vec3_sub(m->aabbMax, m->aabbMin, size);
        glm_vec3_scale(size, 0.5f, size);

        vec3 scale = {
            glm_vec3_norm((vec3){m->model[0][0], m->model[1][0], m->model[2][0]}),
            glm_vec3_norm((vec3){m->model[0][1], m->model[1][1], m->model[2][1]}),
            glm_vec3_norm((vec3){m->model[0][2], m->model[1][2], m->model[2][2]})
        };

        mat4 unscaled_model;
        glm_mat4_copy(m->model, unscaled_model);
        if (scale[0] > 0.0001f) glm_vec3_scale(unscaled_model[0], 1.0f/scale[0], unscaled_model[0]);
        if (scale[1] > 0.0001f) glm_vec3_scale(unscaled_model[1], 1.0f/scale[1], unscaled_model[1]);
        if (scale[2] > 0.0001f) glm_vec3_scale(unscaled_model[2], 1.0f/scale[2], unscaled_model[2]);

        if (m->collider_type == 1) { // Cube
            float hx = fmaxf(size[0]*scale[0], 0.01f);
            float hy = fmaxf(size[1]*scale[1], 0.01f);
            float hz = fmaxf(size[2]*scale[2], 0.01f);
            vec3 corners[8];
            for(int c=0; c<8; c++) {
                vec4 l = { (c&1)?hx:-hx, (c&2)?hy:-hy, (c&4)?hz:-hz, 1.0f };
                vec4 w; glm_mat4_mulv(unscaled_model, l, w);
                glm_vec3_copy((vec3){w[0],w[1],w[2]}, corners[c]);
            }
            int edges[12][2] = {
                {0,1},{1,3},{3,2},{2,0},
                {4,5},{5,7},{7,6},{6,4},
                {0,4},{1,5},{2,6},{3,7}
            };
            for(int e=0; e<12; e++) {
                line(corners[edges[e][0]], corners[edges[e][1]], CT.collision);
            }
        } else if (m->collider_type == 2 || m->collider_type == 3) { // Sphere or Capsule
            float r = fmaxf(size[0]*scale[0], fmaxf(size[1]*scale[1], size[2]*scale[2]));
            if (r < 0.01f) r = 0.01f;
            float hh = 0.0f;

            if (m->collider_type == 3) { // Capsule
                r = fmaxf(size[0]*scale[0], size[2]*scale[2]);
                if (r < 0.01f) r = 0.01f;
                hh = fmaxf(0.0f, size[1]*scale[1] - r);
            }

            vec3 axes[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
            for (int a=0; a<3; a++) {
                vec3 u, v;
                vec3 ref = {0,1,0};
                if (fabsf(axes[a][1]) > 0.9f) { ref[0]=1; ref[1]=0; ref[2]=0; }
                glm_vec3_cross(axes[a], ref, u); glm_vec3_normalize(u);
                glm_vec3_cross(axes[a], u, v); glm_vec3_normalize(v);

                vec3 prev1, prev2;
                for (int i=0; i<=32; i++) {
                    float ang = (float)i / 32.0f * GLM_PI * 2.0f;
                    float cu = cosf(ang)*r, cv = sinf(ang)*r;

                    vec4 l1 = { u[0]*cu + v[0]*cv, u[1]*cu + v[1]*cv + hh, u[2]*cu + v[2]*cv, 1.0f };
                    vec4 w1; glm_mat4_mulv(unscaled_model, l1, w1);
                    vec3 pt1 = {w1[0], w1[1], w1[2]};
                    if (i>0) line(prev1, pt1, CT.collision);
                    glm_vec3_copy(pt1, prev1);

                    if (hh > 0.0f) {
                        vec4 l2 = { u[0]*cu + v[0]*cv, u[1]*cu + v[1]*cv - hh, u[2]*cu + v[2]*cv, 1.0f };
                        vec4 w2; glm_mat4_mulv(unscaled_model, l2, w2);
                        vec3 pt2 = {w2[0], w2[1], w2[2]};
                        if (i>0) line(prev2, pt2, CT.collision);
                        glm_vec3_copy(pt2, prev2);
                    }
                }
            }

            if (hh > 0.0f) {
                for (int i=0; i<4; i++) {
                    float ang = (float)i / 4.0f * GLM_PI * 2.0f;
                    vec4 l1 = { cosf(ang)*r,  hh, sinf(ang)*r, 1.0f };
                    vec4 l2 = { cosf(ang)*r, -hh, sinf(ang)*r, 1.0f };
                    vec4 w1, w2;
                    glm_mat4_mulv(unscaled_model, l1, w1);
                    glm_mat4_mulv(unscaled_model, l2, w2);
                    line((vec3){w1[0], w1[1], w1[2]}, (vec3){w2[0], w2[1], w2[2]}, CT.collision);
                }
            }
        }
    }

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

        // Draw Translation Snap Ticks
        if (gizmo.mode == GIZMO_MODE_TRANSLATE &&
            (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
             glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)) {

            // Calculate scale from the fixed pivot, so ticks don't grow as the object moves away
            float tick_size = gizmo_world_scale(gizmo.drag_start_pivot) * 0.15f;
            line_set_width(2.0f);

            // Draw enough ticks to appear completely infinite along the main guide line
            int tick_count = 1000;

            if (draw_x) {
                vec3 perp = {0.0f, -1.0f, 0.0f}; // Inverted UP
                int base_offset = (int)roundf(origin[0] - gizmo.drag_start_pivot[0]);
                for (int i = -tick_count; i <= tick_count; i++) {
                    vec3 start; glm_vec3_copy(origin, start);
                    start[0] = gizmo.drag_start_pivot[0] + (float)(base_offset + i);
                    vec3 end;
                    for(int k=0;k<3;k++) end[k] = start[k] + perp[k] * tick_size;
                    Color c = {CT.x_bright.r, CT.x_bright.g, CT.x_bright.b, 0.7f};
                    line(start, end, c);
                }
            }
            if (draw_y) {
                vec3 perp = {-1.0f, 0.0f, 0.0f}; // Inverted RIGHT
                int base_offset = (int)roundf(origin[1] - gizmo.drag_start_pivot[1]);
                for (int i = -tick_count; i <= tick_count; i++) {
                    vec3 start; glm_vec3_copy(origin, start);
                    start[1] = gizmo.drag_start_pivot[1] + (float)(base_offset + i);
                    vec3 end;
                    for(int k=0;k<3;k++) end[k] = start[k] + perp[k] * tick_size;
                    Color c = {CT.y_bright.r, CT.y_bright.g, CT.y_bright.b, 0.7f};
                    line(start, end, c);
                }
            }
            if (draw_z) {
                vec3 perp = {0.0f, -1.0f, 0.0f}; // Inverted UP
                int base_offset = (int)roundf(origin[2] - gizmo.drag_start_pivot[2]);
                for (int i = -tick_count; i <= tick_count; i++) {
                    vec3 start; glm_vec3_copy(origin, start);
                    start[2] = gizmo.drag_start_pivot[2] + (float)(base_offset + i);
                    vec3 end;
                    for(int k=0;k<3;k++) end[k] = start[k] + perp[k] * tick_size;
                    Color c = {CT.z_bright.r, CT.z_bright.g, CT.z_bright.b, 0.7f};
                    line(start, end, c);
                }
            }
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
        // Draw sphere overlay when hovered or dragging
        bool sphere_active = (gizmo.hovered == GIZMO_PART_SPHERE || gizmo.dragging == GIZMO_PART_SPHERE);
        if (sphere_active) {
            // Compute screen-space center and pixel radius of the outer ring for the sphere overlay
            // We draw a filled screen-space circle using world-space triangles on a camera-facing disc
            vec3 to_cam_n;
            glm_vec3_sub(camera.position, origin, to_cam_n);
            glm_vec3_normalize(to_cam_n);

            float alpha = (gizmo.dragging == GIZMO_PART_SPHERE) ? CT.gizmo_sphere.a : CT.gizmo_sphere.a * 0.45f;
            Color sphere_col = {CT.gizmo_sphere.r, CT.gizmo_sphere.g, CT.gizmo_sphere.b, alpha};

            float sphere_r = scale * 0.98f; // just inside the colored arcs, not touching outer ring

            // Build two tangent vectors for a camera-facing disc
            vec3 u2, v2;
            vec3 ref2 = {0,1,0};
            if (fabsf(to_cam_n[1]) > 0.9f) { ref2[0]=1; ref2[1]=0; ref2[2]=0; }
            glm_vec3_cross(to_cam_n, ref2, u2); glm_vec3_normalize(u2);
            glm_vec3_cross(to_cam_n, u2, v2);   glm_vec3_normalize(v2);

            const int DISC_SEGS = 32;
            vec3 prev_rim;
            for (int i = 0; i <= DISC_SEGS; i++) {
                float ang = (float)i / DISC_SEGS * GLM_PI * 2.0f;
                vec3 rim;
                for (int k = 0; k < 3; k++)
                    rim[k] = origin[k] + (cosf(ang)*u2[k] + sinf(ang)*v2[k]) * sphere_r;
                if (i > 0) {
                    triangle(origin, prev_rim, rim, sphere_col);
                    triangle(origin, rim, prev_rim, sphere_col);
                }
                glm_vec3_copy(rim, prev_rim);
            }
        }

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

            float display_angle = s_rot_angle;
            bool is_snapping = (glfwGetKey(context.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                                glfwGetKey(context.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);

            if (is_snapping) {
                float snap_step = GLM_PI / 12.0f;
                display_angle = roundf(display_angle / snap_step) * snap_step;
            }

            // 2. Fill rotated slice (starts exactly at the drag origin, sweeps to current angle)
            Color pie_c = {base_c.r, base_c.g, base_c.b, GIZMO_PIE_ALPHA};
            draw_pie(origin, n, s_rot_v_start, display_angle, active_scale, pie_c);

            if (is_snapping) {
                float snap_step = GLM_PI / 12.0f;
                line_set_width(2.0f);
                Color tick_c = {base_c.r, base_c.g, base_c.b, 0.7f};
                for (int i = 0; i < 24; i++) {
                    float ang = i * snap_step;
                    vec3 tick_pt, tick_inner;
                    mat4 rot; glm_mat4_identity(rot);
                    glm_rotate(rot, ang, n);
                    vec4 v_start4 = {s_rot_v_start[0], s_rot_v_start[1], s_rot_v_start[2], 0.0f};
                    vec4 v_curr4;
                    glm_mat4_mulv(rot, v_start4, v_curr4);

                    for(int k=0;k<3;k++) {
                        tick_pt[k] = origin[k] + v_curr4[k] * active_scale;
                        tick_inner[k] = origin[k] + v_curr4[k] * (active_scale * 0.90f);
                    }
                    line(tick_pt, tick_inner, tick_c);
                }
            }

            // 3. Draw lines at the two edges of the pie slice (extended slightly)
            Color line_c = (gizmo.dragging == GIZMO_PART_RS) ? CT.gizmo_outer_circle_bright : base_c;

            vec3 pt_start, pt_curr;
            mat4 rot; glm_mat4_identity(rot);
            glm_rotate(rot, display_angle, n);
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
            // Draw normal arcs when not actively dragging (or dragging sphere)
            // During sphere drag / spring-back: rotate arc axes by display quaternion
            bool sphere_animating = (gizmo.dragging == GIZMO_PART_SPHERE) || s_sphere_springing_back;

            vec3 arc_ax = {1,0,0};
            vec3 arc_ay = {0,1,0};
            vec3 arc_az = {0,0,1};

            if (sphere_animating) {
                // Build rotation matrix directly from display_q (not inverse) —
                // the trackball already encodes the correct world-space rotation.
                // We rotate arc normals WITH the drag rotation so they follow the mesh.
                mat4 rot_m;
                glm_quat_mat4(s_sphere_display_q, rot_m);
                vec4 tmp;
                vec4 ax4 = {1,0,0,0}; glm_mat4_mulv(rot_m, ax4, tmp); glm_vec3_copy(tmp, arc_ax);
                vec4 ay4 = {0,1,0,0}; glm_mat4_mulv(rot_m, ay4, tmp); glm_vec3_copy(tmp, arc_ay);
                vec4 az4 = {0,0,1,0}; glm_mat4_mulv(rot_m, az4, tmp); glm_vec3_copy(tmp, arc_az);
            }

            Color cx  = AXIS_COL(GIZMO_PART_RX, CT.x, CT.x_bright);
            Color cy  = AXIS_COL(GIZMO_PART_RY, CT.y, CT.y_bright);
            Color cz  = AXIS_COL(GIZMO_PART_RZ, CT.z, CT.z_bright);
            Color crs = AXIS_COL(GIZMO_PART_RS, CT.gizmo_outer_circle, CT.gizmo_outer_circle_bright);

            // Dim arcs while sphere is actively dragged
            if (gizmo.dragging == GIZMO_PART_SPHERE) {
                cx.a *= 0.5f; cy.a *= 0.5f; cz.a *= 0.5f; crs.a *= 0.5f;
            }

            line_set_width(GIZMO_LINE_THICK);
            draw_arc(origin, arc_ax, scale, cx);
            draw_arc(origin, arc_ay, scale, cy);
            draw_arc(origin, arc_az, scale, cz);

            line_set_width(GIZMO_LINE_THIN);
            draw_full_ring(origin, to_cam, scale * 1.2f, crs);
        }

    }
    else if (gizmo.mode == GIZMO_MODE_SCALE) {
        Color cx  = AXIS_COL(GIZMO_PART_X,      CT.x, CT.x_bright);
        Color cy  = AXIS_COL(GIZMO_PART_Y,      CT.y, CT.y_bright);
        Color cz  = AXIS_COL(GIZMO_PART_Z,      CT.z, CT.z_bright);
        Color cxy = AXIS_COL(GIZMO_PART_XY,     CT.z, CT.z_bright);
        Color cxz = AXIS_COL(GIZMO_PART_XZ,     CT.y, CT.y_bright);
        Color cyz = AXIS_COL(GIZMO_PART_YZ,     CT.x, CT.x_bright);
        Color cxyz_base = {1.0f, 1.0f, 1.0f, 1.0f};
        Color cxyz = AXIS_COL(GIZMO_PART_XYZ,   cxyz_base, ((Color){1.0f,1.0f,0.6f,1.0f}));

        // Live tip extension: when dragging an axis, stretch the shaft toward mouse
        float tip_x_t = GIZMO_ARROW_TIP * scale;
        float tip_y_t = GIZMO_ARROW_TIP * scale;
        float tip_z_t = GIZMO_ARROW_TIP * scale;

        if (gizmo.dragging == GIZMO_PART_X || gizmo.dragging == GIZMO_PART_XY ||
            gizmo.dragging == GIZMO_PART_XZ || gizmo.dragging == GIZMO_PART_XYZ) {
            float stretched = GIZMO_ARROW_TIP * scale * s_curr_scale_v[0];
            if (stretched > 0.01f) tip_x_t = stretched;
        }
        if (gizmo.dragging == GIZMO_PART_Y || gizmo.dragging == GIZMO_PART_XY ||
            gizmo.dragging == GIZMO_PART_YZ || gizmo.dragging == GIZMO_PART_XYZ) {
            float stretched = GIZMO_ARROW_TIP * scale * s_curr_scale_v[1];
            if (stretched > 0.01f) tip_y_t = stretched;
        }
        if (gizmo.dragging == GIZMO_PART_Z || gizmo.dragging == GIZMO_PART_XZ ||
            gizmo.dragging == GIZMO_PART_YZ || gizmo.dragging == GIZMO_PART_XYZ) {
            float stretched = GIZMO_ARROW_TIP * scale * s_curr_scale_v[2];
            if (stretched > 0.01f) tip_z_t = stretched;
        }

        float cube_h = scale * 0.06f;

        vec3 tip_x, tip_y, tip_z;
        glm_vec3_copy(origin, tip_x); glm_vec3_muladds(ax, tip_x_t, tip_x);
        glm_vec3_copy(origin, tip_y); glm_vec3_muladds(ay, tip_y_t, tip_y);
        glm_vec3_copy(origin, tip_z); glm_vec3_muladds(az, tip_z_t, tip_z);

        line(origin, tip_x, cx); draw_cube_tip(tip_x, cube_h, cx);
        vec3 shaft_x, shaft_y, shaft_z;
        glm_vec3_copy(tip_x, shaft_x); glm_vec3_muladds(ax, -cube_h, shaft_x);
        glm_vec3_copy(tip_y, shaft_y); glm_vec3_muladds(ay, -cube_h, shaft_y);
        glm_vec3_copy(tip_z, shaft_z); glm_vec3_muladds(az, -cube_h, shaft_z);

        line(origin, shaft_x, cx); draw_cube_tip(tip_x, cube_h, cx);
        draw_cube_tip(tip_y, cube_h, cy);
        draw_cube_tip(tip_z, cube_h, cz);

        draw_plane_square(origin, ax, ay, scale, cxy);
        draw_plane_square(origin, az, ax, scale, cxz);
        draw_plane_square(origin, ay, az, scale, cyz);

        // Center uniform-scale cube (white, like Unity)
        float center_h = scale * 0.08f;
        // Each shaft runs from the center cube face to the near face of the tip cube
        vec3 center_face_x, center_face_y, center_face_z;
        glm_vec3_copy(origin, center_face_x); glm_vec3_muladds(ax, center_h, center_face_x);
        glm_vec3_copy(origin, center_face_y); glm_vec3_muladds(ay, center_h, center_face_y);
        glm_vec3_copy(origin, center_face_z); glm_vec3_muladds(az, center_h, center_face_z);
        line(center_face_x, shaft_x, cx);
        line(center_face_y, shaft_y, cy);
        line(center_face_z, shaft_z, cz);
        draw_cube_tip(origin, center_h, cxyz);
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
        gizmo.hovered = GIZMO_PART_NONE; // freeze hover during drag so AXIS_COL never tints
        return;
    }
    // Reset scale stretch visual when not dragging
    s_curr_scale_v[0] = 1.0f;
    s_curr_scale_v[1] = 1.0f;
    s_curr_scale_v[2] = 1.0f;

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

                    // Initialize trackball sphere state
                    if (gizmo.dragging == GIZMO_PART_SPHERE) {
                        glm_mat4_identity(s_sphere_drag_rot);
                        glm_quat_identity(s_sphere_accum_q);
                        s_sphere_springing_back = false;
                        s_sphere_spring_t = 0.0f;
                        glm_quat_identity(s_sphere_display_q);
                        vec2 sc;
                        if (world_to_screen(gizmo.drag_start_pivot, sc)) {
                            vec3 ring_edge_world;
                            glm_vec3_copy(gizmo.drag_start_pivot, ring_edge_world);
                            ring_edge_world[0] += gizmo_world_scale(gizmo.drag_start_pivot) * 1.0f;
                            vec2 ring_edge_s;
                            float ring_radius_px = GIZMO_SCREEN_SIZE;
                            if (world_to_screen(ring_edge_world, ring_edge_s)) {
                                ring_radius_px = fabsf(ring_edge_s[0] - sc[0]);
                            }
                            screen_to_sphere_vec((float)xpos, (float)my, sc, ring_radius_px, s_sphere_v_prev);
                        }
                    }

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
            inspector_select_bone(editor_hovered_bone);
        } else {
            // Not over gizmo → pick mesh
            gizmo_pick_mesh(xpos, ypos); // ypos in screen-top coords (pick handles flip)
        }
    } else if (action == GLFW_RELEASE) {
        if (gizmo.dragging == GIZMO_PART_SPHERE) {
            // Snapshot current display quat as spring-back start
            s_sphere_springing_back = true;
            s_sphere_spring_t = 0.0f;
            glm_quat_copy(s_sphere_display_q, s_sphere_spring_from_q);
        }
        gizmo.dragging = GIZMO_PART_NONE;
    }
}
