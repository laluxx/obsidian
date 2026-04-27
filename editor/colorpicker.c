#include "colorpicker.h"
#include "renderer.h"
#include "context.h"
#include "theme.h"
#include "font.h"
#include "ui.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// Minimum float
static float cp_minf(float a, float b) { return a < b ? a : b; }
static float cp_maxf(float a, float b) { return a > b ? a : b; }

/// HSV <-> RGB conversion

// Convert HSV (h in [0,360), s,v in [0,1]) to RGB in [0,1]
static void hsv_to_rgb(float h, float s, float v, float* r, float* g, float* b) {
    if (s < 1e-6f) { *r = *g = *b = v; return; }
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float sector = h / 60.0f;
    int   i = (int)sector;
    float f = sector - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
        case 0: *r=v; *g=t; *b=p; break;
        case 1: *r=q; *g=v; *b=p; break;
        case 2: *r=p; *g=v; *b=t; break;
        case 3: *r=p; *g=q; *b=v; break;
        case 4: *r=t; *g=p; *b=v; break;
        default:*r=v; *g=p; *b=q; break;
    }
}

// Convert RGB [0,1] to HSV (h in [0,360), s,v in [0,1])
static void rgb_to_hsv(float r, float g, float b, float* h, float* s, float* v) {
    float mx = cp_maxf(r, cp_maxf(g, b));
    float mn = cp_minf(r, cp_minf(g, b));
    float delta = mx - mn;
    *v = mx;
    *s = (mx > 1e-6f) ? (delta / mx) : 0.0f;
    if (delta < 1e-6f) { *h = 0.0f; return; }
    if      (mx == r) *h = 60.0f * fmodf((g - b) / delta, 6.0f);
    else if (mx == g) *h = 60.0f * ((b - r) / delta + 2.0f);
    else              *h = 60.0f * ((r - g) / delta + 4.0f);
    if (*h < 0.0f) *h += 360.0f;
}

////  Triangle geometry

static void cp_triangle_vertices(float cx, float cy, float radius, float hue_deg,
                                 float v0[2], float v1[2], float v2[2]) {
    float base_angle = hue_deg * ((float)GLM_PI / 180.0f);
    float a0 = base_angle;
    float a1 = base_angle + (2.0f * (float)GLM_PI / 3.0f);
    float a2 = base_angle - (2.0f * (float)GLM_PI / 3.0f);
    float r = radius * CP_TRI_RADIUS_FRAC;
    v0[0] = cx + cosf(a0) * r;  v0[1] = cy + sinf(a0) * r;
    v1[0] = cx + cosf(a1) * r;  v1[1] = cy + sinf(a1) * r;
    v2[0] = cx + cosf(a2) * r;  v2[1] = cy + sinf(a2) * r;
}

static bool barycentric(float px, float py, float ax, float ay, float bx, float by,
                         float cx, float cy, float* u, float* v, float* w) {
    float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(denom) < 1e-9f) return false;
    *u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denom;
    *v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denom;
    *w = 1.0f - *u - *v;
    return true;
}

static void bary_to_sv(float u, float v, float w, float* sat, float* val) {
    (void)v;
    float vl = clampf(u + w, 0.0f, 1.0f);
    *val = vl;
    *sat = (vl > 1e-6f) ? clampf(u / vl, 0.0f, 1.0f) : 0.0f;
}

static void sv_to_bary(float sat, float val, float* u, float* v, float* w) {
    *u = sat * val;
    *v = 1.0f - val;
    *w = (1.0f - sat) * val;
}

static void bary_clamp(float* u, float* v, float* w) {
    if (*u >= 0.0f && *v >= 0.0f && *w >= 0.0f) return;
    *u = cp_maxf(0.0f, *u); *v = cp_maxf(0.0f, *v); *w = cp_maxf(0.0f, *w);
    float sum = *u + *v + *w;
    if (sum > 1e-9f) { *u /= sum; *v /= sum; *w /= sum; }
    else             { *u = 0.0f; *v = 0.0f; *w = 1.0f; }
}

// UI Helper: Get the center of the wheel based on the window's layout
static void get_wheel_center(ColorPickerState* cp, float* cx, float* cy) {
    *cx = ui_window_content_x(&cp->window) + cp->radius;
    // Y-up: The wheel sits at the top of the content area
    *cy = ui_window_content_y(&cp->window) + cp->window.content_h - cp->radius;
}

///  Lifecycle

void colorpicker_init(ColorPickerState* cp) {
    memset(cp, 0, sizeof(ColorPickerState));
    cp->radius       = 130.0f;
    cp->hue_cursor_t = 0.0f;
    cp->hue_cursor_v = 0.0f;
    cp->sv_cursor_t  = 0.0f;
    cp->sv_cursor_v  = 0.0f;
    cp->hue          = 0.0f;
    cp->saturation   = 1.0f;
    cp->value        = 1.0f;
    cp->alpha        = 1.0f;

    ui_window_init(&cp->window, "Color Picker");
    sv_to_bary(cp->saturation, cp->value, &cp->bary_u, &cp->bary_v, &cp->bary_w);
}

void colorpicker_open(ColorPickerState* cp, float anchor_x, float anchor_y,
                      float* rgba, float* target_color,
                      void (*on_change)(float, float, float, float, void*),
                      void* user_data) {
    float r = rgba[0], g = rgba[1], b = rgba[2];
    float h, s, v; rgb_to_hsv(r, g, b, &h, &s, &v);

    cp->hue        = h;
    cp->saturation = s;
    cp->value      = v;
    cp->alpha      = rgba[3];
    sv_to_bary(s, v, &cp->bary_u, &cp->bary_v, &cp->bary_w);

    cp->target_color = target_color;
    cp->on_change    = on_change;
    cp->user_data    = user_data;

    float content_w = cp->radius * 2.0f;
    float content_h = cp->radius * 2.0f; // Exclude swatch from main panel height!

    // Nudge anchor up slightly to guarantee room for the hanging swatch
    // Pass 0.0f for all bounds to use default 5px screen margin (allows overlapping panels)
    ui_window_open(&cp->window, anchor_x, anchor_y - (CP_SWATCH_GAP + CP_SWATCH_H), content_w, content_h, 0.0f, 0.0f, 0.0f, 0.0f);
    cp->visible = true; // Sync for backwards compatibility
    cp->drag = CP_DRAG_NONE;
}

void colorpicker_close(ColorPickerState* cp) {
    ui_window_close(&cp->window);
    cp->drag = CP_DRAG_NONE;
    cp->target_color = NULL; // Stop tracking immediately
}

static void cp_notify(ColorPickerState* cp) {
    float r, g, b;
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, &r, &g, &b);
    if (cp->target_color) {
        cp->target_color[0] = r; cp->target_color[1] = g;
        cp->target_color[2] = b; cp->target_color[3] = cp->alpha;
    }
    if (cp->on_change) cp->on_change(r, g, b, cp->alpha, cp->user_data);
}

///  Per-frame

void colorpicker_update(ColorPickerState* cp, float dt, double mx, double my) {
    // Keep the external visible flag perfectly synced with the window
    cp->visible = cp->window.visible;
    if (!cp->visible) return;

    ui_window_update(&cp->window, dt, mx, my);

    // True Mass-Spring-Damper Physics (Hooke's Law)
    float tension = 1200.0f; // Spring stiffness
    float damp    = 18.0f;   // Air friction (lower = more bounce, higher = stiffer)

    // Animate Hue Cursor (Pure Physics, No Clamping!)
    bool hue_active = (cp->drag == CP_DRAG_HUE);
    float target_h = hue_active ? 1.0f : 0.0f;
    float accel_h = (target_h - cp->hue_cursor_t) * tension - (cp->hue_cursor_v * damp);
    cp->hue_cursor_v += accel_h * dt;
    cp->hue_cursor_t += cp->hue_cursor_v * dt;

    // Animate SV Cursor (Pure Physics, No Clamping!)
    bool sv_active = (cp->drag == CP_DRAG_SV);
    float target_s = sv_active ? 1.0f : 0.0f;
    float accel_s = (target_s - cp->sv_cursor_t) * tension - (cp->sv_cursor_v * damp);
    cp->sv_cursor_v += accel_s * dt;
    cp->sv_cursor_t += cp->sv_cursor_v * dt;
}

void colorpicker_render(ColorPickerState* cp, Font* font) {
    if (!ui_window_begin_render(&cp->window)) return;

    // Draw the window background and chrome FIRST so it sits behind the content!
    ui_window_render_chrome(&cp->window, font);

    float cx, cy; get_wheel_center(cp, &cx, &cy);
    float r = cp->radius;
    float d = r * 2.0f;

    // ── 1. Hue ring ───────────────────────────────────────────────────────
    ui_draw_shader_quad(&cp->window, cx - r, cy - r, d, d, -3, (vec4){CP_RING_INNER_FRAC, CP_RING_OUTER_FRAC, 0.0f, cp->window._alpha});

    // ── 2. SV triangle ────────────────────────────────────────────────────
    ui_draw_shader_quad(&cp->window, cx - r, cy - r, d, d, -4, (vec4){cp->hue, CP_TRI_RADIUS_FRAC, 0.0f, cp->window._alpha});

    // ── 3. Thin separator ring ────────────────────────────────────────────
    float ri = r * CP_RING_INNER_FRAC;
    Color sep = {0.0f, 0.0f, 0.0f, 0.18f};
    float prev_x = cx + ri, prev_y = cy;
    for (int i = 1; i <= 80; i++) {
        float a = (float)i / 80.0f * 2.0f * (float)GLM_PI;
        float nx = cx + cosf(a) * ri;
        float ny = cy + sinf(a) * ri;
        ui_draw_line(&cp->window, prev_x, prev_y, nx, ny, sep);
        prev_x = nx; prev_y = ny;
    }

    // ── 4. Swatch bar ─────────────────────────────────────────────────────
    float sw_w = (r + CP_PANEL_PAD) * 2.0f;
    float sw_h = CP_SWATCH_H;
    float sw_x = cx - sw_w * 0.5f;
    float sw_y = ui_window_panel_y(&cp->window) - CP_SWATCH_GAP - CP_SWATCH_H;
    vec4 radii = {CP_PANEL_RADIUS, CP_PANEL_RADIUS, CP_PANEL_RADIUS, CP_PANEL_RADIUS};

    float cr, cg, cb;
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, &cr, &cg, &cb);
    Color current = {cr, cg, cb, 1.0f};
    Color shadow = {0.0f, 0.0f, 0.0f, 0.25f};
    Color border = {CT.bg_alt.r, CT.bg_alt.g, CT.bg_alt.b, 0.6f};

    ui_draw_quad(&cp->window, sw_x + 3.0f, sw_y - 3.0f, sw_w, sw_h, radii, 0.0f, shadow, shadow);
    ui_draw_quad(&cp->window, sw_x, sw_y, sw_w, sw_h, radii, 0.0f, current, current);
    ui_draw_quad(&cp->window, sw_x, sw_y, sw_w, sw_h, radii, 1.5f, border, (Color){0,0,0,0});

    // Hex label
    if (font) {
        int ri_c = (int)clampf(cr * 255.0f + 0.5f, 0.0f, 255.0f);
        int gi_c = (int)clampf(cg * 255.0f + 0.5f, 0.0f, 255.0f);
        int bi_c = (int)clampf(cb * 255.0f + 0.5f, 0.0f, 255.0f);
        char hex[10]; snprintf(hex, sizeof hex, "#%02X%02X%02X", ri_c, gi_c, bi_c);

        float luminance = 0.299f * cr + 0.587f * cg + 0.114f * cb;
        Color label_col = (luminance > 0.5f) ? (Color){0.1f, 0.1f, 0.1f, 0.8f} : (Color){0.9f, 0.9f, 0.9f, 0.8f};
        float text_w = measure_text_width(font, hex, 1.0f);
        ui_draw_text(&cp->window, font, hex, cx - text_w * 0.5f, sw_y + sw_h * 0.5f - 2.0f, label_col);
    }

    // ── 5. Hue ring cursor ────────────────────────────────────────────────
    float angle = cp->hue * ((float)GLM_PI / 180.0f);
    float rm    = r * (CP_RING_OUTER_FRAC + CP_RING_INNER_FRAC) * 0.5f;
    float hx = cx + cosf(angle) * rm;
    float hy = cy + sinf(angle) * rm;
    float hr, hg, hb; hsv_to_rgb(cp->hue, 1.0f, 1.0f, &hr, &hg, &hb);
    float rad = CP_CURSOR_RADIUS_MIN + (CP_CURSOR_RADIUS_MAX - CP_CURSOR_RADIUS_MIN) * cp->hue_cursor_t;

    ui_draw_circle(&cp->window, hx, hy, rad + CP_CURSOR_OUTLINE, (Color){0,0,0,0.7f});
    ui_draw_circle(&cp->window, hx, hy, rad + CP_CURSOR_OUTLINE * 0.5f, (Color){1,1,1,1});
    ui_draw_circle(&cp->window, hx, hy, rad, (Color){hr, hg, hb, 1.0f});

    // ── 6. SV triangle cursor ─────────────────────────────────────────────
    float v0[2], v1[2], v2[2];
    cp_triangle_vertices(cx, cy, r, cp->hue, v0, v1, v2);
    float sx = cp->bary_u * v0[0] + cp->bary_v * v1[0] + cp->bary_w * v2[0];
    float sy = cp->bary_u * v0[1] + cp->bary_v * v1[1] + cp->bary_w * v2[1];
    rad = CP_CURSOR_RADIUS_MIN + (CP_CURSOR_RADIUS_MAX - CP_CURSOR_RADIUS_MIN) * cp->sv_cursor_t;

    ui_draw_circle(&cp->window, sx, sy, rad + CP_CURSOR_OUTLINE, (Color){0,0,0,0.7f});
    ui_draw_circle(&cp->window, sx, sy, rad + CP_CURSOR_OUTLINE * 0.5f, (Color){1,1,1,1});
    ui_draw_circle(&cp->window, sx, sy, rad, (Color){cr, cg, cb, 1.0f});

    // End render
    ui_window_end_render(&cp->window);
}

///  Input

bool colorpicker_wants_mouse(ColorPickerState* cp, double mx, double my) {
    // Check main panel first
    if (ui_window_wants_mouse(&cp->window, mx, my)) return true;

    if (!cp->visible) return false;

    extern VulkanContext context;
    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;  // Y-up

    // Check hanging swatch area
    float cx, cy; get_wheel_center(cp, &cx, &cy);
    float sw_w = (cp->radius + CP_PANEL_PAD) * 2.0f;
    float sw_h = CP_SWATCH_H;
    float sw_x = cx - sw_w * 0.5f;
    float sw_y = ui_window_panel_y(&cp->window) - CP_SWATCH_GAP - CP_SWATCH_H;

    float pad = 4.0f;
    return mxf >= sw_x - pad && mxf <= sw_x + sw_w + pad &&
           myf >= sw_y - pad && myf <= sw_y + sw_h + pad;
}

bool colorpicker_mouse_move(ColorPickerState* cp, double mx, double my) {
    if (!cp->visible) return false;

    // Let the generic UI system handle window dragging and hover states first!
    if (ui_window_mouse_move(&cp->window, mx, my)) {
        if (cp->window.drag == UI_DRAG_WINDOW) return true;
    }

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;  // Y-up
    float cx, cy; get_wheel_center(cp, &cx, &cy);

    if (cp->drag == CP_DRAG_HUE) {
        float dx = mxf - cx;
        float dy = myf - cy;
        float angle = atan2f(dy, dx) * (180.0f / (float)GLM_PI);
        if (angle < 0.0f) angle += 360.0f;
        cp->hue = angle;
        cp_notify(cp);
        return true;
    }

    if (cp->drag == CP_DRAG_SV) {
        float v0[2], v1[2], v2[2];
        cp_triangle_vertices(cx, cy, cp->radius, cp->hue, v0, v1, v2);
        float u, v, w;
        if (barycentric(mxf, myf, v0[0], v0[1], v1[0], v1[1], v2[0], v2[1], &u, &v, &w)) {
            bary_clamp(&u, &v, &w);
            cp->bary_u = u; cp->bary_v = v; cp->bary_w = w;
            bary_to_sv(u, v, w, &cp->saturation, &cp->value);
            cp_notify(cp);
        }
        return true;
    }

    return colorpicker_wants_mouse(cp, mx, my);
}

bool colorpicker_mouse_button(ColorPickerState* cp, int button, int action, double mx, double my) {
    if (!cp->visible) return false;

    // Let UI system handle close button and titlebar drag logic
    if (ui_window_mouse_button(&cp->window, button, action, mx, my)) {
        return true;
    }

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;  // Y-up
    float cx, cy; get_wheel_center(cp, &cx, &cy);

    if (button == 0 /* GLFW_MOUSE_BUTTON_LEFT */) {
        if (action == 1 /* GLFW_PRESS */) {
            // Hit test Hue Ring
            float dx = mxf - cx; float dy = myf - cy;
            float d  = sqrtf(dx * dx + dy * dy);
            float ri = cp->radius * CP_RING_INNER_FRAC - CP_HIT_RING_PAD;
            float ro = cp->radius * CP_RING_OUTER_FRAC + CP_HIT_RING_PAD;

            if (d >= ri && d <= ro) {
                cp->drag = CP_DRAG_HUE;
                float angle = atan2f(dy, dx) * (180.0f / (float)GLM_PI);
                if (angle < 0.0f) angle += 360.0f;
                cp->hue = angle;
                cp_notify(cp);
                return true;
            }

            // Hit test SV Triangle
            float v0[2], v1[2], v2[2];
            cp_triangle_vertices(cx, cy, cp->radius, cp->hue, v0, v1, v2);
            float u, v, w;
            if (barycentric(mxf, myf, v0[0], v0[1], v1[0], v1[1], v2[0], v2[1], &u, &v, &w)) {
                float pad = CP_HIT_TRI_PAD / (cp->radius * CP_TRI_RADIUS_FRAC * 0.866f);
                if (u >= -pad && v >= -pad && w >= -pad) {
                    cp->drag = CP_DRAG_SV;
                    bary_clamp(&u, &v, &w);
                    cp->bary_u = u; cp->bary_v = v; cp->bary_w = w;
                    bary_to_sv(u, v, w, &cp->saturation, &cp->value);
                    cp_notify(cp);
                    return true;
                }
            }
        }
        else if (action == 0 /* GLFW_RELEASE */) {
            if (cp->drag != CP_DRAG_NONE) {
                cp->drag = CP_DRAG_NONE;
                return true;
            }
        }
    }

    return colorpicker_wants_mouse(cp, mx, my);
}

///  Utility

void colorpicker_get_rgba(const ColorPickerState* cp, float* r, float* g, float* b, float* a) {
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, r, g, b);
    *a = cp->alpha;
}

void colorpicker_set_rgba(ColorPickerState* cp, float r, float g, float b, float a) {
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    cp->hue        = h;
    cp->saturation = s;
    cp->value      = v;
    cp->alpha      = a;
    sv_to_bary(s, v, &cp->bary_u, &cp->bary_v, &cp->bary_w);
}
