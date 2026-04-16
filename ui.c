#include "ui.h"
#include "renderer.h"
#include "context.h"
#include "theme.h"
#include "easing.h"
#include <string.h>

/// Internal helpers

static float ui_minf(float a, float b) { return a < b ? a : b; }
static float ui_maxf(float a, float b) { return a > b ? a : b; }

static Color ui_fade(Color c, float alpha) {
    return (Color){ c.r, c.g, c.b, c.a * alpha };
}

/// Layout

float ui_window_panel_w(const UIWindow* w) {
    return w->content_w + UI_PANEL_PAD * 2.0f;
}

float ui_window_panel_h(const UIWindow* w) {
    return w->content_h + UI_PANEL_PAD * 2.0f + UI_TITLE_H;
}

float ui_window_panel_x(const UIWindow* w) {
    return w->x - ui_window_panel_w(w) * 0.5f;
}

float ui_window_panel_y(const UIWindow* w) {
    // w->y is the vertical centre of the panel background (excluding title bar).
    // Panel bottom = centre - (content_h/2 + padding).
    return w->y - w->content_h * 0.5f - UI_PANEL_PAD;
}

float ui_window_content_x(const UIWindow* w) {
    return ui_window_panel_x(w) + UI_PANEL_PAD;
}

float ui_window_content_y(const UIWindow* w) {
    return w->y - w->content_h * 0.5f;
}

/// Smart positioning
//
//  Given an anchor point (the element that was clicked), choose a panel centre
//  that keeps the whole window inside the viewport.  Strategy:
//    • Prefer to pop LEFT of the anchor.
//    • If that would clip the left edge, pop RIGHT instead.
//    • Clamp vertically so neither the title bar above nor the bottom edge
//      below goes off-screen.
//
static void ui_smart_position(float anchor_x, float anchor_y_yup,
                               float panel_w,  float panel_h,
                               float b_min_x, float b_min_y, float b_max_x, float b_max_y,
                               float* out_cx,  float* out_cy) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    float min_x = b_min_x > 0.0f ? b_min_x : 5.0f;
    float max_x = b_max_x > 0.0f ? b_max_x : sw - 5.0f;
    float min_y = b_min_y > 0.0f ? b_min_y : 5.0f;
    float max_y = b_max_y > 0.0f ? b_max_y : sh - 5.0f;

    // Default: pop to the left of the anchor.
    float cx = anchor_x - 20.0f - panel_w * 0.5f;

    // Flip right if left edge would be clipped.
    if (cx - panel_w * 0.5f < min_x)
        cx = anchor_x + 20.0f + panel_w * 0.5f;

    // Clamp to right boundary
    if (cx + panel_w * 0.5f > max_x) {
        cx = max_x - panel_w * 0.5f;
    }

    // Final safety clamp for the left edge
    if (cx - panel_w * 0.5f < min_x) {
        cx = min_x + panel_w * 0.5f;
    }

    // Vertical centre: start aligned with anchor, then clamp.
    float cy = anchor_y_yup;

    // Exactly resolve the panel bottom and top edges relative to cy
    float content_h = panel_h - UI_PANEL_PAD * 2.0f - UI_TITLE_H;
    float bottom_edge = cy - content_h * 0.5f - UI_PANEL_PAD;
    float top_edge = bottom_edge + panel_h;

    if (bottom_edge < min_y) cy += (min_y - bottom_edge);
    if (top_edge > max_y)    cy -= (top_edge - max_y);

    *out_cx = cx;
    *out_cy = cy;
}

/// Lifecycle

void ui_window_init(UIWindow* w, const char* title) {
    memset(w, 0, sizeof(UIWindow));

    strncpy(w->title, title ? title : "", sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';

    w->anim_t  = 0.0f;
    w->closing = false;
    w->visible = false;
    w->drag    = UI_DRAG_NONE;

    extern VulkanContext context;
    w->close_icon_idx = texture_pool_add_svg(&context,
                            "./assets/icons/Close.svg", 24, 24);
}

void ui_window_open(UIWindow* w,
                    float anchor_x, float anchor_y_glfw,
                    float content_w, float content_h,
                    float b_min_x, float b_min_y, float b_max_x, float b_max_y) {
    float sh = (float)context.swapChainExtent.height;
    float anchor_y_yup = sh - anchor_y_glfw;   // flip to Y-up

    w->content_w = content_w;
    w->content_h = content_h;
    w->bound_min_x = b_min_x;
    w->bound_min_y = b_min_y;
    w->bound_max_x = b_max_x;
    w->bound_max_y = b_max_y;

    float pw = ui_window_panel_w(w);
    float ph = ui_window_panel_h(w);

    float cx, cy;
    ui_smart_position(anchor_x, anchor_y_yup, pw, ph, b_min_x, b_min_y, b_max_x, b_max_y, &cx, &cy);

    w->target_x = cx;
    w->target_y = cy;

    if (!w->visible || w->closing) {
        // Snap to position; don't spring from wherever we were last.
        w->x       = cx;
        w->y       = cy;
        w->vel_x   = 0.0f;
        w->vel_y   = 0.0f;
        w->anim_t  = 0.0f;
        w->visible = true;
        w->closing = false;
    }

    w->drag = UI_DRAG_NONE;
}

void ui_window_close(UIWindow* w) {
    w->closing = true;
    w->drag    = UI_DRAG_NONE;
}

/// Update

void ui_window_update(UIWindow* w, float dt, double mx, double my) {
    if (!w->visible) return;
    (void)mx; (void)my;

    // ── Animation progress ────────────────────────────────────────────────────
    if (w->closing) {
        w->anim_t -= 5.0f * dt;
        if (w->anim_t <= 0.0f) {
            w->anim_t  = 0.0f;
            w->visible = false;
            w->closing = false;
            return;
        }
    } else if (w->anim_t < 1.0f) {
        w->anim_t += 5.0f * dt;
        if (w->anim_t > 1.0f) w->anim_t = 1.0f;
    }

    // ── Window position spring (only when not being dragged) ──────────────────
    if (w->drag != UI_DRAG_WINDOW) {
        extern VulkanContext context;
        float sw = (float)context.swapChainExtent.width;
        float sh = (float)context.swapChainExtent.height;

        // Apply real-time bounds clamping to the TARGET
        float min_x = w->bound_min_x > 0.0f ? w->bound_min_x : 5.0f;
        float max_x = w->bound_max_x > 0.0f ? w->bound_max_x : sw - 5.0f;
        float min_y = w->bound_min_y > 0.0f ? w->bound_min_y : 5.0f;
        float max_y = w->bound_max_y > 0.0f ? w->bound_max_y : sh - 5.0f;

        float pw  = ui_window_panel_w(w);
        float ph  = ui_window_panel_h(w);
        float half_pw = pw * 0.5f;

        // Clamp target X ONLY if the window fits
        if (pw <= max_x - min_x) {
            if (w->target_x - half_pw < min_x) w->target_x = min_x + half_pw;
            if (w->target_x + half_pw > max_x) w->target_x = max_x - half_pw;
        }

        // Clamp target Y ONLY if the window fits
        if (ph <= max_y - min_y) {
            float target_py = w->target_y - w->content_h * 0.5f - UI_PANEL_PAD;
            float target_top = target_py + ph;

            if (target_py < min_y) w->target_y += (min_y - target_py);
            if (target_top > max_y) w->target_y -= (target_top - max_y);
        }

        // Apply Hooke's Law! If the target was physically shoved by a closing/opening panel,
        // the window will naturally glide alongside it!
        const float tension = 800.0f;
        const float damp    =  24.0f;

        float ax = (w->target_x - w->x) * tension - w->vel_x * damp;
        float ay = (w->target_y - w->y) * tension - w->vel_y * damp;

        w->vel_x += ax * dt;
        w->vel_y += ay * dt;
        w->x     += w->vel_x * dt;
        w->y     += w->vel_y * dt;
    }
}

/// Rendering

static float s_anim_y_offset = 0.0f;

bool ui_window_begin_render(UIWindow* w) {
    if (!w->visible) return false;

    // Easing curves match colorpicker behaviour exactly.
    float ease_val, alpha_val;
    if (w->closing) {
        ease_val  = ease_quart_in(w->anim_t);
        alpha_val = ease_quad_in(w->anim_t);
    } else {
        ease_val  = ease_cubic_bezier(w->anim_t, 0.05f, 0.9f, 0.1f, 1.05f);
        alpha_val = ease_quad_out(w->anim_t);
    }

    w->_scale = 0.70f + 0.30f * ease_val;
    w->_alpha = alpha_val;

    // Glide up cleanly from slightly below (exactly matching original colorpicker)
    s_anim_y_offset = (1.0f - ease_val) * -20.0f;

    // The scale pivot is the vertical centre of the full panel (including title).
    float ph  = ui_window_panel_h(w);
    float py  = ui_window_panel_y(w);
    w->_cx = w->x;
    w->_cy = py + ph * 0.5f;

    return true;
}

void ui_window_render_chrome(UIWindow* w, Font* font) {
    if (!w->visible) return;

    float alpha = w->_alpha;

    float pw = ui_window_panel_w(w);
    float ph = ui_window_panel_h(w);    // content + padding + title bar
    float px = ui_window_panel_x(w);
    float py = ui_window_panel_y(w);

    vec4 radii = { UI_PANEL_RADIUS, UI_PANEL_RADIUS,
                   UI_PANEL_RADIUS, UI_PANEL_RADIUS };

    // ── Drop shadow ───────────────────────────────────────────────────────────
    Color shadow = ui_fade((Color){ 0.0f, 0.0f, 0.0f, UI_SHADOW_ALPHA }, alpha);
    ui_draw_quad(w,
        px + UI_SHADOW_OFFSET, py - UI_SHADOW_OFFSET,
        pw, ph,
        radii, 0.0f, shadow, shadow);

    // ── Background ────────────────────────────────────────────────────────────
    ui_draw_quad(w, px, py, pw, ph,
        radii, 0.0f,
        ui_fade(CT.bg, alpha), ui_fade(CT.bg, alpha));

    // ── Title bar (top strip) ─────────────────────────────────────────────────
    float bar_y = py + ph - UI_TITLE_H;
    vec4 bar_radii = { radii[0], radii[1], 0.0f, 0.0f };
    ui_draw_quad(w, px, bar_y, pw, UI_TITLE_H,
        bar_radii, 0.0f,
        ui_fade(CT.bg_alt, alpha), ui_fade(CT.bg_alt, alpha));

    // ── Border ────────────────────────────────────────────────────────────────
    Color border = ui_fade((Color){ CT.bg_alt.r, CT.bg_alt.g, CT.bg_alt.b, 0.6f }, alpha);
    ui_draw_quad(w, px, py, pw, ph, radii, 1.5f, border, (Color){ 0,0,0,0 });

    // ── Title text ────────────────────────────────────────────────────────────
    if (font) {
        float ty = bar_y + UI_TITLE_H * 0.5f - 2.0f;
        ui_draw_text(w, font, w->title, px + UI_PANEL_PAD, ty,
                     ui_fade(CT.text, alpha));
    }

    // ── Close button ─────────────────────────────────────────────────────────
    float close_cx = px + pw - UI_PANEL_PAD - 6.0f;
    float close_cy = bar_y + UI_TITLE_H * 0.5f;
    Color close_col = ui_fade(w->close_hovered ? CT.error : CT.border, alpha);

    Texture2D* close_tex = texture_pool_get(w->close_icon_idx);
    if (close_tex && close_tex->loaded) {
        float hw = UI_CLOSE_SIZE * 0.5f;
        ui_draw_texture(w,
            close_cx - hw, close_cy - hw,
            UI_CLOSE_SIZE, UI_CLOSE_SIZE,
            close_tex, close_col);
    } else {
        // Fallback cross if the SVG is missing from disk.
        float hw = 5.0f;
        ui_draw_line(w,
            close_cx - hw, close_cy - hw,
            close_cx + hw, close_cy + hw,
            close_col);
        ui_draw_line(w,
            close_cx - hw, close_cy + hw,
            close_cx + hw, close_cy - hw,
            close_col);
    }
}

void ui_window_end_render(UIWindow* w) {
    (void)w;
    // Reserved for future use (e.g. stencil/scissor pop).
}

/// Scaled draw helpers
//
//  Each helper maps logical coordinates through the animation transform:
//
//      screen = window_centre + (logical - window_centre) * scale
//
//  and fades the alpha channel by w->_alpha.
//
#define UI_TX(w, x) ((w)->_cx + ((x) - (w)->_cx) * (w)->_scale)
#define UI_TY(w, y) (((w)->_cy + ((y) - (w)->_cy) * (w)->_scale) + s_anim_y_offset)
#define UI_TS(w, s) ((s) * (w)->_scale)

void ui_draw_quad(UIWindow* w,
                  float x, float y, float width, float height,
                  vec4 radii, float border, Color border_col, Color fill) {
    vec2 pos  = { UI_TX(w, x), UI_TY(w, y) };
    vec2 size = { UI_TS(w, width), UI_TS(w, height) };
    vec4 cr   = { UI_TS(w, radii[0]), UI_TS(w, radii[1]),
                  UI_TS(w, radii[2]), UI_TS(w, radii[3]) };
    exQuad2D(pos, size, cr,
             border * w->_scale,
             ui_fade(border_col, w->_alpha),
             ui_fade(fill,       w->_alpha));
}

void ui_draw_shader_quad(UIWindow* w,
                         float x, float y, float width, float height,
                         int shader_id, vec4 custom_params) {
    // Note: custom_params are fragment-space uniforms (0-1 fractions) and
    // must NOT be scaled — they describe geometry inside the shader.
    vec2 pos  = { UI_TX(w, x), UI_TY(w, y) };
    vec2 size = { UI_TS(w, width), UI_TS(w, height) };
    shaderQuad2D(pos, size, shader_id, custom_params);
}

void ui_draw_line(UIWindow* w,
                  float x0, float y0, float x1, float y1,
                  Color col) {
    vec2 a = { UI_TX(w, x0), UI_TY(w, y0) };
    vec2 b = { UI_TX(w, x1), UI_TY(w, y1) };
    line2D(a, b, ui_fade(col, w->_alpha));
}

void ui_draw_circle(UIWindow* w, float cx, float cy, float radius, Color col) {
    vec2 c = { UI_TX(w, cx), UI_TY(w, cy) };
    circle2D(c, UI_TS(w, radius), ui_fade(col, w->_alpha));
}

void ui_draw_text(UIWindow* w, Font* font, const char* str,
                  float x, float y, Color col) {
    float sx = UI_TX(w, x);
    float sy = UI_TY(w, y);
    text(font, str, sx, sy, ui_fade(col, w->_alpha));
}

void ui_draw_texture(UIWindow* w,
                     float x, float y, float width, float height,
                     Texture2D* tex, Color tint) {
    vec2 pos  = { UI_TX(w, x), UI_TY(w, y) };
    vec2 size = { UI_TS(w, width), UI_TS(w, height) };
    texture2D(pos, size, tex, ui_fade(tint, w->_alpha));
}

/// Input

bool ui_window_wants_mouse(UIWindow* w, double mx, double my) {
    if (!w->visible) return false;

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;   // Y-up

    float pw = ui_window_panel_w(w);
    float ph = ui_window_panel_h(w);
    float px = ui_window_panel_x(w);
    float py = ui_window_panel_y(w);

    return mxf >= px && mxf <= px + pw &&
           myf >= py && myf <= py + ph;
}

bool ui_window_mouse_move(UIWindow* w, double mx, double my) {
    if (!w->visible) return false;

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;   // Y-up

    // ── Update close button hover ─────────────────────────────────────────────
    {
        float pw  = ui_window_panel_w(w);
        float ph  = ui_window_panel_h(w);
        float px  = ui_window_panel_x(w);
        float py  = ui_window_panel_y(w);
        float bar_y = py + ph - UI_TITLE_H;
        float ccx = px + pw - UI_PANEL_PAD - 6.0f;
        float ccy = bar_y + UI_TITLE_H * 0.5f;
        float r   = UI_CLOSE_HIT_R;
        w->close_hovered = (mxf >= ccx - r && mxf <= ccx + r &&
                             myf >= ccy - r && myf <= ccy + r);
    }

    // ── Window drag ───────────────────────────────────────────────────────────
    if (w->drag == UI_DRAG_WINDOW) {
        float sw = (float)context.swapChainExtent.width;
        float sw_ = sw;  // shadow suppress unused
        (void)sw_;
        w->x = mxf - w->drag_offset_x;
        w->y = myf - w->drag_offset_y;

        // Clamp so the window can't escape the screen or its specific bounds.
        float min_x = w->bound_min_x > 0.0f ? w->bound_min_x : 5.0f;
        float max_x = w->bound_max_x > 0.0f ? w->bound_max_x : sw - 5.0f;
        float min_y = w->bound_min_y > 0.0f ? w->bound_min_y : 5.0f;
        float max_y = w->bound_max_y > 0.0f ? w->bound_max_y : sh - 5.0f;

        float pw  = ui_window_panel_w(w);
        float ph  = ui_window_panel_h(w);
        float half_pw = pw * 0.5f;

        // Only clamp X if the window actually fits horizontally!
        if (pw <= max_x - min_x) {
            if (w->x - half_pw < min_x)    w->x = min_x + half_pw;
            if (w->x + half_pw > max_x)    w->x = max_x - half_pw;
        }

        // Only clamp Y if the window actually fits vertically!
        if (ph <= max_y - min_y) {
            float py  = ui_window_panel_y(w);
            float top = py + ph;  // top of title bar
            if (py < min_y)                w->y += (min_y - py);
            if (top > max_y)               w->y -= (top - max_y);
        }

        // Lock spring to the dragged position.

        // Lock spring to the dragged position.
        w->target_x = w->x;
        w->target_y = w->y;
        w->vel_x    = 0.0f;
        w->vel_y    = 0.0f;

        return true;
    }

    // Return false so the specific widget can process internal drag logic!
    return false;
}

bool ui_window_mouse_button(UIWindow* w, int button, int action,
                             double mx, double my) {
    if (!w->visible) return false;

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;   // Y-up

    if (button == 0 /* GLFW_MOUSE_BUTTON_LEFT */) {
        if (action == 1 /* GLFW_PRESS */) {

            // ── Close button ──────────────────────────────────────────────────
            if (w->close_hovered) {
                ui_window_close(w);
                return true;
            }

            // ── Title bar drag ────────────────────────────────────────────────
            {
                float pw    = ui_window_panel_w(w);
                float ph    = ui_window_panel_h(w);
                float px    = ui_window_panel_x(w);
                float py    = ui_window_panel_y(w);
                float bar_y = py + ph - UI_TITLE_H;

                bool in_title = (mxf >= px && mxf <= px + pw &&
                                 myf >= bar_y && myf <= bar_y + UI_TITLE_H);
                if (in_title) {
                    w->drag          = UI_DRAG_WINDOW;
                    w->drag_offset_x = mxf - w->x;
                    w->drag_offset_y = myf - w->y;
                    return true;
                }
            }

            // ── Click outside → close ─────────────────────────────────────────
            if (!ui_window_wants_mouse(w, mx, my)) {
                ui_window_close(w);
                return false;   // let the scene handle the click too
            }

        } else if (action == 0 /* GLFW_RELEASE */) {
            if (w->drag == UI_DRAG_WINDOW) {
                w->drag = UI_DRAG_NONE;
                return true;
            }
        }
    }

    // Return false so the underlying widget (color picker, image viewer, etc.)
    // can process the click for its own internal hit tests!
    return false;
}
