#include "editor.h"
#include "renderer.h"
#include "context.h"
#include "scene.h"
#include "font.h"
#include "keychords.h"
#include "theme.h"
#include "gizmo.h"
#include "colorpicker.h"
#include "easing.h"
#include "vulkan_setup.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <GLFW/glfw3.h>

///  Globals

Editor editor = {0};

// UI Input State
static double s_ui_mx = 0.0;
static double s_ui_my = 0.0;
static bool   s_ui_mdown = false;
static bool   s_ui_mclicked = false;
static void* s_ui_active_id = NULL;
static float  s_ui_drag_start_x = 0.0f;
static float  s_ui_drag_start_y = 0.0f;
static float  s_ui_drag_start_val = 0.0f;

static ColorPickerState s_color_picker;
static int32_t s_icon_lock = -1;

static void panel_get_rect(Panel* p, float* out_x, float* out_y, float* out_w, float* out_h);

typedef enum { MSG_INFO, MSG_SUCCESS, MSG_WARNING, MSG_ERROR } MessageType;
typedef struct {
    char text[128];
    MessageType type;
    float timer;
    bool active;

    // Physics & Interaction State
    float x, y;
    float vel_x, vel_y;
    bool is_dragging;
    float drag_offset_x, drag_offset_y;
    bool hovered;
    float hover_t; // [0,1] for color lerping
} EditorMessage;

// Initialize deeply offscreen so the first pop springs in naturally
static EditorMessage s_message = { .x = 20.0f, .y = -200.0f };

void message(MessageType type, const char* text) {
    strncpy(s_message.text, text, sizeof(s_message.text) - 1);
    s_message.text[sizeof(s_message.text) - 1] = '\0';
    s_message.type = type;
    s_message.timer = 3.0f; // Stay on screen for 3 seconds
    s_message.active = true;

    // If it was completely offscreen, snap it to the bottom to prepare for a fresh spring bounce
    if (s_message.y < -100.0f) {
        s_message.x = 20.0f;
        s_message.y = -100.0f;
        s_message.vel_x = 0.0f;
        s_message.vel_y = 0.0f;
    }
}

// --- Image Viewer State ---
typedef struct {
    UIWindow window;
    int32_t  tex_idx;
    bool     visible;
    char     filepath[256];
    char     filename[64];
    size_t   file_size;
} ImageViewerState;

static ImageViewerState s_image_viewer = {0};
static double s_last_item_click_time = 0.0;
static int s_last_clicked_item = -1;

// --- Search & Incremental Caching State ---
static bool  s_is_searching = false;
static char  s_search_query[64] = "";
static float s_search_anim_t = 0.0f;

static int   s_search_matches[256];
static int   s_search_match_count = 0;
static int   s_search_current_idx = -1;

static int   s_search_cursor = 0;
static double s_search_last_key_time = 0.0;
static int   s_search_prev_match_idx = 0;

static int   s_saved_expanded_count = 0;
static char  s_saved_expanded_paths[64][256];
static int   s_saved_selected_index = -1;
static float s_saved_scroll_y = 0.0f;
static float s_saved_scroll_target = 0.0f;

static KeyChordMap editor_search_keymap;
extern void set_active_text_input(void (*cb)(char));

// Smooth Scrolling State
static float s_fs_scroll_y = 0.0f;
static float s_fs_scroll_target = 0.0f;
static float s_fs_scroll_start = 0.0f;
static float s_fs_scroll_t = 1.0f;

#define CACHE_MAX_DIRS 16
#define CACHE_MAX_FILES 2048

typedef struct {
    char root[256];
    char paths[CACHE_MAX_FILES][128];
    int count;
} FsCache;

static FsCache s_fs_cache[CACHE_MAX_DIRS];
static int s_fs_cache_count = 0;

static FsCache* find_cached_parent(const char* dir) {
    FsCache* best = NULL;
    size_t max_len = 0;
    size_t dir_len = strlen(dir);
    for(int i = 0; i < s_fs_cache_count; i++) {
        size_t len = strlen(s_fs_cache[i].root);
        if (len > max_len && len < dir_len && strncmp(s_fs_cache[i].root, dir, len) == 0) {
            best = &s_fs_cache[i];
            max_len = len;
        }
    }
    return best;
}

static void populate_cache_recursive(const char* root, FsCache* c, const char* current_dir) {
    DIR* dir = opendir(current_dir);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // skip hidden
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", current_dir, entry->d_name);

        struct stat st;
        if (stat(full, &st) == 0) {
            if (c->count < CACHE_MAX_FILES) {
                const char* rel = full + strlen(root);
                if (rel[0] == '/') rel++;
                strncpy(c->paths[c->count], rel, 127);
                c->count++;
            }
            if (S_ISDIR(st.st_mode)) populate_cache_recursive(root, c, full);
        }
    }
    closedir(dir);
}

static FsCache* get_or_build_cache(const char* dir, bool force_refresh) {
    if (!force_refresh) {
        for(int i = 0; i < s_fs_cache_count; i++) {
            if (strcmp(s_fs_cache[i].root, dir) == 0) return &s_fs_cache[i];
        }
        FsCache* parent = find_cached_parent(dir);
        if (parent && s_fs_cache_count < CACHE_MAX_DIRS) {
            FsCache* c = &s_fs_cache[s_fs_cache_count++];
            strncpy(c->root, dir, sizeof(c->root)-1);
            c->count = 0;
            const char* prefix = dir + strlen(parent->root);
            if (prefix[0] == '/') prefix++;
            size_t prefix_len = strlen(prefix);
            for(int i = 0; i < parent->count; i++) {
                if (strncmp(parent->paths[i], prefix, prefix_len) == 0 && parent->paths[i][prefix_len] == '/') {
                    strncpy(c->paths[c->count], parent->paths[i] + prefix_len + 1, 127);
                    c->count++;
                }
            }
            return c; // Incremental build successful!
        }
    }
    if (s_fs_cache_count >= CACHE_MAX_DIRS) s_fs_cache_count = 0; // Simple eviction
    FsCache* c = &s_fs_cache[s_fs_cache_count++];
    strncpy(c->root, dir, sizeof(c->root)-1);
    c->count = 0;
    populate_cache_recursive(dir, c, dir);
    return c;
}

static Color lerp_color(Color a, Color b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

static void editor_get_safe_area(float* out_min_x, float* out_min_y, float* out_max_x, float* out_max_y) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    float min_x = 5.0f;
    float min_y = 5.0f;
    float max_x = sw - 5.0f;
    float max_y = sh - 5.0f;
    float px, py, pw, ph;

    panel_get_rect(&editor.panels[PANEL_LEFT], &px, &py, &pw, &ph);
    if (px + pw + 5.0f > min_x) min_x = px + pw + 5.0f;

    panel_get_rect(&editor.panels[PANEL_RIGHT], &px, &py, &pw, &ph);
    if (px - 5.0f < max_x) max_x = px - 5.0f;

    panel_get_rect(&editor.panels[PANEL_BOTTOM], &px, &py, &pw, &ph);
    if (py + ph + 5.0f > min_y) min_y = py + ph + 5.0f;

    panel_get_rect(&editor.panels[PANEL_TOP], &px, &py, &pw, &ph);
    if (py - 5.0f < max_y) max_y = py - 5.0f;

    *out_min_x = min_x;
    *out_min_y = min_y;
    *out_max_x = max_x;
    *out_max_y = max_y;
}

static void image_viewer_init(ImageViewerState* iv) {
    memset(iv, 0, sizeof(*iv));
    ui_window_init(&iv->window, "Image Viewer");
}

static void image_viewer_open(ImageViewerState* iv, const FileItem* item, float anchor_x, float anchor_y) {
    int32_t tex_idx = texture_pool_add(&context, item->full_path);
    if (tex_idx < 0) {
        message(MSG_ERROR, "Failed to load image");
        return;
    }

    Texture2D* tex = texture_pool_get(tex_idx);
    iv->tex_idx = tex_idx;

    // Cache the exact file metadata for the Inspector
    strncpy(iv->filepath, item->full_path, sizeof(iv->filepath) - 1);
    iv->filepath[sizeof(iv->filepath) - 1] = '\0';
    strncpy(iv->filename, item->name, sizeof(iv->filename) - 1);
    iv->filename[sizeof(iv->filename) - 1] = '\0';
    iv->file_size = item->size_bytes;

    strncpy(iv->window.title, iv->filename, sizeof(iv->window.title) - 1);
    iv->window.title[sizeof(iv->window.title) - 1] = '\0';

    float min_x, min_y, max_x, max_y;
    editor_get_safe_area(&min_x, &min_y, &max_x, &max_y);

    // Restrict popup to 90% of the available safe workspace area
    float safe_w = (max_x - min_x) * 0.9f;
    float safe_h = (max_y - min_y) * 0.9f;
    float cw = (float)tex->width;
    float ch = (float)tex->height;

    if (cw > safe_w || ch > safe_h) {
        float scale = fminf(safe_w / cw, safe_h / ch);
        cw *= scale;
        ch *= scale;
    }

    ui_window_open(&iv->window, anchor_x, anchor_y, cw, ch, min_x, min_y, max_x, max_y);
    iv->visible = true;
}

static void image_viewer_update(ImageViewerState* iv, float dt, double mx, double my) {
    iv->visible = iv->window.visible;
    if (!iv->visible) return;

    // Pump the dynamic safe bounds into the UI engine every single frame!
    float min_x, min_y, max_x, max_y;
    editor_get_safe_area(&min_x, &min_y, &max_x, &max_y);
    iv->window.bound_min_x = min_x;
    iv->window.bound_min_y = min_y;
    iv->window.bound_max_x = max_x;
    iv->window.bound_max_y = max_y;

    ui_window_update(&iv->window, dt, mx, my);
}

static void image_viewer_render(ImageViewerState* iv, Font* font) {
    if (!ui_window_begin_render(&iv->window)) return;
    ui_window_render_chrome(&iv->window, font);

    Texture2D* tex = texture_pool_get(iv->tex_idx);
    if (tex && tex->loaded) {
        float cx = ui_window_content_x(&iv->window);
        float cy = ui_window_content_y(&iv->window);
        float cw = iv->window.content_w;
        float ch = iv->window.content_h;
        vec4 radii = {4.0f, 4.0f, 4.0f, 4.0f};

        // Draw a dark background directly behind the image, then the image, then a thin inner border
        ui_draw_quad(&iv->window, cx, cy, cw, ch, radii, 0.0f, (Color){0,0,0,0}, (Color){0.05f, 0.05f, 0.05f, 1.0f});
        ui_draw_texture(&iv->window, cx, cy, cw, ch, tex, (Color){1.0f, 1.0f, 1.0f, 1.0f});

        Color border = {CT.bg_alt.r, CT.bg_alt.g, CT.bg_alt.b, 0.6f};
        ui_draw_quad(&iv->window, cx, cy, cw, ch, radii, 1.5f, border, (Color){0,0,0,0});
    }
    ui_window_end_render(&iv->window);
}

static void on_color_picked(float r, float g, float b, float a, void* user) {
    (void)r; (void)g; (void)b; (void)a; (void)user;
    markMeshesSSBODirty(&context);
}

bool editor_wants_mouse(void) {
    float sh = (float)context.swapChainExtent.height;
    double raw_y = sh - s_ui_my;
    if (s_image_viewer.visible && ui_window_wants_mouse(&s_image_viewer.window, s_ui_mx, raw_y)) return true;
    if (s_color_picker.visible && colorpicker_wants_mouse(&s_color_picker, s_ui_mx, raw_y)) return true;

    if (editor.font) {
        float h = editor.font->ascent - editor.font->descent + 32.0f;
        if (s_message.active || s_message.y > -(h + 5.0f)) {
            float w = measure_text_width(editor.font, s_message.text, 1.0f) + 32.0f;
            if (s_ui_mx >= s_message.x && s_ui_mx <= s_message.x + w &&
                s_ui_my >= s_message.y && s_ui_my <= s_message.y + h) return true;
            if (s_message.is_dragging) return true;
        }
    }

    if (s_ui_active_id != NULL) return true; // Actively dragging UI
    for (int i = 0; i < PANEL_COUNT; i++) {
        Panel* p = &editor.panels[i];
        if (p->t < 0.0005f) continue;
        float x, y, w, h;
        panel_get_rect(p, &x, &y, &w, &h);
        if (s_ui_mx >= x && s_ui_mx <= x + w && s_ui_my >= y && s_ui_my <= y + h) {
            return true; // Hovering an open panel
        }
    }
    return false;
}

void editor_mouse_move(double xpos, double ypos) {
    float sh = (float)context.swapChainExtent.height;
    s_ui_mx = xpos;
    s_ui_my = sh - ypos; // Align with panel Y-up coordinates
    if (s_image_viewer.visible) {
        ui_window_mouse_move(&s_image_viewer.window, xpos, ypos);
    }
    if (s_color_picker.visible) {
        colorpicker_mouse_move(&s_color_picker, xpos, ypos);
    }
}

void editor_mouse_button(int button, int action) {
    float sh = (float)context.swapChainExtent.height;
    double raw_y = sh - s_ui_my;

    // Highest Z-order popup gets first priority
    if (s_image_viewer.visible) {
        if (ui_window_mouse_button(&s_image_viewer.window, button, action, s_ui_mx, raw_y)) {
            return;
        }
    }
    if (s_color_picker.visible) {
        if (colorpicker_mouse_button(&s_color_picker, button, action, s_ui_mx, raw_y)) {
            return; // Input consumed by the color picker
        }
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            // Check Message Drag
            if (editor.font && (s_message.active || s_message.y > -80.0f)) {
                float w = measure_text_width(editor.font, s_message.text, 1.0f) + 32.0f;
                float h = editor.font->ascent - editor.font->descent + 32.0f;
                if (s_ui_mx >= s_message.x && s_ui_mx <= s_message.x + w &&
                    s_ui_my >= s_message.y && s_ui_my <= s_message.y + h) {
                    s_message.is_dragging = true;
                    s_message.drag_offset_x = (float)s_ui_mx - s_message.x;
                    s_message.drag_offset_y = (float)s_ui_my - s_message.y;
                    s_ui_mdown = true;
                    s_ui_mclicked = true;
                    return; // Consumed!
                }
            }

            s_ui_mdown = true;
            s_ui_mclicked = true;
        } else if (action == GLFW_RELEASE) {
            if (s_message.is_dragging) {
                s_message.is_dragging = false;
                return; // Consumed!
            }
            s_ui_mdown = false;
            s_ui_active_id = NULL; // Release drag
        }
    }
}

// Animation constants
#define EDITOR_MAX_DT      0.05f   // cap dt to avoid jumps after focus loss
#define EDITOR_ANIM_SPEED  12.0f   // how fast t chases target_t

#define PANEL_RADIUS   8.0f  // Corner radius of panels
#define TITLE_H       32.0f  // Title-bar height
#define PAD           14.0f  // Inner padding
#define LH_EXTRA       5.0f  // Line height multiplier

/// Panel layout
//
//  Coordinate system: (0,0) bottom-left, Y grows up.
//  Panels slide in from their respective edges.
//
//  eased t=0 → fully off-screen
//  eased t=1 → fully on-screen, flush with the viewport edge
//
static float panel_eased(Panel* p) {
    EaseFn fn = p->ease_fn ? p->ease_fn : ease_quart_out;
    return fn(clampf(p->t, 0.0f, 1.0f));
}

static void panel_get_rect(Panel* p,
                            float* out_x, float* out_y,
                            float* out_w, float* out_h) {
    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;
    float et = panel_eased(p);

    switch (p->side) {

        case PANEL_BOTTOM: {
            // Slides up from below the screen.
            // When t=1: y = 0 (bottom edge flush), height = size.
            // When t=0: y = -size (completely off-screen below).
            float h = p->size;
            *out_x = 0.0f;
            *out_w = sw;
            *out_h = h;
            *out_y = lerpf(-h, 0.0f, et);
            break;
        }

        case PANEL_TOP: {
            // Slides down from above the screen.
            // When t=1: y = sh - size (top edge flush with viewport top).
            // When t=0: y = sh (completely off-screen above).
            float h = p->size;
            *out_x = 0.0f;
            *out_w = sw;
            *out_h = h;
            *out_y = lerpf(sh, sh - h, et);
            break;
        }

        case PANEL_RIGHT: {
            // Slides in from the right edge.
            // When t=1: x = sw - size.
            // When t=0: x = sw (completely off-screen right).
            float w = p->size;
            *out_y = 0.0f;
            *out_w = w;
            *out_h = sh;
            *out_x = lerpf(sw, sw - w, et);
            break;
        }

        case PANEL_LEFT: {
            // Slides in from the left edge.
            // When t=1: x = 0.
            // When t=0: x = -size (completely off-screen left).
            float w = p->size;
            *out_y = 0.0f;
            *out_w = w;
            *out_h = sh;
            *out_x = lerpf(-w, 0.0f, et);
            break;
        }

        default:
            *out_x = *out_y = *out_w = *out_h = 0.0f;
            break;
    }
}

///  Panel Chrome

static void panel_draw_chrome(Panel* p, float x, float y, float w, float h) {
    // Determine corner radii based on panel side (TL, TR, BR, BL)
    vec4 radii = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (p->side) {
        case PANEL_BOTTOM: radii[0] = PANEL_RADIUS; radii[1] = PANEL_RADIUS; break;
        case PANEL_TOP:    radii[2] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        case PANEL_LEFT:   radii[1] = PANEL_RADIUS; radii[2] = PANEL_RADIUS; break;
        case PANEL_RIGHT:  radii[0] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        default: break;
    }

    // Fully opaque background (Borders removed entirely)
    exQuad2D((vec2){x, y}, (vec2){w, h}, radii, 0.0f, CT.bg, CT.bg);

    Color bar = CT.bg_alt;
    float bar_h = TITLE_H;

    float tx = x + PAD;
    float ty;

    switch (p->side) {
        case PANEL_TOP:
            // Title bar INVERTED to BOTTOM of panel
            exQuad2D((vec2){x, y}, (vec2){w, bar_h}, (vec4){0.0f, 0.0f, radii[2], radii[3]}, 0.0f, bar, bar);
            ty = y + bar_h  * 0.5f;
            break;
        default:
            // Title bar at TOP of panel
            exQuad2D((vec2){x, y + h - bar_h}, (vec2){w, bar_h}, (vec4){radii[0], radii[1], 0.0f, 0.0f}, 0.0f, bar, bar);
            ty = y + h - bar_h * 0.5f;
            break;
    }

    // Title text
    text(editor.font, p->title, tx, ty, CT.text);
}

//  Content area helper: shifts area depending on titlebar location
//  Content area helper: shifts area depending on titlebar location

static void content_area(Panel* p, float px, float py, float pw, float ph,
                          float* cx, float* cy, float* cw, float* ch) {
    *cx = px + PAD;
    *cw = pw - 2.0f * PAD;
    switch (p->side) {
        case PANEL_BOTTOM:
        case PANEL_LEFT:
        case PANEL_RIGHT:
            *cy = py + PAD;
            *ch = ph - TITLE_H - PAD;
            break;
        case PANEL_TOP:
            *cy = py + TITLE_H + PAD;
            *ch = ph - TITLE_H - PAD;
            break;
        default:
            *cy = py + PAD;
            *ch = ph - 2.0f * PAD;
            break;
    }
}

/// Inspector  (Right panel)

static void draw_field_lock(float cx, float row, const char* label) {
    if (s_icon_lock >= 0) {
        float lw = measure_text_width(editor.font, label, 1.0f);
        Texture2D* tex = texture_pool_get(s_icon_lock);
        if (tex && tex->loaded) {
            // Removed the - 6.0f to move the icon exactly 6 pixels UP
            float icon_y = row - (editor.font->ascent - editor.font->descent) * 0.5f;
            texture2D((vec2){cx + lw + 8.0f, icon_y}, (vec2){14, 14}, tex, CT.text_dim);
        }
    }
}

static bool field_vec3(float cx, float row, float col2_x, float cw,
                       const char* label, float* v, const char* unit, float speed, bool locked) {
    if (!editor.font) return false;
    bool changed = false;
    text(editor.font, label, cx, row, CT.text_dim);
    if (locked) draw_field_lock(cx, row, label);

    float box_x = col2_x;
    float box_w = (cx + cw) - box_x;
    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    float sec_w = box_w / 3.0f;
    Color axis_colors[3] = {CT.x_dark, CT.y_dark, CT.z_dark};
    Color axis_colors_active[3] = {CT.x, CT.y, CT.z};
    const char* axis_labels[3] = {"X", "Y", "Z"};

    for (int i = 0; i < 3; i++) {
        float hit_x = box_x + i * sec_w;
        bool hovered = (s_ui_mx >= hit_x && s_ui_mx <= hit_x + sec_w && s_ui_my >= box_y && s_ui_my <= box_y + box_h);
        void* id = (void*)&v[i];

        if (!locked && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
            s_ui_active_id = id;
            s_ui_drag_start_x = (float)s_ui_mx;
        }

        if (!locked && s_ui_active_id == id) {
            float delta = ((float)s_ui_mx - s_ui_drag_start_x) * speed;
            GLFWwindow* win = glfwGetCurrentContext();
            if (win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)) {
                delta *= 0.1f;
            }
            v[i] += delta;
            s_ui_drag_start_x = (float)s_ui_mx;
            changed = true;
        }

        Color label_col = (!locked && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? axis_colors_active[i] : axis_colors[i];

        float px = hit_x + 6.0f;
        text(editor.font, axis_labels[i], px, row, label_col);

        char num[32];
        snprintf(num, sizeof(num), "% .2f", v[i]);
        float num_x = px + 14.0f;
        text(editor.font, num, num_x, row, CT.text);

        if (unit) {
            float unit_x = num_x + 58.0f;
            text(editor.font, unit, unit_x, row, CT.text_dim);
        }
    }
    return changed;
}

static bool field_vec4_color(float cx, float row, float col2_x, float cw,
                             const char* label, float* c, bool locked) {
    if (!editor.font) return false;
    bool changed = false;
    text(editor.font, label, cx, row, CT.text_dim);
    if (locked) draw_field_lock(cx, row, label);

    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    float swatch_size = box_h - 4.0f;
    float swatch_x = col2_x - 25.0f;
    Color actual_color = {c[0], c[1], c[2], c[3]};
    exQuad2D((vec2){swatch_x, box_y + 2.0f}, (vec2){swatch_size, swatch_size}, (vec4){3.0f, 3.0f, 3.0f, 3.0f}, 0.0f, actual_color, actual_color);

    bool swatch_hovered = (s_ui_mx >= swatch_x && s_ui_mx <= swatch_x + swatch_size &&
                           s_ui_my >= box_y + 2.0f && s_ui_my <= box_y + 2.0f + swatch_size);

    if (!locked && swatch_hovered && s_ui_mclicked) {
        float sh = (float)context.swapChainExtent.height;
        float anchor_x = swatch_x - 10.0f; // Open to the left of the swatch to keep it on-screen
        float anchor_y = sh - (box_y + 2.0f + swatch_size * 0.5f);
        colorpicker_open(&s_color_picker, anchor_x, anchor_y, c, c, on_color_picked, NULL);
    }

    float box_x = swatch_x + swatch_size + 8.0f;
    float box_w = (cx + cw) - box_x + 8.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    float sec_w = box_w / 4.0f;
    Color axis_colors[4] = {CT.x_dark, CT.y_dark, CT.z_dark, CT.text_dim};
    Color axis_colors_active[4] = {CT.x, CT.y, CT.z, CT.text};
    const char* axis_labels[4] = {"R", "G", "B", "A"};

    for (int i = 0; i < 4; i++) {
        float hit_x = box_x + i * sec_w;
        bool hovered = (s_ui_mx >= hit_x && s_ui_mx <= hit_x + sec_w && s_ui_my >= box_y && s_ui_my <= box_y + box_h);
        void* id = (void*)&c[i];

        if (!locked && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
            s_ui_active_id = id;
            s_ui_drag_start_x = (float)s_ui_mx;
        }

        if (!locked && s_ui_active_id == id) {
            float delta = ((float)s_ui_mx - s_ui_drag_start_x) * 0.005f;
            GLFWwindow* win = glfwGetCurrentContext();
            if (win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)) {
                delta *= 0.1f;
            }
            c[i] = clampf(c[i] + delta, 0.0f, 1.0f);
            s_ui_drag_start_x = (float)s_ui_mx;
            changed = true;
        }

        Color label_col = (!locked && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? axis_colors_active[i] : axis_colors[i];

        float px = hit_x + 8.0f;
        text(editor.font, axis_labels[i], px, row, label_col);

        char num[32];
        snprintf(num, sizeof(num), "%.2f", c[i]);
        text(editor.font, num, px + 18.0f, row, CT.text);
    }
    return changed;
}

static bool field_float(float cx, float row, float col2_x, float cw,
                        const char* label, float* val, float speed,
                        bool clamp_val, float min_v, float max_v, bool locked) {
    if (!editor.font) return false;
    bool changed = false;

    bool hovered = (s_ui_mx >= cx && s_ui_mx <= cx + cw &&
                    s_ui_my >= row - editor.font->descent - 3.0f &&
                    s_ui_my <= row + editor.font->ascent + 3.0f);
    void* id = (void*)val;

    if (!locked && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
        s_ui_active_id = id;
        s_ui_drag_start_x = (float)s_ui_mx;
    }

    if (!locked && s_ui_active_id == id) {
        float delta = ((float)s_ui_mx - s_ui_drag_start_x) * speed;
        GLFWwindow* win = glfwGetCurrentContext();
        if (win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)) {
            delta *= 0.1f;
        }
        *val += delta;
        if (clamp_val) {
            *val = clampf(*val, min_v, max_v);
        }
        s_ui_drag_start_x = (float)s_ui_mx;
        changed = true;
    }

    Color label_col = (!locked && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? CT.text : CT.text_dim;
    text(editor.font, label, cx, row, label_col);
    if (locked) draw_field_lock(cx, row, label);

    char num[32];
    snprintf(num, sizeof(num), "%.3f", *val);
    float text_w = measure_text_width(editor.font, num, 1.0f);

    float box_x = col2_x;
    float box_w = text_w + 16.0f; // Exactly 8px padding on left and right!
    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    text(editor.font, num, box_x + 8.0f, row, CT.text);
    return changed;
}

static void field_text(float cx, float row, float col2_x, float cw,
                       const char* label, const char* value, bool locked) {
    (void)cw;
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);
    if (locked) draw_field_lock(cx, row, label);

    // Read-only text fields (like Geometry) no longer have heavy backgrounds!
    text(editor.font, value, col2_x + 8.0f, row, CT.text);
}

static void render_image_inspector_content(float cx, float cy, float cw, float ch) {
    ImageViewerState* iv = &s_image_viewer;
    Texture2D* tex = texture_pool_get(iv->tex_idx);
    if (!tex) return;

    float lh  = editor.font->ascent - editor.font->descent + LH_EXTRA;
    float space_w = font_width(editor.font);

    // Align dynamically to the longest label!
    float col2 = cx + strlen("Formatted Mem") * space_w + space_w * 2.0f;
    float row = cy + ch - PAD;

#define SECTION(label) do { \
    text(editor.font, label, cx, row, CT.accent); \
    row -= lh + 6.0f; \
} while(0)

    // ── File Info ──────────────────────────────────────────────────────────
    SECTION("File Info");
    field_text(cx, row, col2, cw, "Name", iv->filename, false);
    row -= lh + 6.0f;

    const char* ext = strrchr(iv->filename, '.');
    field_text(cx, row, col2, cw, "Type", ext ? ext : "Unknown", false);
    row -= lh + 6.0f;

    char size_str[64];
    if (iv->file_size < 1024) snprintf(size_str, sizeof(size_str), "%zu Bytes", iv->file_size);
    else if (iv->file_size < 1024 * 1024) snprintf(size_str, sizeof(size_str), "%.2f KB", iv->file_size / 1024.0f);
    else snprintf(size_str, sizeof(size_str), "%.2f MB", iv->file_size / (1024.0f * 1024.0f));
    field_text(cx, row, col2, cw, "Size on Disk", size_str, false);
    row -= lh + 12.0f;

    // ── Dimensions ─────────────────────────────────────────────────────────
    SECTION("Dimensions");
    char buf[64];
    snprintf(buf, sizeof(buf), "%d px", tex->width);
    field_text(cx, row, col2, cw, "Width", buf, false);
    row -= lh + 6.0f;

    snprintf(buf, sizeof(buf), "%d px", tex->height);
    field_text(cx, row, col2, cw, "Height", buf, false);
    row -= lh + 6.0f;

    float aspect = (float)tex->width / (float)tex->height;
    snprintf(buf, sizeof(buf), "%.3f", aspect);
    field_text(cx, row, col2, cw, "Aspect Ratio", buf, false);
    row -= lh + 12.0f;

    // ── Texture Data ───────────────────────────────────────────────────────
    SECTION("Texture Data");

    // Exact mathematical VRAM usage for an uncompressed RGBA8 texture
    size_t mem_size = (size_t)tex->width * (size_t)tex->height * 4;
    snprintf(buf, sizeof(buf), "%zu Bytes", mem_size);
    field_text(cx, row, col2, cw, "Memory Size", buf, false);
    row -= lh + 6.0f;

    if (mem_size < 1024) snprintf(size_str, sizeof(size_str), "%zu B", mem_size);
    else if (mem_size < 1024 * 1024) snprintf(size_str, sizeof(size_str), "%.2f KB", mem_size / 1024.0f);
    else snprintf(size_str, sizeof(size_str), "%.2f MB", mem_size / (1024.0f * 1024.0f));

    field_text(cx, row, col2, cw, "Formatted Mem", size_str, false);
    row -= lh + 6.0f;

    field_text(cx, row, col2, cw, "Internal Format", "RGBA8 Unorm", false);
    row -= lh + 6.0f;
    field_text(cx, row, col2, cw, "Color Space", "sRGB", false);
    row -= lh + 6.0f;
    field_text(cx, row, col2, cw, "Bit Depth", "8-bit", false);
    row -= lh + 6.0f;
    field_text(cx, row, col2, cw, "Has Alpha", "Yes", false);
    row -= lh + 6.0f;
    field_text(cx, row, col2, cw, "Alpha Mode", "Straight", false);
    row -= lh + 12.0f;

#undef SECTION
}

static void render_inspector_content(float cx, float cy, float cw, float ch) {
    // Smart Routing: If the Image Viewer is open AND not actively closing, it steals the focus!
    if (s_image_viewer.visible && !s_image_viewer.window.closing) {
        render_image_inspector_content(cx, cy, cw, ch);
        return;
    }

    InspectorState* s = &editor.inspector;

    if (s->selected_mesh_index < 0 ||
        s->selected_mesh_index >= (int)scene.meshes.count) {
        text(editor.font, "Nothing selected", cx, cy + ch - PAD, CT.text_dim);
        return;
    }

    Mesh* m = &scene.meshes.items[s->selected_mesh_index];
    float lh  = editor.font->ascent - editor.font->descent + LH_EXTRA;
    float col2 = cx + 120.0f; // Vector fields stay perfectly aligned to 120

    float space_w = font_width(editor.font);

    // Calculate dynamic alignments based on the longest label per section + exactly 1 space of padding!
    float col2_mat  = cx + strlen("Emissive Strength") * space_w + space_w;
    float col2_geo  = cx + strlen("Alpha Mode") * space_w + space_w;
    float col2_anim = cx + strlen("Joints") * space_w + space_w;

    float row = cy + ch - PAD;

#define SECTION(label) do { \
    text(editor.font, label, cx, row, CT.accent); \
    row -= lh + 6.0f; \
} while(0)

    // ── Transform ──────────────────────────────────────────────────────────
    SECTION("Transform");

    vec3 pos = {m->model[3][0], m->model[3][1], m->model[3][2]};
    vec3 euler;
    glm_euler_angles(m->model, euler);
    vec3 rot_deg = {glm_deg(euler[0]), glm_deg(euler[1]), glm_deg(euler[2])};
    vec3 scale = {
        glm_vec3_norm((vec3){m->model[0][0], m->model[1][0], m->model[2][0]}),
        glm_vec3_norm((vec3){m->model[0][1], m->model[1][1], m->model[2][1]}),
        glm_vec3_norm((vec3){m->model[0][2], m->model[1][2], m->model[2][2]})
    };

    bool transform_changed = false;
    transform_changed |= field_vec3(cx, row, col2, cw, "Position", pos, "m", 0.05f, false);
    row -= lh + 6.0f;
    transform_changed |= field_vec3(cx, row, col2, cw, "Rotation", rot_deg, "°", 0.5f, false);
    row -= lh + 6.0f;
    transform_changed |= field_vec3(cx, row, col2, cw, "Scale", scale, "m", 0.02f, false);
    row -= lh + 12.0f;

    if (transform_changed) {
        glm_mat4_identity(m->model);
        glm_translate(m->model, pos);
        // Reapply rotation: Z, Y, X to maintain standard euler composition
        glm_rotate_z(m->model, glm_rad(rot_deg[2]), m->model);
        glm_rotate_y(m->model, glm_rad(rot_deg[1]), m->model);
        glm_rotate_x(m->model, glm_rad(rot_deg[0]), m->model);
        glm_scale(m->model, scale);
        markMeshesSSBODirty(&context);
    }

    // ── Material ───────────────────────────────────────────────────────────
    SECTION("Material");

    bool mat_changed = false;
    mat_changed |= field_vec4_color(cx, row, col2, cw, "Color", m->baseColorFactor, false);
    row -= lh + 6.0f;

    vec4 emissive = {m->emissiveFactor[0], m->emissiveFactor[1], m->emissiveFactor[2], 1.0f};
    if (field_vec4_color(cx, row, col2, cw, "Emissive", emissive, false)) {
        m->emissiveFactor[0] = emissive[0];
        m->emissiveFactor[1] = emissive[1];
        m->emissiveFactor[2] = emissive[2];
        mat_changed = true;
    }
    row -= lh + 6.0f;

    mat_changed |= field_float(cx, row, col2_mat, cw, "Emissive Strength", &m->emissiveStrength, 0.1f, true, 0.0f, 1e6f, false);
    row -= lh + 6.0f;

    mat_changed |= field_float(cx, row, col2_mat, cw, "Metallic", &m->metallicFactor, 0.01f, true, 0.0f, 1.0f, false);
    row -= lh + 6.0f;

    mat_changed |= field_float(cx, row, col2_mat, cw, "Roughness", &m->roughnessFactor, 0.01f, true, 0.0f, 1.0f, false);
    row -= lh + 6.0f;

    mat_changed |= field_float(cx, row, col2_mat, cw, "Transmission", &m->transmissionFactor, 0.01f, true, 0.0f, 1.0f, false);
    row -= lh + 6.0f;

    mat_changed |= field_float(cx, row, col2_mat, cw, "IOR", &m->ior, 0.01f, true, 1.0f, 3.0f, false);
    row -= lh + 12.0f;

    if (mat_changed) {
        markMeshesSSBODirty(&context);
    }

    // ── Geometry ───────────────────────────────────────────────────────────
    SECTION("Geometry");

    char buf[64];
    snprintf(buf, sizeof buf, "%u", m->vertexCount);
    field_text(cx, row, col2_geo, cw, "Vertices", buf, false);
    row -= lh + 6.0f;

    snprintf(buf, sizeof buf, "%u", m->indexCount);
    field_text(cx, row, col2_geo, cw, "Indices", buf, false);
    row -= lh + 6.0f;

    const char* amode = (m->alpha_mode == 0) ? "Opaque" : (m->alpha_mode == 1) ? "Mask" : "Blend";
    field_text(cx, row, col2_geo, cw, "Alpha Mode", amode, false);
    row -= lh + 6.0f;

    field_text(cx, row, col2_geo, cw, "Unlit", m->is_unlit ? "Yes" : "No", false);
    row -= lh + 6.0f;

    if (m->name) {
        field_text(cx, row, col2_geo, cw, "Name", m->name, false);
        row -= lh + 6.0f;
    }

    // ── Bounds ─────────────────────────────────────────────────────────────
    SECTION("Bounds");
    vec3 bmin = {m->aabbMin[0], m->aabbMin[1], m->aabbMin[2]};
    vec3 bmax = {m->aabbMax[0], m->aabbMax[1], m->aabbMax[2]};
    field_vec3(cx, row, col2, cw, "AABB Min", bmin, "m", 0.0f, true);
    row -= lh + 6.0f;
    field_vec3(cx, row, col2, cw, "AABB Max", bmax, "m", 0.0f, true);
    row -= lh + 12.0f;

    // ── Animation ──────────────────────────────────────────────────────────
    if (m->jointCount > 0 || m->morphCount > 0) {
        SECTION("Animation");

        if (m->jointCount > 0) {
            snprintf(buf, sizeof buf, "%d (off %d)", m->jointCount, m->jointOffset);
            field_text(cx, row, col2_anim, cw, "Joints", buf, true);
            row -= lh + 6.0f;
        }
        if (m->morphCount > 0) {
            snprintf(buf, sizeof buf, "%d", m->morphCount);
            field_text(cx, row, col2_anim, cw, "Morphs", buf, true);
            row -= lh + 6.0f;

            if (m->morph_data) {
                for (int i = 0; i < m->morphCount && i < 8; i++) {
                    char wlbl[16];
                    snprintf(wlbl, sizeof wlbl, "  [%d]", i);
                    if (field_float(cx, row, col2_anim, cw, wlbl, &m->morph_data->weights[i], 0.01f, true, 0.0f, 1.0f, false)) {
                        markMeshesSSBODirty(&context);
                    }
                    row -= lh + 6.0f;
                }
            }
        }
    }

#undef SECTION
}

void file_manager_refresh(void);

static void render_filesystem_content(float cx, float cy, float cw, float ch) {
    FileManagerState* s = &editor.file_manager;

    // Solid background to hide the inspector underneath when dragged up
    exQuad2D((vec2){cx - PAD, cy}, (vec2){cw + PAD*2.0f, ch}, (vec4){0,0,0,0}, 0.0f, CT.bg, CT.bg);

    float tab_h = TITLE_H;
    float tab_y = cy + ch - tab_h;

    // --- DRAW LIST FIRST (So the title bar perfectly overlaps scrolled items!) ---
    if (!editor.fs_collapsed || editor.fs_anim_t < 1.0f) {
        // Apply smooth scrolling offset!
        float list_y = tab_y - PAD + s_fs_scroll_y;
        float lh = editor.font->ascent - editor.font->descent + LH_EXTRA + 6.0f;

        // Track the target item at each depth for the selected path
        int selected_ancestors[32];
        for (int i = 0; i < 32; i++) selected_ancestors[i] = -1;

        if (s->selected_index >= 0 && s->selected_index < s->item_count) {
            int trace_idx = s->selected_index;
            while (trace_idx >= 0) {
                int d = s->items[trace_idx].depth;
                if (d < 32) selected_ancestors[d] = trace_idx;

                if (d == 0) break;
                int parent = -1;
                for (int p = trace_idx - 1; p >= 0; p--) {
                    if (s->items[p].depth < d) { parent = p; break; }
                }
                trace_idx = parent;
            }
        }

        // Track the ancestors of the current item being drawn
        int current_ancestors[32];
        for (int i = 0; i < 32; i++) current_ancestors[i] = -1;

        for (int i = 0; i < s->item_count; i++) {
            FileItem* item = &s->items[i];
            if (list_y < cy) break; // Hard clip at bottom

            int D = item->depth;
            if (D < 32) current_ancestors[D] = i;

            float indent = D * 16.0f;
            float row_x = cx + indent;

            // Hover & Selection
            bool hovered = (s_ui_mx >= cx && s_ui_mx <= cx + cw && s_ui_my >= list_y - lh && s_ui_my <= list_y);
            bool selected = (s->selected_index == i);

            float sel_x = cx + D * 16.0f + 14.0f;
            float sel_w = cw - (sel_x - cx) + PAD;

            bool is_visible = (list_y <= tab_y + 4.0f); // Prevents drawing above the title bar!

            if (is_visible) {
                if (selected) {
                    exQuad2D((vec2){sel_x, list_y - lh}, (vec2){sel_w, lh}, (vec4){4,4,4,4}, 0.0f, CT.fs_hovered, CT.fs_hovered);
                } else if (hovered) {
                    exQuad2D((vec2){sel_x, list_y - lh}, (vec2){sel_w, lh}, (vec4){4,4,4,4}, 0.0f, CT.fs_selected, CT.fs_selected);
                }
            }

            // Input
            if (hovered && s_ui_mclicked && is_visible) {
                s->selected_index = i;
                if (item->type == FILE_ITEM_DIR && s_ui_mx < row_x + 18.0f) {
                    if (item->expanded) {
                        for (int e = 0; e < s->expanded_count; e++) {
                            if (strcmp(s->expanded_paths[e], item->full_path) == 0) {
                                s->expanded_paths[e][0] = '\0';
                                strcpy(s->expanded_paths[e], s->expanded_paths[s->expanded_count - 1]);
                                s->expanded_count--;
                                break;
                            }
                        }
                    } else {
                        if (s->expanded_count < 64) {
                            strcpy(s->expanded_paths[s->expanded_count++], item->full_path);
                        }
                    }
                    file_manager_refresh();
                    return; // Tree rebuilt, stop rendering this frame
                } else {
                    double now = glfwGetTime();
                    if (s_last_clicked_item == i && (now - s_last_item_click_time) < 0.3) {
                        // It's a double click!
                        if (item->type == FILE_ITEM_FILE) {
                            const char* ext = strrchr(item->name, '.');
                            if (ext) {
                                if (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
                                    strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".bmp") == 0 ||
                                    strcasecmp(ext, ".tga") == 0 || strcasecmp(ext, ".hdr") == 0 ||
                                    strcasecmp(ext, ".exr") == 0) {

                                    float sh = (float)context.swapChainExtent.height;
                                    image_viewer_open(&s_image_viewer, item, s_ui_mx, sh - s_ui_my);
                                } else {
                                    message(MSG_WARNING, "File format not yet supported!");
                                }
                            }
                        }
                        s_last_clicked_item = -1;
                    } else {
                        s_last_clicked_item = i;
                        s_last_item_click_time = now;
                    }
                }
            }

            // Calculate Animating Target Bound for the continuous beam overlay
            float anim_y = -9999.0f;
            if (s_is_searching && s_search_match_count > 0 && s_search_current_idx >= 0) {
                int active_match = s_search_matches[s_search_current_idx];
                float current_beam_index = s_search_prev_match_idx + (active_match - s_search_prev_match_idx) * ease_cubic_out(clampf(s_search_anim_t, 0.0f, 1.0f));

                // Shift down by exactly half a line height so the beam geometrically intersects the spur!
                anim_y = tab_y - PAD + s_fs_scroll_y - (current_beam_index * lh) - (lh * 0.5f);
            }

            // Draw Godot-style Tree Lines
            for (int d = 0; d < D; d++) {
                float line_x = cx + d * 16.0f + 8.0f;

                bool parent_continues = false;
                for (int j = i + 1; j < s->item_count; j++) {
                    if (s->items[j].depth <= d) break; // Parent closed
                    if (s->items[j].depth == d + 1) { parent_continues = true; break; }
                }

                Color upper_col = CT.fs_tree_dimmed;
                Color lower_col = CT.fs_tree_dimmed;
                Color spur_col  = CT.fs_tree_dimmed;

                bool in_selected_folder = (current_ancestors[d] == selected_ancestors[d]);
                int target = selected_ancestors[d + 1];

                if (in_selected_folder && target != -1 && !s_is_searching) {
                    bool is_final_folder = (target == s->selected_index);

                    if (i < target) {
                        upper_col = CT.fs_tree;
                        lower_col = CT.fs_tree;
                        if (is_final_folder) spur_col = (Color){0, 0, 0, 0};
                    } else if (i == target) {
                        upper_col = CT.fs_tree;
                        spur_col  = CT.fs_tree;
                        if (is_final_folder) lower_col = (Color){0, 0, 0, 0};
                    } else if (i > target) {
                        if (is_final_folder) {
                            upper_col = (Color){0, 0, 0, 0};
                            lower_col = (Color){0, 0, 0, 0};
                            spur_col  = (Color){0, 0, 0, 0};
                        }
                    }
                }

                if (is_visible) {
                    if (d == D - 1) {
                        if (spur_col.a > 0.0f)  quad2D((vec2){line_x, list_y - lh * 0.5f}, (vec2){10.0f, 1.0f}, spur_col);
                        if (upper_col.a > 0.0f) quad2D((vec2){line_x, list_y - lh * 0.5f}, (vec2){1.0f, lh * 0.5f}, upper_col);
                        if (parent_continues && lower_col.a > 0.0f) {
                            quad2D((vec2){line_x, list_y - lh}, (vec2){1.0f, lh * 0.5f}, lower_col);
                        }
                    } else {
                        if (parent_continues) {
                            if (upper_col.a > 0.0f) quad2D((vec2){line_x, list_y - lh * 0.5f}, (vec2){1.0f, lh * 0.5f}, upper_col);
                            if (lower_col.a > 0.0f) quad2D((vec2){line_x, list_y - lh}, (vec2){1.0f, lh * 0.5f}, lower_col);
                        }
                    }

                    // OVERLAY ACTIVE SEARCH BEAM
                    if (s_is_searching && in_selected_folder && target != -1) {
                        float y_top = list_y;
                        float y_mid = list_y - lh * 0.5f;
                        float y_bot = list_y - lh;

                        if (d == D - 1) {
                            if (i < target) {
                                float cy_bot = fmaxf(y_mid, anim_y);
                                if (y_top > cy_bot) quad2D((vec2){line_x, cy_bot}, (vec2){1.0f, y_top - cy_bot}, CT.accent);
                                if (parent_continues) {
                                    float c_bot = fmaxf(y_bot, anim_y);
                                    if (y_mid > c_bot) quad2D((vec2){line_x, c_bot}, (vec2){1.0f, y_mid - c_bot}, CT.accent);
                                }
                            } else if (i == target) {
                                float cy_bot = fmaxf(y_mid, anim_y);
                                if (y_top > cy_bot) quad2D((vec2){line_x, cy_bot}, (vec2){1.0f, y_top - cy_bot}, CT.accent);
                                if (y_mid >= anim_y) {
                                    quad2D((vec2){line_x, y_mid}, (vec2){10.0f, 1.0f}, CT.accent);
                                }
                            }
                        } else {
                            if (parent_continues && i < target) {
                                float cy_bot_u = fmaxf(y_mid, anim_y);
                                if (y_top > cy_bot_u) quad2D((vec2){line_x, cy_bot_u}, (vec2){1.0f, y_top - cy_bot_u}, CT.accent);

                                float cy_bot_l = fmaxf(y_bot, anim_y);
                                if (y_mid > cy_bot_l) quad2D((vec2){line_x, cy_bot_l}, (vec2){1.0f, y_mid - cy_bot_l}, CT.accent);
                            }
                        }
                    }
                }
            }

            if (is_visible) {
                // Draw Icons
                float icon_y = list_y - lh * 0.5f - 8.0f;
                float current_x = row_x;

                if (item->type == FILE_ITEM_DIR) {
                    Texture2D* arrow = texture_pool_get(item->expanded ? s->icon_arrow_down : s->icon_arrow_right);
                    if (arrow) texture2D((vec2){current_x, icon_y}, (vec2){16, 16}, arrow, (Color){1.0f,1.0f,1.0f,1.0f});
                    current_x += 18.0f;

                    Texture2D* folder = texture_pool_get(s->icon_folder);
                    if (folder) texture2D((vec2){current_x, icon_y}, (vec2){16, 16}, folder, CT.accent);
                    current_x += 20.0f;
                } else {
                    current_x += 18.0f;
                    Texture2D* file_icon = texture_pool_get(s->icon_file);
                    if (file_icon) texture2D((vec2){current_x, icon_y}, (vec2){16, 16}, file_icon, (Color){1.0f,1.0f,1.0f,1.0f});
                    current_x += 20.0f;
                }

                // Text
                text(editor.font, item->name, current_x, list_y - lh * 0.5f - 2.0f, selected ? CT.text : CT.text_dim);
            }

            list_y -= lh;
        }
    } // End of List Draw

    // --- DRAW TITLE BAR ON TOP ---
    exQuad2D((vec2){cx - PAD, tab_y}, (vec2){cw + PAD*2.0f, tab_h}, (vec4){8.0f, 8.0f, 0.0f, 0.0f}, 0.0f, CT.bg_alt, CT.bg_alt);
    text(editor.font, "FileSystem", cx, tab_y + tab_h * 0.5f - 2.0f, CT.text);

    if (s_is_searching) {
        char counter_str[32] = "0/0";
        if (s_search_match_count > 0) {
            snprintf(counter_str, sizeof(counter_str), "%d/%d", s_search_current_idx + 1, s_search_match_count);
        }
        float counter_text_w = measure_text_width(editor.font, counter_str, 1.0f);
        float mc_w = counter_text_w + 16.0f;

        float sb_x = cx + measure_text_width(editor.font, "FileSystem", 1.0f) + 20.0f;
        float sb_w = cw - (sb_x - cx) - PAD - mc_w - 8.0f;

        // Perfect Vertical Alignment of Input Box & Text
        float sb_h = editor.font->ascent - editor.font->descent + 8.0f;
        float sb_y = tab_y + (tab_h - sb_h) * 0.5f;
        exQuad2D((vec2){sb_x, sb_y}, (vec2){sb_w, sb_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

        // Render Emacs Block Cursor and Text in a single loop
        float cursor_w = font_width(editor.font);
        float text_h = editor.font->ascent;
        float cursor_h = sb_h;

        // Adjusted baseline to bring text up inside the field properly
        float ty = sb_y + (editor.font->descent * 2);

        double time_since_key = glfwGetTime() - s_search_last_key_time;
        bool cursor_visible = (time_since_key < 0.5) || (fmod(time_since_key, 1.0) < 0.5);

        float cur_draw_x = sb_x + 8.0f;
        int len = strlen(s_search_query);

        for (int i = 0; i <= len; i++) {
            bool is_cursor_pos = (i == s_search_cursor);
            char c = (i < len) ? s_search_query[i] : '\0';

            if (is_cursor_pos && cursor_visible) {
                // Draw rectangular block cursor exactly the width of a whitespace
                exQuad2D((vec2){cur_draw_x, sb_y}, (vec2){cursor_w, cursor_h}, (vec4){2.0f,2.0f,2.0f,2.0f}, 0.0f, CT.accent, CT.accent);
            }

            if (c != '\0') {
                Color col = (is_cursor_pos && cursor_visible) ? CT.bg_deep : CT.accent;
                float adv = character(editor.font, (uint32_t)c, cur_draw_x, ty, col);
                cur_draw_x += adv > 0.0f ? adv : cursor_w;
            } else if (is_cursor_pos) {
                break;
            }
        }

        // Draw the Match Counter Badge
        float mc_h = tab_h - 8.0f;
        float mc_x = sb_x + sb_w + 8.0f;
        float mc_y = tab_y + 4.0f;
        exQuad2D((vec2){mc_x, mc_y}, (vec2){mc_w, mc_h}, (vec4){6.0f, 6.0f, 6.0f, 6.0f}, 0.0f, CT.accent, CT.accent);
        text(editor.font, counter_str, mc_x + 8.0f, mc_y + mc_h * 0.5f - 2.0f, CT.bg);
    }
}

static void render_right_panel(Panel* panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    // split_y represents the exact pixel boundary between the two views
    float split_y = cy + ch * (1.0f - editor.inspector_fs_split);

    // Splitter Interaction (Mapped directly to the FileSystem titlebar)
    bool split_hovered = (s_ui_mx >= cx - PAD && s_ui_mx <= cx + cw + PAD &&
                          s_ui_my >= split_y - TITLE_H && s_ui_my <= split_y);

    if (split_hovered && s_ui_mclicked && s_ui_active_id == NULL) {
        double now = glfwGetTime();
        if (editor.fs_collapsed) {
            // Single click to expand
            editor.fs_collapsed = false;
            editor.fs_split_start = editor.inspector_fs_split;
            editor.fs_split_target = editor.fs_split_saved > 0.0f ? editor.fs_split_saved : 0.5f;
            editor.fs_anim_t = 0.0f;
        } else if (now - editor.last_tab_click_time < 0.3) {
            // Double click to collapse
            editor.fs_collapsed = true;
            editor.fs_split_saved = editor.inspector_fs_split;
            editor.fs_split_start = editor.inspector_fs_split;
            // Target split pushes the tab to the very bottom
            editor.fs_split_target = 1.0f - (TITLE_H / ch);
            editor.fs_anim_t = 0.0f;
        } else {
            // Start drag
            s_ui_active_id = &editor.inspector_fs_split;
            s_ui_drag_start_val = editor.inspector_fs_split;
            s_ui_drag_start_y = (float)s_ui_my;
        }
        editor.last_tab_click_time = now;
    }

    if (s_ui_active_id == &editor.inspector_fs_split) {
        float delta = ((float)s_ui_my - s_ui_drag_start_y) / ch;
        // Limit drag so tab doesn't go below the screen
        float max_split = 1.0f - (TITLE_H / ch);
        editor.inspector_fs_split = clampf(s_ui_drag_start_val - delta, 0.1f, max_split);
        editor.fs_split_target = editor.inspector_fs_split;
        editor.fs_anim_t = 1.0f;
        editor.fs_collapsed = false;
        split_y = cy + ch * (1.0f - editor.inspector_fs_split);
    }

    // Render Inspector (Top Half)
    float insp_ch = (cy + ch) - split_y;
    if (insp_ch > 20.0f) {
        render_inspector_content(cx, split_y, cw, insp_ch);
    }

    // Render FileSystem (Bottom Half)
    float fs_ch = split_y - cy;
    if (fs_ch > 20.0f) {
        render_filesystem_content(cx, cy, cw, fs_ch);
    }
}

///  Hierarchy  (Left panel)

static void render_hierarchy(Panel* panel,
                              float px, float py, float pw, float ph) {
    if (!editor.font) return;

    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    HierarchyState* s = &editor.hierarchy;
    int count = (int)scene.meshes.count;

    if (count == 0) {
        text(editor.font, "Scene is empty", cx, cy + ch - PAD, CT.text_dim);
        return;
    }

    float lh   = editor.font->ascent - editor.font->descent + LH_EXTRA;
    // List items from top down (each item decreases y)
    float row  = cy + ch - PAD;

    for (int i = 0; i < count; i++) {
        if (row < cy) break;

        Mesh* m        = &scene.meshes.items[i];
        bool selected  = (i == s->selected_index);

        if (selected) {
            // Highlight bar
            float bar_y = row - editor.font->descent - 2.0f;
            exQuad2D((vec2){cx - 6.0f, bar_y}, (vec2){cw + 12.0f, lh + 2.0f}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_alt, CT.bg_alt);
            // Accent left stripe
            quad2D((vec2){cx - 6.0f, bar_y}, (vec2){3.0f, lh + 2.0f}, CT.accent);
        }

        // Alpha-mode dot (3×3 square)
        Color dot;
        if      (m->alpha_mode == 0) dot = CT.success;
        else if (m->alpha_mode == 1) dot = CT.error;
        else                         dot = CT.warning;

        float dot_y = row + (editor.font->ascent - editor.font->descent) * 0.5f - 3.0f;
        quad2D((vec2){cx + 2.0f, dot_y}, (vec2){6.0f, 6.0f}, dot);

        Color tc = selected ? CT.text : CT.text_dim;
        const char* label = m->name ? m->name : "(unnamed)";
        text(editor.font, label, cx + 14.0f, row, tc);

        row -= lh + 2.0f;
    }
}

///  File Manager  (Bottom panel)

#define FM_TILE_W    110.0f
#define FM_TILE_H     52.0f
#define FM_TILE_GAP    8.0f

static void render_file_manager(Panel* panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);
    text(editor.font, "— bottom panel coming soon —", cx, cy + ch - PAD, CT.text_dim);
}

///  Console

static void render_placeholder(Panel* panel,
                                float px, float py, float pw, float ph) {
    if (!editor.font) return;

    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    text(editor.font, "— console coming soon —", cx, cy + ch - PAD, CT.text_dim);
}

/// File Manager

// Native POSIX case-insensitive sort, prioritizing folders
static int compare_file_items(const void* a, const void* b) {
    const FileItem* fa = (const FileItem*)a;
    const FileItem* fb = (const FileItem*)b;
    if (fa->type != fb->type) return fa->type == FILE_ITEM_DIR ? -1 : 1;
    return strcasecmp(fa->name, fb->name);
}

// Deep recursive scanner matching the Godot Tree topology
static void scan_dir_recursive(FileManagerState* s, const char* path, int depth) {
    DIR* dir = opendir(path);
    if (!dir) return;

    FileItem local_items[256];
    int local_count = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && local_count < 256) {
        if (entry->d_name[0] == '.') continue; // skip hidden

        FileItem* item = &local_items[local_count++];
        strncpy(item->name, entry->d_name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
        snprintf(item->full_path, sizeof(item->full_path), "%s/%s", path, entry->d_name);
        item->depth = depth;

        struct stat st;
        if (stat(item->full_path, &st) == 0) {
            item->type = S_ISDIR(st.st_mode) ? FILE_ITEM_DIR : FILE_ITEM_FILE;
            item->size_bytes = (size_t)st.st_size;
        } else {
            item->type = FILE_ITEM_FILE;
            item->size_bytes = 0;
        }

        item->expanded = false;
        if (item->type == FILE_ITEM_DIR) {
            for (int i = 0; i < s->expanded_count; i++) {
                if (strcmp(s->expanded_paths[i], item->full_path) == 0) {
                    item->expanded = true;
                    break;
                }
            }
        }
    }
    closedir(dir);

    if (local_count > 1) {
        qsort(local_items, local_count, sizeof(FileItem), compare_file_items);
    }

    // Append sorted items to master list, and immediately recurse if expanded!
    for (int i = 0; i < local_count; i++) {
        if (s->item_count >= FILE_MANAGER_MAX_ITEMS) break;
        s->items[s->item_count] = local_items[i];
        s->item_count++;

        if (local_items[i].expanded) {
            scan_dir_recursive(s, local_items[i].full_path, depth + 1);
        }
    }
}

bool file_manager_navigate(const char* path) {
    FileManagerState* s = &editor.file_manager;
    s->item_count = 0;
    strncpy(s->current_path, path, sizeof(s->current_path) - 1);
    s->current_path[sizeof(s->current_path) - 1] = '\0';
    scan_dir_recursive(s, s->current_path, 0);
    return true;
}

void file_manager_refresh(void) {
    FileManagerState* s = &editor.file_manager;
    s->item_count = 0;
    // Default to the project assets folder if no path is set
    if (s->current_path[0] == '\0') {
        strncpy(s->current_path, "./assets", sizeof(s->current_path) - 1);
    }
    scan_dir_recursive(s, s->current_path, 0);
}

/// Keybinding callbacks

static void toggle_bottom(void) { editor_toggle_panel(PANEL_BOTTOM); }
static void toggle_right(void)  { editor_toggle_panel(PANEL_RIGHT);  }
static void toggle_top(void)    { editor_toggle_panel(PANEL_TOP);    }
static void toggle_left(void)   { editor_toggle_panel(PANEL_LEFT);   }

// Fast, portable case-insensitive substring search
static bool str_contains_ci(const char* haystack, const char* needle) {
    if (!*needle) return true;
    for (const char* h = haystack; *h; h++) {
        char hc = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
        char nc = (*needle >= 'A' && *needle <= 'Z') ? *needle + 32 : *needle;
        if (hc == nc) {
            const char* h1 = h + 1;
            const char* n1 = needle + 1;
            while (*n1) {
                char h1c = (*h1 >= 'A' && *h1 <= 'Z') ? *h1 + 32 : *h1;
                char n1c = (*n1 >= 'A' && *n1 <= 'Z') ? *n1 + 32 : *n1;
                if (h1c != n1c) break;
                h1++;
                n1++;
            }
            if (!*n1) return true;
        }
    }
    return false;
}

static void update_search_scroll(void) {
    FileManagerState* s = &editor.file_manager;
    if (s_search_match_count > 0 && s_search_current_idx >= 0) {
        s_search_prev_match_idx = s->selected_index > 0 ? s->selected_index : 0;
        s->selected_index = s_search_matches[s_search_current_idx];
        float lh = editor.font->ascent - editor.font->descent + LH_EXTRA + 6.0f;
        float target_y_pos = s->selected_index * lh;
        float ch = editor.panels[PANEL_BOTTOM].size - 32.0f - 28.0f;

        s_fs_scroll_start = s_fs_scroll_y;
        s_fs_scroll_target = target_y_pos - (ch * 0.5f);
        if (s_fs_scroll_target < 0.0f) s_fs_scroll_target = 0.0f;
        s_fs_scroll_t = 0.0f; // Trigger ease!

        s_search_anim_t = 0.0f; // Restart trace animation
    }
}

void search_next(void) {
    if (!s_is_searching || s_search_match_count == 0) return;
    s_search_current_idx = (s_search_current_idx + 1) % s_search_match_count;
    update_search_scroll();
}

void search_prev(void) {
    if (!s_is_searching || s_search_match_count == 0) return;
    s_search_current_idx = (s_search_current_idx - 1 + s_search_match_count) % s_search_match_count;
    update_search_scroll();
}

void search_execute(void) {
    FileManagerState* s = &editor.file_manager;
    s_search_match_count = 0;
    s_search_current_idx = -1;
    if (strlen(s_search_query) == 0) return;

    FsCache* cache = get_or_build_cache(s->current_path, false);
    char best_matches[256][256];

    for(int i = 0; i < cache->count; i++) {
        const char* filename = strrchr(cache->paths[i], '/');
        filename = filename ? filename + 1 : cache->paths[i];

        if (str_contains_ci(filename, s_search_query)) {
            snprintf(best_matches[s_search_match_count], 256, "%s/%s", s->current_path, cache->paths[i]);
            s_search_match_count++;
            if (s_search_match_count >= 256) break;
        }
    }

    if (s_search_match_count > 0) {
        // Expand parents for ALL matches
        for (int m = 0; m < s_search_match_count; m++) {
            char temp[256];
            strncpy(temp, best_matches[m], sizeof(temp));
            char* last_slash;
            while ((last_slash = strrchr(temp, '/')) != NULL) {
                *last_slash = '\0';
                if (strlen(temp) < strlen(s->current_path)) break;

                bool found = false;
                for(int i = 0; i < s->expanded_count; i++) {
                    if (strcmp(s->expanded_paths[i], temp) == 0) { found = true; break; }
                }
                if (!found && s->expanded_count < 64) {
                    strncpy(s->expanded_paths[s->expanded_count++], temp, 255);
                }
            }
        }
        file_manager_refresh();

        // Map string matches to rendered UI indices
        int mapped_count = 0;
        for (int m = 0; m < s_search_match_count; m++) {
            for(int i = 0; i < s->item_count; i++) {
                if (strcmp(s->items[i].full_path, best_matches[m]) == 0) {
                    s_search_matches[mapped_count++] = i;
                    break;
                }
            }
        }
        s_search_match_count = mapped_count;

if (s_search_match_count > 0) {
            s_search_current_idx = 0;
            update_search_scroll();
        }
    }
}

void editor_search_insert_char(char c) {
    int len = strlen(s_search_query);
    if (len < 63) {
        memmove(&s_search_query[s_search_cursor + 1], &s_search_query[s_search_cursor], len - s_search_cursor + 1);
        s_search_query[s_search_cursor] = c;
        s_search_cursor++;
        search_execute();
        s_search_last_key_time = glfwGetTime();
    }
}

void editor_search_backspace(void) {
    if (s_search_cursor > 0) {
        int len = strlen(s_search_query);
        memmove(&s_search_query[s_search_cursor-1], &s_search_query[s_search_cursor], len - s_search_cursor + 1);
        s_search_cursor--;
        search_execute();
        s_search_last_key_time = glfwGetTime();
    }
}

static void search_start(void) {
    FileManagerState* s = &editor.file_manager;
    if (s_is_searching) return;
    s_is_searching = true;
    s_search_query[0] = '\0';
    s_search_cursor = 0;
    s_search_anim_t = 0.0f;
    s_search_match_count = 0;
    s_search_current_idx = -1;
    s_search_last_key_time = glfwGetTime();
    s_search_prev_match_idx = 0;

    s_saved_expanded_count = s->expanded_count;
    for(int i=0; i<s->expanded_count; i++) strcpy(s_saved_expanded_paths[i], s->expanded_paths[i]);
    s_saved_selected_index = s->selected_index;
    s_saved_scroll_y = s_fs_scroll_y;
    s_saved_scroll_target = s_fs_scroll_target;
    s_fs_scroll_start = s_fs_scroll_y;
    s_fs_scroll_t = 1.0f;

    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = editor_search_keymap;
    editor_search_keymap = temp;
    set_active_text_input(editor_search_insert_char);
}

void search_cancel(void) {
    if (!s_is_searching) return;
    s_is_searching = false;
    FileManagerState* s = &editor.file_manager;
    s->expanded_count = s_saved_expanded_count;
    for(int i=0; i<s->expanded_count; i++) strcpy(s->expanded_paths[i], s_saved_expanded_paths[i]);
    s->selected_index = s_saved_selected_index;
    s_search_match_count = 0;
    s_search_current_idx = -1;

    s_fs_scroll_y = s_saved_scroll_y;
    s_fs_scroll_target = s_saved_scroll_target;
    s_fs_scroll_start = s_saved_scroll_y;
    s_fs_scroll_t = 1.0f; // Instant revert! No easing!

    file_manager_refresh();

    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = editor_search_keymap;
    editor_search_keymap = temp;
    set_active_text_input(NULL);
}

static void search_commit(void) {
    if (!s_is_searching) return;
    s_is_searching = false;
    search_execute(); // final resolve

    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = editor_search_keymap;
    editor_search_keymap = temp;
    set_active_text_input(NULL);
}

static void cb_open_color_picker(void) {
    if (editor.inspector.selected_mesh_index < 0 || editor.inspector.selected_mesh_index >= (int)scene.meshes.count) {
        message(MSG_ERROR, "No mesh selected! Cannot pick color.");
        return;
    }
    Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];
    float sh = (float)context.swapChainExtent.height;
    float raw_y = sh - s_ui_my;
    colorpicker_open(&s_color_picker, s_ui_mx, raw_y, m->baseColorFactor, m->baseColorFactor, on_color_picked, NULL);
}

/// Lifecycle

void editor_init(void) {
    memset(&editor, 0, sizeof(Editor));

    // ── Bottom — File Manager ─────────────────────────────────────────────
    editor.panels[PANEL_BOTTOM] = (Panel){
        .side           = PANEL_BOTTOM,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 280.0f,
        .min_size       = 140.0f,
        .max_size       = 600.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Files",
        .render_content = render_file_manager,
    };

    // ── Right — Inspector ─────────────────────────────────────────────────
    editor.panels[PANEL_RIGHT] = (Panel){
        .side           = PANEL_RIGHT,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 420.0f,
        .min_size       = 200.0f,
        .max_size       = 600.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Inspector",
        .render_content = render_right_panel,
    };

    // ── Top — Console ─────────────────────────────────────────────────────
    editor.panels[PANEL_TOP] = (Panel){
        .side           = PANEL_TOP,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 200.0f,
        .min_size       = 80.0f,
        .max_size       = 400.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Console",
        .render_content = render_placeholder,
    };

    // ── Left — Hierarchy ──────────────────────────────────────────────────
    editor.panels[PANEL_LEFT] = (Panel){
        .side           = PANEL_LEFT,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 260.0f,
        .min_size       = 160.0f,
        .max_size       = 500.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Hierarchy",
        .render_content = render_hierarchy,
    };

    // ── Sub-states ────────────────────────────────────────────────────────
    editor.inspector.selected_mesh_index = -1;
    editor.hierarchy.selected_index      = -1;
    editor.hierarchy.show_hidden         = false;
    editor.inspector_fs_split            = 0.5f; // Split inspector/filesystem at 50%
    editor.fs_split_target               = 0.5f;
    editor.fs_split_start                = 0.5f;
    editor.fs_split_saved                = 0.5f;
    editor.fs_anim_t                     = 1.0f;
    editor.fs_collapsed                  = false;
    editor.last_tab_click_time           = 0.0;

    // Cache SVGs instantly via our zero-copy binary pipeline
    editor.file_manager.icon_folder      = texture_pool_add_svg(&context, "./assets/icons/Folder.svg", 16, 16);
    editor.file_manager.icon_file        = texture_pool_add_svg(&context, "./assets/icons/File.svg", 16, 16);
    editor.file_manager.icon_arrow_right = texture_pool_add_svg(&context, "./assets/icons/GuiTreeArrowRight.svg", 16, 16);
    editor.file_manager.icon_arrow_down  = texture_pool_add_svg(&context, "./assets/icons/GuiTreeArrowDown.svg", 16, 16);
    s_icon_lock = texture_pool_add_svg(&context, "./assets/icons/Lock.svg", 16, 16);

    file_manager_navigate("./assets");

    // ── Font ──────────────────────────────────────────────────────────────
    editor.font = load_font("./assets/fonts/MapleMono-NF-Regular.ttf", 18);

    // ── Keybindings ───────────────────────────────────────────────────────
    // M-j = Files (bottom)   M-l = Inspector (right)
    // M-k = Console (top)    M-h = Hierarchy (left)
    keychord_bind(&keymap, "M-j", toggle_bottom, "Toggle file manager", PRESS);
    keychord_bind(&keymap, "M-l", toggle_right,  "Toggle inspector",    PRESS);
    keychord_bind(&keymap, "M-k", toggle_top,    "Toggle console",      PRESS);
    keychord_bind(&keymap, "M-h", toggle_left,   "Toggle hierarchy",    PRESS);
    keychord_bind(&keymap, "c", cb_open_color_picker, "Pick Mesh Color", PRESS);

    // Search keybinds!
    keychord_bind(&keymap, "C-s", search_start, "Start search", PRESS);

    keymap_init(&editor_search_keymap);
    keychord_bind(&editor_search_keymap, "C-g", search_cancel, "Cancel search", PRESS);
    keychord_bind(&editor_search_keymap, "RET", search_commit, "Commit search", PRESS);
    keychord_bind(&editor_search_keymap, "C-n", search_next, "Next search match", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "C-p", search_prev, "Prev search match", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "DEL", editor_search_backspace, "Search backspace", PRESS | REPEAT);

    colorpicker_init(&s_color_picker);

    colorpicker_init(&s_color_picker);
    image_viewer_init(&s_image_viewer);

    editor.last_time   = glfwGetTime();
    editor.initialized = true;

    fprintf(stdout, "[Editor] Initialized — easeOutExpo panels, MapleMono 18px\n");
}

void editor_cleanup(void) {
    if (!editor.initialized) return;
    if (editor.font) {
        destroy_font(editor.font);
        editor.font = NULL;
    }
    editor.initialized = false;
}

void editor_update(void) {
    if (!editor.initialized) return;

    double now = glfwGetTime();
    float  dt  = (float)(now - editor.last_time);
    editor.last_time = now;
    if (dt > EDITOR_MAX_DT) dt = EDITOR_MAX_DT;

    if (s_color_picker.visible) {
        colorpicker_update(&s_color_picker, dt, s_ui_mx, s_ui_my);
    }

    if (s_image_viewer.visible) {
        image_viewer_update(&s_image_viewer, dt, s_ui_mx, s_ui_my);
    }

    if (s_is_searching) {
        if (s_search_anim_t < 1.0f) {
            s_search_anim_t += 3.0f * dt;
        }
    }

    // High-End UI Easing: 333ms Cubic Out for a highly tactile scroll
    if (s_fs_scroll_t < 1.0f) {
        s_fs_scroll_t += 3.0f * dt;
        if (s_fs_scroll_t >= 1.0f) {
            s_fs_scroll_t = 1.0f;
            s_fs_scroll_y = s_fs_scroll_target;
        } else {
            s_fs_scroll_y = s_fs_scroll_start + (s_fs_scroll_target - s_fs_scroll_start) * ease_cubic_out(s_fs_scroll_t);
        }
    }

    float msg_h = editor.font ? (editor.font->ascent - editor.font->descent + 32.0f) : 50.0f;

    if (s_message.active || s_message.y > -(msg_h + 5.0f)) {
        float w = measure_text_width(editor.font, s_message.text, 1.0f) + 32.0f;

        s_message.hovered = (s_ui_mx >= s_message.x && s_ui_mx <= s_message.x + w &&
                             s_ui_my >= s_message.y && s_ui_my <= s_message.y + msg_h);

        // Pause timer and lerp color on hover/drag
        if (s_message.hovered || s_message.is_dragging) {
            s_message.timer = 3.0f;
            s_message.hover_t += 10.0f * dt;
            if (s_message.hover_t > 1.0f) s_message.hover_t = 1.0f;
        } else {
            s_message.hover_t -= 10.0f * dt;
            if (s_message.hover_t < 0.0f) s_message.hover_t = 0.0f;

            if (s_message.active) {
                s_message.timer -= dt;
                if (s_message.timer <= 0.0f) s_message.active = false;
            }
        }

        float target_x = 20.0f;
        float target_y = s_message.active ? 20.0f : -(msg_h + 20.0f);

        if (s_message.is_dragging) {
            float raw_x = (float)s_ui_mx - s_message.drag_offset_x;
            float raw_y = (float)s_ui_my - s_message.drag_offset_y;

            float dx = raw_x - target_x;
            float dy = raw_y - target_y;
            float dist = sqrtf(dx * dx + dy * dy);

            // Radial rubber-band physics: pure circular clamping
            // Multiplying the previous 250.0 radius by sqrt(2) gives exactly double the area!
            float limit = 353.5f;
            float pull_dist = dist / (1.0f + dist / limit);

            if (dist > 0.0001f) {
                s_message.x = target_x + (dx / dist) * pull_dist;
                s_message.y = target_y + (dy / dist) * pull_dist;
            } else {
                s_message.x = target_x;
                s_message.y = target_y;
            }
            s_message.vel_x = 0.0f;
            s_message.vel_y = 0.0f;
        } else {
            // Hooke's Law spring physics!
            float tension = 800.0f;
            float damp    = 24.0f;

            float ax = (target_x - s_message.x) * tension - s_message.vel_x * damp;
            float ay = (target_y - s_message.y) * tension - s_message.vel_y * damp;

            s_message.vel_x += ax * dt;
            s_message.vel_y += ay * dt;
            s_message.x += s_message.vel_x * dt;
            s_message.y += s_message.vel_y * dt;
        }
    }

    // Animate FileSystem Splitter
    if (editor.fs_anim_t < 1.0f) {
        editor.fs_anim_t += 4.0f * dt; // 250ms duration
        if (editor.fs_anim_t >= 1.0f) {
            editor.fs_anim_t = 1.0f;
            editor.inspector_fs_split = editor.fs_split_target;
        } else {
            float e = ease_quart_in_out(editor.fs_anim_t);
            editor.inspector_fs_split = editor.fs_split_start + (editor.fs_split_target - editor.fs_split_start) * e;
        }
    }

    for (int i = 0; i < PANEL_COUNT; i++) {
        Panel* p    = &editor.panels[i];
        float  diff = p->target_t - p->t;

        if (fabsf(diff) < 0.0001f) {
            p->t = p->target_t;
        } else {
            // Exponential approach: smooth, frame-rate independent
            p->t += diff * EDITOR_ANIM_SPEED * dt;
        }
    }
}

void editor_render(void) {
    if (!editor.initialized) return;

    for (int i = 0; i < PANEL_COUNT; i++) {
        Panel* p = &editor.panels[i];

        // Skip fully-closed, non-animating panels
        if (p->t < 0.0005f && p->target_t <= 0.0f) continue;

        float x, y, w, h;
        panel_get_rect(p, &x, &y, &w, &h);

        panel_draw_chrome(p, x, y, w, h);

        if (p->render_content)
            p->render_content(p, x, y, w, h);
    }

    colorpicker_render(&s_color_picker, editor.font);
    image_viewer_render(&s_image_viewer, editor.font);

    // Render the active notification popup
    float msg_h = editor.font ? (editor.font->ascent - editor.font->descent + 32.0f) : 50.0f;
    if (s_message.active || s_message.y > -(msg_h + 5.0f)) {
        float text_w = measure_text_width(editor.font, s_message.text, 1.0f);
        float pad = 16.0f;
        float w = text_w + pad * 2.0f;
        float h = msg_h;

        float x = s_message.x;
        float y = s_message.y;

        vec4 radii = {8.0f, 8.0f, 8.0f, 8.0f};

        // Shadow
        Color shadow = {0.0f, 0.0f, 0.0f, 0.3f};
        exQuad2D((vec2){x + 4.0f, y - 4.0f}, (vec2){w, h}, radii, 0.0f, shadow, shadow);

        // Themed Border
        Color base_border = CT.text_dim;
        if (s_message.type == MSG_ERROR) base_border = CT.error;
        else if (s_message.type == MSG_WARNING) base_border = CT.warning;
        else if (s_message.type == MSG_SUCCESS) base_border = CT.success;

        Color border;
        border.r = base_border.r + (CT.border.r - base_border.r) * s_message.hover_t;
        border.g = base_border.g + (CT.border.g - base_border.g) * s_message.hover_t;
        border.b = base_border.b + (CT.border.b - base_border.b) * s_message.hover_t;
        border.a = base_border.a + (CT.border.a - base_border.a) * s_message.hover_t;

        // Background and Border in a single call
        exQuad2D((vec2){x, y}, (vec2){w, h}, radii, 2.0f, border, CT.bg);

        // Text
        float ty = y + h * 0.5f - 2.0f; // Visual center
        text(editor.font, s_message.text, x + pad, ty, CT.text);
    }

    s_ui_mclicked = false; // Reset one-frame click flag
}

/// Panel Control

void editor_toggle_panel(PanelSide side) {
    Panel* p    = &editor.panels[side];
    p->open     = !p->open;
    p->target_t = p->open ? 1.0f : 0.0f;
}

void editor_open_panel(PanelSide side) {
    Panel* p    = &editor.panels[side];
    p->open     = true;
    p->target_t = 1.0f;
}

void editor_close_panel(PanelSide side) {
    Panel* p    = &editor.panels[side];
    p->open     = false;
    p->target_t = 0.0f;
}

bool editor_panel_is_open(PanelSide side) {
    return editor.panels[side].open;
}

/// Inspector API

void inspector_select_mesh(int index) {
    editor.inspector.selected_mesh_index = index;
    editor.hierarchy.selected_index      = index;
    gizmo.active = true;
}

void inspector_deselect(void) {
    editor.inspector.selected_mesh_index = -1;
    editor.hierarchy.selected_index      = -1;
    gizmo.active = false;
}

/// Hierarchy API

void hierarchy_select(int index) {
    editor.hierarchy.selected_index = index;
    if (index >= 0 && index < (int)scene.meshes.count) {
        editor.inspector.selected_mesh_index = index;
        gizmo.active = true;
    } else {
        editor.inspector.selected_mesh_index = -1;
        gizmo.active = false;
    }
}
