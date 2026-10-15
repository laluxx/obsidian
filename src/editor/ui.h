#pragma once
#include "renderer.h"
#include "font.h"
#include <stdbool.h>

/// Constants

#define UI_PANEL_PAD      16.0f   // Inner padding between panel edge and content
#define UI_PANEL_RADIUS   10.0f   // Panel corner radius (px)
#define UI_TITLE_H        30.0f   // Title-bar height (px)
#define UI_CLOSE_SIZE     14.0f   // Close icon render size (px)
#define UI_CLOSE_HIT_R    12.0f   // Close button hit-test radius (px)
#define UI_SHADOW_OFFSET   3.0f   // Drop-shadow offset (px)
#define UI_SHADOW_ALPHA   0.25f   // Drop-shadow opacity

/// Dg state

typedef enum {
    UI_DRAG_NONE   = 0,
    UI_DRAG_WINDOW = 1,
    // Values 2+ are reserved for widget-specific drag regions
} UIDragState;

/// UIWindow

typedef struct {
    // Position (Y-up screen space, centre of the content area)
    float x, y;

    // Spring-physics target and velocity
    float target_x, target_y;
    float vel_x,    vel_y;

    // Pixel dimensions of the *content* area (excluding title bar and padding)
    float content_w, content_h;

    // Open/close animation  [0, 1]
    float anim_t;
    bool  closing;
    bool  visible;

    // Dragging
    UIDragState drag;
    float       drag_offset_x, drag_offset_y;

    // Close button
    bool  close_hovered;
    int   close_icon_idx;   // texture-pool index; loaded by ui_window_init()

    // Title shown in the header bar
    char title[64];

    // Safe area bounds (0 = default 5px screen margin)
    float bound_min_x, bound_min_y, bound_max_x, bound_max_y;

    // ── Animation transform (written by ui_window_begin_render) ──────────────
    // These are package-private; do not write them from outside ui.c.
    float _scale;
    float _cx, _cy;         // Centre of the scale transform
    float _alpha;           // Current frame alpha

} UIWindow;

/// Lifecycle

// Initialise a UIWindow.  Must be called once before any other function.
// @param title  Text shown in the header bar (copied, max 63 chars).
void ui_window_init(UIWindow* w, const char* title);

// Open (or re-open) the window near an anchor point.
// @param anchor_x / anchor_y  Screen position of the trigger element (GLFW
//                              top-left coordinates — Y is flipped internally).
// @param content_w / content_h  Size of the *content* area inside the window.
//                               The panel chrome is added on top of this.
void ui_window_open(UIWindow* w,
                    float anchor_x, float anchor_y_glfw,
                    float content_w, float content_h,
                    float b_min_x, float b_min_y, float b_max_x, float b_max_y);

// Begin a close animation.  The window becomes invisible after ~200 ms.
void ui_window_close(UIWindow* w);

/// Per-frame

// Advance spring physics and animation.  Call once per frame.
// @param mx / my  Current mouse position in GLFW top-left coordinates.
void ui_window_update(UIWindow* w, float dt, double mx, double my);

/// Rendering

// Call at the start of your widget's render function.
// Sets up the animation transform (scale + fade) and writes w->_scale,
// w->_cx, w->_cy, w->_alpha so the ui_draw_* helpers use them automatically.
// Returns false if the window should not be rendered this frame.
bool ui_window_begin_render(UIWindow* w);

// Draw the panel background, title bar, close button, and drop shadow.
// Call AFTER drawing your content so the chrome sits on top of any overlap.
void ui_window_render_chrome(UIWindow* w, Font* font);

// Call at the end of your widget's render function (currently a no-op but
// exists to make the begin/end pair symmetric for future use).
void ui_window_end_render(UIWindow* w);

/// Scaled draw helpers
//
// These wrappers apply the UIWindow animation transform (scale + alpha) to
// every draw call so widgets don't have to think about it.
//
// Coordinates are in the *logical* pre-animation space; the window centre is
// the origin of the scale transform.

void ui_draw_quad(UIWindow* w, float x, float y, float width, float height,
                  vec4 radii, float border, Color border_col, Color fill);

void ui_draw_shader_quad(UIWindow* w, float x, float y, float width, float height,
                         int shader_id, vec4 custom_params);

void ui_draw_line(UIWindow* w, float x0, float y0, float x1, float y1, Color col);

void ui_draw_circle(UIWindow* w, float cx, float cy, float radius, Color col);

void ui_draw_text(UIWindow* w, Font* font, const char* str, float x, float y, Color col);

void ui_draw_texture(UIWindow* w, float x, float y, float width, float height,
                     Texture2D* tex, Color tint);

/// Input routing

// Returns true when the mouse is anywhere inside the panel rectangle.
bool ui_window_wants_mouse(UIWindow* w, double mx, double my);

// Route GLFW mouse-move events.  Returns true when the event is consumed
// (i.e. we are dragging the window, or the pointer is inside the panel).
bool ui_window_mouse_move(UIWindow* w, double mx, double my);

// Route GLFW mouse-button events.  Returns true when the event is consumed.
// @param button  GLFW button index (0 = left)
// @param action  GLFW action (1 = press, 0 = release)
// Returns UI_DRAG_WINDOW drag if the title bar was pressed; the caller should
// check w->drag to determine what happened.
bool ui_window_mouse_button(UIWindow* w, int button, int action,
                             double mx, double my);

/// Layout helpers

// Total panel width (content_w + padding on both sides).
float ui_window_panel_w(const UIWindow* w);

// Total panel height (content_h + padding + title bar).
float ui_window_panel_h(const UIWindow* w);

// Position of the top-left corner of the panel background rect (Y-up).
float ui_window_panel_x(const UIWindow* w);
float ui_window_panel_y(const UIWindow* w);

// Position of the top-left corner of the *content* area (Y-up).
float ui_window_content_x(const UIWindow* w);
float ui_window_content_y(const UIWindow* w);
