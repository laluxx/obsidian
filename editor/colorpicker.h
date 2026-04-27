#pragma once
#include "font.h"
#include "theme.h"
#include "ui.h"
#include <stdbool.h>

///  Color Picker
//
//  A floating HSV color picker rendered entirely in 2D screen space.
//  Layout: an outer hue ring surrounding an inner equilateral triangle.
//
//  The triangle corners map to:
//    Top vertex     — pure hue (S=1, V=1)
//    Bottom-left    — black  (S=0, V=0)
//    Bottom-right   — white  (S=0, V=1)
//
//  Selection within the triangle is stored and interpolated in barycentric
//  coordinates (u, v, w) where u+v+w = 1, giving smooth, drift-free dragging.
//
//  The triangle rotates with the hue ring so the "pure colour" vertex always
//  points toward the selected hue on the ring.
//
//  Public API surface is intentionally small — see bottom of this file.


////  Constants

#define CP_RING_SEGMENTS      128     // Hue ring smoothness
#define CP_TRI_SUBDIVISIONS   48      // Triangle mesh density for colour fill
#define CP_RING_OUTER_FRAC    1.00f   // Outer radius as fraction of total radius
#define CP_RING_INNER_FRAC    0.76f   // Inner radius fraction (ring thickness)
#define CP_TRI_RADIUS_FRAC    0.70f   // Inscribed triangle radius fraction
#define CP_HIT_RING_PAD       8.0f    // Extra px hit padding on hue ring
#define CP_HIT_TRI_PAD        6.0f    // Extra px hit padding on triangle
#define CP_CURSOR_RADIUS_MIN  7.0f    // Radius when released
#define CP_CURSOR_RADIUS_MAX  12.5f   // Radius when actively dragged (25px max size)
#define CP_CURSOR_OUTLINE     2.0f    // Outline width on selection dots
#define CP_PANEL_RADIUS       10.0f   // Background panel corner radius
#define CP_PANEL_PAD          14.0f   // Padding around the wheel
#define CP_TITLE_H            32.0f   // Title bar height
#define CP_SWATCH_H           28.0f   // Height of the colour swatch bar below wheel
#define CP_SWATCH_GAP         10.0f   // Gap between wheel and swatch


////  Internal drag state

typedef enum {
    CP_DRAG_NONE = 0,
    CP_DRAG_HUE,       // Dragging the outer hue ring
    CP_DRAG_SV,        // Dragging inside the SV triangle
    CP_DRAG_WINDOW,    // Dragging the whole panel via titlebar
} ColorPickerDrag;


////  Color Picker State

typedef struct {
    // ── Unified Window State ────────────────────────────────────────────
    UIWindow      window;

    // ── Geometry ────────────────────────────────────────────────────────
    float         radius;       // Total wheel radius (ring outer edge)

    // ── HSV colour ──────────────────────────────────────────────────────
    float         hue;          // [0, 360)  degrees
    float         saturation;   // [0, 1]
    float         value;        // [0, 1]
    float         alpha;        // [0, 1]  (passed through, not edited in wheel)

    // ── Barycentric SV position ──────────────────────────────────────────
    // Barycentric coords inside the triangle (u=hue tip, v=black, w=white)
    float         bary_u;       // weight toward pure-hue vertex
    float         bary_v;       // weight toward black vertex
    float         bary_w;       // weight toward white vertex

    // ── Interaction ──────────────────────────────────────────────────────
    bool          visible;      // Synced with window.visible for backwards compatibility
    ColorPickerDrag drag;
    float         hue_cursor_t; // Animation position for hue dot
    float         hue_cursor_v; // Animation velocity for hue dot
    float         sv_cursor_t;  // Animation position for sv dot
    float         sv_cursor_v;  // Animation velocity for sv dot

    // ── Callback ─────────────────────────────────────────────────────────
    // Called every frame a value changes.  colour is {r,g,b,a} in [0,1].
    void        (*on_change)(float r, float g, float b, float a, void* user);
    void*         user_data;

    // ── Target pointer ───────────────────────────────────────────────────
    // If non-NULL, the picker writes directly to this float[4] on change.
    float*        target_color;  // float[4] RGBA in [0,1]
} ColorPickerState;


////  Lifecycle

// Initialise all fields.  Call once before use.
void colorpicker_init(ColorPickerState* cp);

// Open the picker at a smart screen position near (anchor_x, anchor_y),
// pre-loaded with the colour from rgba[4].
// anchor_* are in screen-top-left coordinates (GLFW convention).
void colorpicker_open(ColorPickerState* cp,
                      float anchor_x, float anchor_y,
                      float* rgba,          // float[4]
                      float* target_color,  // float[4] written on every change
                      void (*on_change)(float r, float g, float b, float a, void* user),
                      void* user_data);

// Close without committing (leave target_color as-is).
void colorpicker_close(ColorPickerState* cp);


////  Per-frame

// Call each frame to handle animation / hover.
// mx, my in screen-top-left coordinates (GLFW).
void colorpicker_update(ColorPickerState* cp, float dt, double mx, double my);

// Render the picker.  Draws nothing if !cp->visible.
void colorpicker_render(ColorPickerState* cp, Font* font);


////  Input events
//
//  Route raw GLFW events here.  All coordinates in screen-top-left (GLFW).

// Returns true if the picker consumed the mouse-move (is being dragged).
bool colorpicker_mouse_move(ColorPickerState* cp, double mx, double my);

// Returns true if the picker consumed the mouse-button event.
bool colorpicker_mouse_button(ColorPickerState* cp,
                               int button, int action,
                               double mx, double my);

// Returns true if the given screen point is inside the picker panel.
bool colorpicker_wants_mouse(ColorPickerState* cp, double mx, double my);


////  Utility

// Convert the current HSV + alpha to linear RGBA (each component in [0,1]).
void colorpicker_get_rgba(const ColorPickerState* cp,
                          float* r, float* g, float* b, float* a);

// Set the picker colour from RGBA without opening it.
void colorpicker_set_rgba(ColorPickerState* cp,
                          float r, float g, float b, float a);
