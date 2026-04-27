#pragma once
#include <stdbool.h>
#include <cglm/cglm.h>
#include "renderer.h"
#include "camera.h"
#include "scene.h"
#include "context.h"

typedef enum {
    GIZMO_MODE_TRANSLATE = 0,
    GIZMO_MODE_ROTATE    = 1,
    GIZMO_MODE_SCALE     = 2,
    GIZMO_MODE_COUNT     = 3
} GizmoMode;

//  Which part of the gizmo the user is hovering / dragging
typedef enum {
    GIZMO_PART_NONE = -1,
    // Translate / Scale axes
    GIZMO_PART_X    = 0,
    GIZMO_PART_Y    = 1,
    GIZMO_PART_Z    = 2,
    // Plane handles (translate only)
    GIZMO_PART_XY   = 3,
    GIZMO_PART_XZ   = 4,
    GIZMO_PART_YZ   = 5,
    // Rotation arcs
    GIZMO_PART_RX     = 6,
    GIZMO_PART_RY     = 7,
    GIZMO_PART_RZ     = 8,
    // Trackball sphere (free 3D rotation inside the ring)
    GIZMO_PART_SPHERE = 9,
    // Uniform scale (center cube)
    GIZMO_PART_XYZ    = 10,
} GizmoPart;

///  State

typedef struct {
    GizmoMode  mode;
    GizmoPart  hovered;       // updated every frame from mouse pos
    GizmoPart  dragging;      // set on mouse-down, cleared on mouse-up
    bool       active;        // true when a mesh is selected

    // Drag bookkeeping
    double     drag_start_x, drag_start_y;   // screen coords where drag began
    mat4       drag_start_model;             // mesh transform at drag start
    vec3       drag_start_pivot;             // world-space gizmo origin at drag start

    // Hit-test circles (screen space) for each part — filled each frame
    // [0..2] = axis arrows/lines, [3..5] = plane squares, [6..8] = rotation arcs
    vec2       hit_centers[9];
    float      hit_radii[9];
} GizmoState;

extern GizmoState gizmo;

/// API

// Call once at startup (binds keyboard shortcut for mode cycling)
void gizmo_init(void);

// Call every frame AFTER camera update, BEFORE endFrame()
// mesh_index: currently selected mesh index (-1 = no selection → no-op)
void gizmo_render(int mesh_index);

// Feed raw mouse events:
void gizmo_mouse_move(double xpos, double ypos);
void gizmo_mouse_button(int button, int action, int mods, double xpos, double ypos);

// Mesh selection via left-click raycast.
// Returns the mesh index that was hit, or -1 on miss.
// Also updates inspector_select_mesh() / inspector_deselect() automatically.
int  gizmo_pick_mesh(double mouse_x, double mouse_y);

// Convenience: cycle gizmo mode (translate → rotate → scale → …)
void gizmo_cycle_mode(void);
void gizmo_render_overlay(void);
