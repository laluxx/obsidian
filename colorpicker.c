#include "colorpicker.h"
#include "renderer.h"
#include "context.h"
#include "theme.h"
#include "font.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// Minimum float
static float cp_minf(float a, float b) { return a < b ? a : b; }
static float cp_maxf(float a, float b) { return a > b ? a : b; }

/// HSV <-> RGB conversion

// Convert HSV (h in [0,360), s,v in [0,1]) to RGB in [0,1]
static void hsv_to_rgb(float h, float s, float v,
                        float* r, float* g, float* b) {
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
static void rgb_to_hsv(float r, float g, float b,
                        float* h, float* s, float* v) {
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
//
//  The equilateral triangle is defined by three vertices in the plane.
//  The "hue tip" vertex (V0) always points toward angle (hue degrees) on the ring,
//  so rotating hue rotates the entire triangle — colour mapping stays locked.
//
//      pure hue (S=1, V=1)
//         V0
//        /  \
//       /    \
//     V1 ———— V2
//  black      white (S=0,V=0)  (S=0,V=1)
//
static void cp_triangle_vertices(float cx, float cy,
                                  float radius, float hue_deg,
                                  float v0[2], float v1[2], float v2[2]) {
    // V0 at hue angle; V1 and V2 are 120° apart.
    float base_angle = hue_deg * ((float)GLM_PI / 180.0f);

    float a0 = base_angle;
    float a1 = base_angle + (2.0f * (float)GLM_PI / 3.0f);
    float a2 = base_angle - (2.0f * (float)GLM_PI / 3.0f);

    float r = radius * CP_TRI_RADIUS_FRAC;

    v0[0] = cx + cosf(a0) * r;  v0[1] = cy + sinf(a0) * r;
    v1[0] = cx + cosf(a1) * r;  v1[1] = cy + sinf(a1) * r;
    v2[0] = cx + cosf(a2) * r;  v2[1] = cy + sinf(a2) * r;
}

// Compute barycentric coordinates of point P relative to triangle (A,B,C).
// Returns false if the denominator is degenerate.
static bool barycentric(float px, float py,
                         float ax, float ay,
                         float bx, float by,
                         float cx, float cy,
                         float* u, float* v, float* w) {
    float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(denom) < 1e-9f) return false;

    *u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denom;
    *v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denom;
    *w = 1.0f - *u - *v;
    return true;
}

// Given barycentric (u,v,w) in the hue/black/white triangle,
// extract HSV saturation and value.
static void bary_to_sv(float u, float v, float w,
                        float* sat, float* val) {
    (void)v; // v is the black vertex weight — implicitly: val = 1 - v
    float vl = clampf(u + w, 0.0f, 1.0f);
    *val = vl;
    *sat = (vl > 1e-6f) ? clampf(u / vl, 0.0f, 1.0f) : 0.0f;
}

// Inverse: given HSV saturation and value, compute barycentric (u,v,w).
static void sv_to_bary(float sat, float val,
                        float* u, float* v, float* w) {
    *u = sat * val;          // hue-tip weight
    *v = 1.0f - val;         // black weight
    *w = (1.0f - sat) * val; // white weight
}

// Clamp barycentric coords so the point stays inside the triangle.
// Projects the point to the nearest edge or vertex.
static void bary_clamp(float* u, float* v, float* w) {
    // If all non-negative, already inside.
    if (*u >= 0.0f && *v >= 0.0f && *w >= 0.0f) return;

    // Clamp negatives to zero and renormalise.
    *u = cp_maxf(0.0f, *u);
    *v = cp_maxf(0.0f, *v);
    *w = cp_maxf(0.0f, *w);

    float sum = *u + *v + *w;
    if (sum > 1e-9f) { *u /= sum; *v /= sum; *w /= sum; }
    else             { *u = 0.0f; *v = 0.0f; *w = 1.0f; }
}

////  Smart positioning
//
//  Chooses a screen position for the picker panel that keeps it fully within
//  the viewport while staying visually close to the anchor point.
//
static void cp_smart_position(float anchor_x, float anchor_y,
                               float panel_w, float panel_h,
                               float* out_cx, float* out_cy) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    float ay_up = sh - anchor_y;

    // Anchor is the right edge where we clicked. Pop to the LEFT of the anchor.
    float cx = anchor_x - 20.0f - panel_w * 0.5f;
    float cy = ay_up;

    // If it spills past the left edge, pop to the RIGHT of the anchor.
    if (cx - panel_w * 0.5f < 10.0f)
        cx = anchor_x + 20.0f + panel_w * 0.5f;

    // Clamp vertically based on the exact visual bounds (title above, swatch below)
    float r = (panel_w - CP_PANEL_PAD * 2.0f) * 0.5f;
    float top_edge = cy + r + CP_PANEL_PAD + CP_TITLE_H;
    float bottom_edge = cy - r - CP_PANEL_PAD - CP_SWATCH_GAP - CP_SWATCH_H;

    if (bottom_edge < 10.0f) cy += (10.0f - bottom_edge);
    if (top_edge > sh - 10.0f) cy -= (top_edge - (sh - 10.0f));

    *out_cx = cx;
    *out_cy = cy;
}

///  Lifecycle

void colorpicker_init(ColorPickerState* cp) {
    memset(cp, 0, sizeof(ColorPickerState));
    cp->radius      = 130.0f;
    cp->hue         = 0.0f;
    cp->saturation  = 1.0f;
    cp->value       = 1.0f;
    cp->alpha       = 1.0f;
    sv_to_bary(cp->saturation, cp->value,
               &cp->bary_u, &cp->bary_v, &cp->bary_w);
}

void colorpicker_open(ColorPickerState* cp,
                      float anchor_x, float anchor_y,
                      float* rgba,
                      float* target_color,
                      void (*on_change)(float, float, float, float, void*),
                      void* user_data) {
    float r = rgba[0], g = rgba[1], b = rgba[2];
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);

    cp->hue        = h;
    cp->saturation = s;
    cp->value      = v;
    cp->alpha      = rgba[3];
    sv_to_bary(s, v, &cp->bary_u, &cp->bary_v, &cp->bary_w);

    cp->target_color = target_color;
    cp->on_change    = on_change;
    cp->user_data    = user_data;

    // Panel size = diameter + padding on all sides + swatch + titlebar
    float panel_w = cp->radius * 2.0f + CP_PANEL_PAD * 2.0f;
    float panel_h = cp->radius * 2.0f + CP_PANEL_PAD * 2.0f
                  + CP_SWATCH_GAP + CP_SWATCH_H + CP_TITLE_H;

    float cx, cy;
    cp_smart_position(anchor_x, anchor_y, panel_w, panel_h, &cx, &cy);

    cp->x       = cx;
    cp->y       = cy;
    cp->visible = true;
    cp->drag    = CP_DRAG_NONE;
}

void colorpicker_close(ColorPickerState* cp) {
    cp->visible      = false;
    cp->drag         = CP_DRAG_NONE;
    cp->target_color = NULL;
}

///  Change notification helper

static void cp_notify(ColorPickerState* cp) {
    float r, g, b;
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, &r, &g, &b);

    if (cp->target_color) {
        cp->target_color[0] = r;
        cp->target_color[1] = g;
        cp->target_color[2] = b;
        cp->target_color[3] = cp->alpha;
    }
    if (cp->on_change)
        cp->on_change(r, g, b, cp->alpha, cp->user_data);
}

////  Hue ring rendering

static void cp_render_hue_ring(ColorPickerState* cp) {
    float r = cp->radius;
    float d = r * 2.0f;
    // Send a single quad with ID -3. Parameters: inner_frac, outer_frac, 0, alpha
    shaderQuad2D((vec2){cp->x - r, cp->y - r}, (vec2){d, d}, -3, (vec4){CP_RING_INNER_FRAC, CP_RING_OUTER_FRAC, 0.0f, cp->alpha});
}

////  SV triangle rendering

static void cp_render_sv_triangle(ColorPickerState* cp) {
    float r = cp->radius;
    float d = r * 2.0f;
    // Send a single quad with ID -4. Parameters: hue (degrees), radius_frac, 0, alpha
    shaderQuad2D((vec2){cp->x - r, cp->y - r}, (vec2){d, d}, -4, (vec4){cp->hue, CP_TRI_RADIUS_FRAC, 0.0f, cp->alpha});
}

////  Cursor dots

static void cp_draw_cursor(float px, float py, Color fill) {
    float r = CP_CURSOR_RADIUS;
    float o = CP_CURSOR_OUTLINE;

    // Dark outline ring
    Color outline = {0.0f, 0.0f, 0.0f, 0.7f};
    circle2D((vec2){px, py}, r + o, outline);

    // White inner ring for contrast on dark backgrounds
    Color white = {1.0f, 1.0f, 1.0f, 1.0f};
    circle2D((vec2){px, py}, r + o * 0.5f, white);

    // Filled centre with the actual colour
    circle2D((vec2){px, py}, r, fill);
}

////  Panel background

static void cp_render_panel(ColorPickerState* cp, Font* font) {
    float r  = cp->radius;
    float pad = CP_PANEL_PAD;

    float panel_x = cp->x - r - pad;
    float panel_y = cp->y - r - pad; // Bottom of the main panel (excluding swatch)
    float panel_w = (r + pad) * 2.0f;
    float panel_h = (r + pad) * 2.0f + CP_TITLE_H;

    vec4 radii = {CP_PANEL_RADIUS, CP_PANEL_RADIUS,
                  CP_PANEL_RADIUS, CP_PANEL_RADIUS};

    // Subtle shadow layer
    Color shadow = {0.0f, 0.0f, 0.0f, 0.25f};
    exQuad2D((vec2){panel_x + 3.0f, panel_y - 3.0f},
             (vec2){panel_w, panel_h},
             radii, 0.0f, shadow, shadow);

    // Main background
    exQuad2D((vec2){panel_x, panel_y},
             (vec2){panel_w, panel_h},
             radii, 0.0f, CT.bg, CT.bg);

    // Title bar at TOP
    float bar_h = CP_TITLE_H;
    float bar_y = panel_y + panel_h - bar_h;
    exQuad2D((vec2){panel_x, bar_y}, (vec2){panel_w, bar_h}, (vec4){radii[0], radii[1], 0.0f, 0.0f}, 0.0f, CT.bg_alt, CT.bg_alt);

    // Title text
    if (font) {
        float ty = bar_y + bar_h * 0.5f - 2.0f; // Adjusted for visual center
        text(font, "Color Picker", panel_x + pad, ty, CT.text);
    }

    // Thin border
    Color border = {CT.bg_alt.r, CT.bg_alt.g, CT.bg_alt.b, 0.6f};
    exQuad2D((vec2){panel_x, panel_y},
             (vec2){panel_w, panel_h},
             radii, 1.5f, (Color){0,0,0,0}, border);
}

////  Swatch bar
//
//  Drawn below the wheel: left half shows previous colour (not tracked),
//  right half shows the live current colour.
//
static void cp_render_swatch(ColorPickerState* cp) {
    float r   = cp->radius;
    float pad = CP_PANEL_PAD;

    // Match the width of the main window perfectly
    float sw_x = cp->x - r - pad;
    float sw_y = cp->y - r - pad - CP_SWATCH_GAP - CP_SWATCH_H;
    float sw_w = (r + pad) * 2.0f;
    float sw_h = CP_SWATCH_H;

    vec4 radii = {CP_PANEL_RADIUS, CP_PANEL_RADIUS, CP_PANEL_RADIUS, CP_PANEL_RADIUS};

    float cr, cg, cb;
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, &cr, &cg, &cb);
    Color current = {cr, cg, cb, cp->alpha};

    // Shadow for swatch
    Color shadow = {0.0f, 0.0f, 0.0f, 0.25f};
    exQuad2D((vec2){sw_x + 3.0f, sw_y - 3.0f}, (vec2){sw_w, sw_h}, radii, 0.0f, shadow, shadow);

    // Full swatch = current colour
    exQuad2D((vec2){sw_x, sw_y}, (vec2){sw_w, sw_h},
             radii, 0.0f, current, current);

    // Thin swatch border
    Color border = {CT.bg_alt.r, CT.bg_alt.g, CT.bg_alt.b, 0.6f};
    exQuad2D((vec2){sw_x, sw_y}, (vec2){sw_w, sw_h},
             radii, 1.5f, (Color){0,0,0,0}, border);
}

////  Hex label
//
//  A small #RRGGBB label rendered inside the swatch.
//
static void cp_render_hex_label(ColorPickerState* cp, Font* font) {
    if (!font) return;

    float cr, cg, cb;
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, &cr, &cg, &cb);

    int ri = (int)clampf(cr * 255.0f + 0.5f, 0.0f, 255.0f);
    int gi = (int)clampf(cg * 255.0f + 0.5f, 0.0f, 255.0f);
    int bi = (int)clampf(cb * 255.0f + 0.5f, 0.0f, 255.0f);

    char hex[10];
    snprintf(hex, sizeof hex, "#%02X%02X%02X", ri, gi, bi);

    float r   = cp->radius;
    float pad = CP_PANEL_PAD;
    float sw_cx = cp->x;
    float sw_y  = cp->y - r - pad - CP_SWATCH_GAP - CP_SWATCH_H * 0.5f - 2.0f;

    // Choose label colour: white on dark, dark on light
    float luminance = 0.299f * cr + 0.587f * cg + 0.114f * cb;
    Color label_col = (luminance > 0.5f)
                    ? (Color){0.1f, 0.1f, 0.1f, 0.8f}
                    : (Color){0.9f, 0.9f, 0.9f, 0.8f};

    float text_w = measure_text_width(font, hex, 1.0f);
    text(font, hex, sw_cx - text_w * 0.5f, sw_y, label_col);
}

///  Hue cursor dot position

static void cp_hue_cursor_pos(ColorPickerState* cp, float* out_x, float* out_y) {
    float angle = cp->hue * ((float)GLM_PI / 180.0f);
    float rm    = cp->radius * (CP_RING_OUTER_FRAC + CP_RING_INNER_FRAC) * 0.5f;
    *out_x = cp->x + cosf(angle) * rm;
    *out_y = cp->y + sinf(angle) * rm;
}

///  SV cursor dot position

static void cp_sv_cursor_pos(ColorPickerState* cp, float* out_x, float* out_y) {
    float v0[2], v1[2], v2[2];
    cp_triangle_vertices(cp->x, cp->y, cp->radius, cp->hue, v0, v1, v2);

    *out_x = cp->bary_u * v0[0] + cp->bary_v * v1[0] + cp->bary_w * v2[0];
    *out_y = cp->bary_u * v0[1] + cp->bary_v * v1[1] + cp->bary_w * v2[1];
}

///  Per-frame

void colorpicker_update(ColorPickerState* cp, double mx, double my) {
    if (!cp->visible) return;
    (void)mx; (void)my;
    // Could animate open/close here; reserved for future work.
}

void colorpicker_render(ColorPickerState* cp, Font* font) {
    if (!cp->visible) return;

    // ── 1. Panel background ───────────────────────────────────────────────
    cp_render_panel(cp, font);

    // ── 2. Hue ring ───────────────────────────────────────────────────────
    cp_render_hue_ring(cp);

    // ── 3. SV triangle ────────────────────────────────────────────────────
    cp_render_sv_triangle(cp);

    // ── 4. Thin separator ring between wheel and triangle ─────────────────
    // A dark hairline at the inner edge of the hue ring to separate it from
    // the triangle area.  Cosmetic only.
    {
        float ri = cp->radius * CP_RING_INNER_FRAC;
        Color sep = {0.0f, 0.0f, 0.0f, 0.18f};
        int N = 80;
        float prev_x = cp->x + ri, prev_y = cp->y;
        for (int i = 1; i <= N; i++) {
            float a = (float)i / (float)N * 2.0f * (float)GLM_PI;
            float nx = cp->x + cosf(a) * ri;
            float ny = cp->y + sinf(a) * ri;
            line2D((vec2){prev_x, prev_y}, (vec2){nx, ny}, sep);
            prev_x = nx; prev_y = ny;
        }
    }

    // ── 5. Swatch bar ─────────────────────────────────────────────────────
    cp_render_swatch(cp);
    cp_render_hex_label(cp, font);

    // ── 6. Hue ring cursor ────────────────────────────────────────────────
    {
        float hx, hy;
        cp_hue_cursor_pos(cp, &hx, &hy);
        float hr, hg, hb;
        hsv_to_rgb(cp->hue, 1.0f, 1.0f, &hr, &hg, &hb);
        cp_draw_cursor(hx, hy, (Color){hr, hg, hb, 1.0f});
    }

    // ── 7. SV triangle cursor ─────────────────────────────────────────────
    {
        float sx, sy;
        cp_sv_cursor_pos(cp, &sx, &sy);
        float cr, cg, cb;
        hsv_to_rgb(cp->hue, cp->saturation, cp->value, &cr, &cg, &cb);
        cp_draw_cursor(sx, sy, (Color){cr, cg, cb, 1.0f});
    }
}


////  Hit tests

// Returns true if (mx, my) [Y-up screen space] is within the hue ring.
static bool cp_hit_hue_ring(ColorPickerState* cp, float mx, float my) {
    float dx = mx - cp->x;
    float dy = my - cp->y;
    float d  = sqrtf(dx * dx + dy * dy);
    float ri = cp->radius * CP_RING_INNER_FRAC - CP_HIT_RING_PAD;
    float ro = cp->radius * CP_RING_OUTER_FRAC + CP_HIT_RING_PAD;
    return d >= ri && d <= ro;
}

// Returns true if (mx, my) [Y-up screen space] is within the title bar.
static bool cp_hit_titlebar(ColorPickerState* cp, float mx, float my) {
    float r   = cp->radius;
    float pad = CP_PANEL_PAD;
    float panel_x = cp->x - r - pad;
    float panel_y = cp->y - r - pad;
    float panel_w = (r + pad) * 2.0f;
    float panel_h = (r + pad) * 2.0f + CP_TITLE_H;
    float bar_y   = panel_y + panel_h - CP_TITLE_H;

    return mx >= panel_x && mx <= panel_x + panel_w &&
           my >= bar_y && my <= bar_y + CP_TITLE_H;
}

// Returns true if (mx, my) is inside (or very close to) the SV triangle.
static bool cp_hit_triangle(ColorPickerState* cp, float mx, float my) {
    float v0[2], v1[2], v2[2];
    cp_triangle_vertices(cp->x, cp->y, cp->radius, cp->hue, v0, v1, v2);

    float u, v, w;
    if (!barycentric(mx, my,
                     v0[0], v0[1],
                     v1[0], v1[1],
                     v2[0], v2[1],
                     &u, &v, &w)) return false;

    float pad = CP_HIT_TRI_PAD /
                (cp->radius * CP_TRI_RADIUS_FRAC * 0.866f); // normalised tolerance
    return u >= -pad && v >= -pad && w >= -pad;
}

// Returns true if (mx, my) is anywhere inside the panel rect.
bool colorpicker_wants_mouse(ColorPickerState* cp, double mx, double my) {
    if (!cp->visible) return false;

    float r   = cp->radius;
    float pad = CP_PANEL_PAD;
    float sh  = (float)context.swapChainExtent.height;

    // Convert GLFW top-left to Y-up
    float mxf = (float)mx;
    float myf = sh - (float)my;

    float panel_x = cp->x - r - pad;
    float panel_y = cp->y - r - pad - CP_SWATCH_GAP - CP_SWATCH_H;
    float panel_w = (r + pad) * 2.0f;
    float panel_h = (r + pad) * 2.0f + CP_SWATCH_GAP + CP_SWATCH_H + CP_TITLE_H;

    return mxf >= panel_x && mxf <= panel_x + panel_w &&
           myf >= panel_y && myf <= panel_y + panel_h;
}

///  Input

bool colorpicker_mouse_move(ColorPickerState* cp, double mx, double my) {
    if (!cp->visible) return false;

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;  // Y-up

    if (cp->drag == CP_DRAG_WINDOW) {
        float sw = (float)context.swapChainExtent.width;
        cp->x = mxf - cp->drag_offset_x;
        cp->y = myf - cp->drag_offset_y;

        // Clamp so the window doesn't get permanently lost off-screen
        float r = cp->radius, pad = CP_PANEL_PAD;
        float panel_w = (r + pad) * 2.0f;
        float panel_h = (r + pad) * 2.0f + CP_TITLE_H;

        if (cp->x - r - pad < 0.0f) cp->x = r + pad;
        if (cp->x - r - pad + panel_w > sw) cp->x = sw - panel_w + r + pad;
        if (cp->y - r - pad + panel_h > sh) cp->y = sh - panel_h + r + pad;
        if (cp->y - r - pad < CP_SWATCH_GAP + CP_SWATCH_H) cp->y = r + pad + CP_SWATCH_GAP + CP_SWATCH_H;

        return true;
    }

    if (cp->drag == CP_DRAG_HUE) {
        float dx = mxf - cp->x;
        float dy = myf - cp->y;
        float angle = atan2f(dy, dx) * (180.0f / (float)GLM_PI);
        if (angle < 0.0f) angle += 360.0f;
        cp->hue = angle;
        cp_notify(cp);
        return true;
    }

    if (cp->drag == CP_DRAG_SV) {
        float v0[2], v1[2], v2[2];
        cp_triangle_vertices(cp->x, cp->y, cp->radius, cp->hue, v0, v1, v2);

        float u, v, w;
        if (barycentric(mxf, myf,
                        v0[0], v0[1],
                        v1[0], v1[1],
                        v2[0], v2[1],
                        &u, &v, &w)) {
            bary_clamp(&u, &v, &w);
            cp->bary_u = u;
            cp->bary_v = v;
            cp->bary_w = w;
            bary_to_sv(u, v, w, &cp->saturation, &cp->value);
            cp_notify(cp);
        }
        return true;
    }

    return colorpicker_wants_mouse(cp, mx, my);
}

bool colorpicker_mouse_button(ColorPickerState* cp,
                               int button, int action,
                               double mx, double my) {
    if (!cp->visible) return false;

    float sh  = (float)context.swapChainExtent.height;
    float mxf = (float)mx;
    float myf = sh - (float)my;  // Y-up

    if (button == 0 /* GLFW_MOUSE_BUTTON_LEFT */) {
        if (action == 1 /* GLFW_PRESS */) {
            if (cp_hit_titlebar(cp, mxf, myf)) {
                cp->drag = CP_DRAG_WINDOW;
                cp->drag_offset_x = mxf - cp->x;
                cp->drag_offset_y = myf - cp->y;
                return true;
            }
            if (cp_hit_hue_ring(cp, mxf, myf)) {
                cp->drag = CP_DRAG_HUE;
                // Immediately apply
                float dx = mxf - cp->x;
                float dy = myf - cp->y;
                float angle = atan2f(dy, dx) * (180.0f / (float)GLM_PI);
                if (angle < 0.0f) angle += 360.0f;
                cp->hue = angle;
                cp_notify(cp);
                return true;
            }
            if (cp_hit_triangle(cp, mxf, myf)) {
                cp->drag = CP_DRAG_SV;
                float v0[2], v1[2], v2[2];
                cp_triangle_vertices(cp->x, cp->y, cp->radius, cp->hue, v0, v1, v2);
                float u, v, w;
                if (barycentric(mxf, myf,
                                v0[0], v0[1],
                                v1[0], v1[1],
                                v2[0], v2[1],
                                &u, &v, &w)) {
                    bary_clamp(&u, &v, &w);
                    cp->bary_u = u;
                    cp->bary_v = v;
                    cp->bary_w = w;
                    bary_to_sv(u, v, w, &cp->saturation, &cp->value);
                    cp_notify(cp);
                }
                return true;
            }
            // Click outside panel -> close
            if (!colorpicker_wants_mouse(cp, mx, my)) {
                colorpicker_close(cp);
                return false;
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

void colorpicker_get_rgba(const ColorPickerState* cp,
                          float* r, float* g, float* b, float* a) {
    hsv_to_rgb(cp->hue, cp->saturation, cp->value, r, g, b);
    *a = cp->alpha;
}

void colorpicker_set_rgba(ColorPickerState* cp,
                          float r, float g, float b, float a) {
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    cp->hue        = h;
    cp->saturation = s;
    cp->value      = v;
    cp->alpha      = a;
    sv_to_bary(s, v, &cp->bary_u, &cp->bary_v, &cp->bary_w);
}
