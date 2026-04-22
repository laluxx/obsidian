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
#include "vertico.h"
#include "text_editor.h"
#include "animation_editor.h"
#include "tree_view.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

///  Globals

Editor editor = {0};
bool editor_show_bones = true;
void* editor_selected_bone = NULL;

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
static int32_t s_icon_mesh = -1;
static int32_t s_icon_world = -1;
static int32_t s_icon_visible = -1;
static int32_t s_icon_hidden = -1;
static int32_t s_icon_bone = -1;

static float s_ui_dt = 0.0f;

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

static double s_bottom_click_time = 0.0;
static float s_bottom_saved_size = 280.0f;
static float s_bottom_anim_t = 1.0f;
static float s_bottom_start_size = 280.0f;
static float s_bottom_target_size = 280.0f;

static bool s_hier_needs_scroll = false;

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

static TreeViewState s_fs_tree_state;
static TreeViewState s_hier_tree_state;
static TreeViewState s_bone_tree_state;

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
    if (text_editor_wants_mouse(s_ui_mx, s_ui_my)) return true;

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

            if (editor.panels[PANEL_BOTTOM].open && editor.panels[PANEL_BOTTOM].t > 0.01f) {
                float px, py, pw, ph;
                panel_get_rect(&editor.panels[PANEL_BOTTOM], &px, &py, &pw, &ph);
                float bar_y = py + ph - TEXT_EDITOR_TITLE_H;

                float close_size = 16.0f;
                float close_x = px + pw - 14.0f - close_size;
                if (s_ui_mx >= close_x - 8.0f && s_ui_mx <= close_x + close_size + 8.0f &&
                    s_ui_my >= bar_y && s_ui_my <= bar_y + TEXT_EDITOR_TITLE_H) {
                    if (g_anim_editor.visible) {
                        anim_editor_close();
                    } else {
                        extern void text_editor_close(void);
                        text_editor_close();
                    }
                    s_ui_mdown = true;
                    s_ui_mclicked = true;
                    return; /* Consumed! */
                }

                /* Relaxed bounds so fast or slight-off clicks on the title bar still capture */
                float hit_y_min = g_anim_editor.visible ? bar_y : bar_y - 8.0f; // Don't relax downwards into the ruler!
                if (s_ui_mx >= px && s_ui_mx <= px + pw && s_ui_my >= hit_y_min && s_ui_my <= bar_y + TEXT_EDITOR_TITLE_H + 8.0f) {
                    bool on_buttons = false;
                    if (g_anim_editor.visible) {
                        float label_w = 0.0f;
                        if (g_anim_editor.selected_track >= 0 && g_anim_editor.selected_track < g_anim_editor.track_count)
                            label_w = measure_text_width(editor.font, g_anim_editor.tracks[g_anim_editor.selected_track].name, 1.0f);
                        float btn_start = px + 12.0f + label_w + 12.0f; // Pad + Label + Spacing
                        float btn_end = btn_start + 64.0f; // Width of Stop + Play/Pause buttons + padding
                        if (s_ui_mx >= btn_start && s_ui_mx <= btn_end) on_buttons = true;
                    }

                    if (!on_buttons) {
                        if (editor.panels[PANEL_BOTTOM].size <= TEXT_EDITOR_TITLE_H + 2.0f) {
                            s_bottom_start_size = editor.panels[PANEL_BOTTOM].size;
                            s_bottom_target_size = s_bottom_saved_size > TEXT_EDITOR_TITLE_H + 20.0f ? s_bottom_saved_size : 280.0f;
                            s_bottom_anim_t = 0.0f;
                        } else {
                            double now = glfwGetTime();
                            if (now - s_bottom_click_time < 0.3) {
                                s_bottom_saved_size = editor.panels[PANEL_BOTTOM].size;
                                s_bottom_start_size = editor.panels[PANEL_BOTTOM].size;
                                s_bottom_target_size = TEXT_EDITOR_TITLE_H;
                                s_bottom_anim_t = 0.0f;
                            } else {
                                s_ui_active_id = &editor.panels[PANEL_BOTTOM].size;
                                s_ui_drag_start_y = (float)s_ui_my;
                                s_ui_drag_start_val = editor.panels[PANEL_BOTTOM].size;
                            }
                        }
                        s_bottom_click_time = glfwGetTime();
                        s_ui_mdown = true;
                        s_ui_mclicked = true;
                        return; /* Consumed! */
                    }
                }
            }

            if (anim_editor_wants_mouse(s_ui_mx, sh - s_ui_my)) {
                anim_editor_mouse_button(button, action, s_ui_mx, sh - s_ui_my);
                return; /* Consumed! */
            }
            if (g_anim_editor.visible && anim_editor_wants_mouse(s_ui_mx, sh - s_ui_my)) {
                anim_editor_mouse_button(button, action, s_ui_mx, sh - s_ui_my);
                return; /* Consumed! */
            } else if (!g_anim_editor.visible && text_editor_wants_mouse(s_ui_mx, sh - s_ui_my)) {
                text_editor_mouse_button(button, action, s_ui_mx, sh - s_ui_my);
                return; /* Consumed! */
            }

            s_ui_mdown = true;
            s_ui_mclicked = true;
        } else if (action == GLFW_RELEASE) {
            if (s_message.is_dragging) {
                s_message.is_dragging = false;
                return; /* Consumed! */
            }

            /* Forward release to clear selection states */
            anim_editor_mouse_button(button, action, s_ui_mx, sh - s_ui_my);
            text_editor_mouse_button(button, action, s_ui_mx, sh - s_ui_my);

            s_ui_mdown = false;
            s_ui_active_id = NULL; /* Release drag */
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
            // Centered, width mirrors the Vertico top panel.
            // Slides up from below the screen.
            float h = p->size;
            float w = sw * 0.45f;
            if (g_anim_editor.visible) w = (sw * 0.85f) - 30.0f; // Animation editor gets a slightly less wide panel
            if (w < 450.0f) w = 450.0f;
            if (w > sw - 40.0f) w = sw - 40.0f;
            *out_x = (sw - w) * 0.5f;
            *out_w = w;
            *out_h = h;
            *out_y = lerpf(-h, 0.0f, et);
            break;
        }

        case PANEL_TOP: {
            // Centered dropdown console (Vertico Palette)
            float h = p->size;
            float w = sw * 0.45f;
            if (w < 450.0f) w = 450.0f;
            if (w > sw - 40.0f) w = sw - 40.0f;
            *out_x = (sw - w) * 0.5f; // Centered
            *out_w = w;
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

static void panel_draw_bg(Panel* p, float x, float y, float w, float h) {
    vec4 radii = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (p->side) {
        case PANEL_BOTTOM: radii[0] = PANEL_RADIUS; radii[1] = PANEL_RADIUS; break;
        case PANEL_TOP:    radii[2] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        case PANEL_LEFT:   radii[1] = PANEL_RADIUS; radii[2] = PANEL_RADIUS; break;
        case PANEL_RIGHT:  radii[0] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        default: break;
    }
    exQuad2D((vec2){x, y}, (vec2){w, h}, radii, 0.0f, CT.bg, CT.bg);

    if (p->side == PANEL_TOP) {
        Color bar = CT.bg_alt;
        float bar_h = TITLE_H;
        exQuad2D((vec2){x, y}, (vec2){w, bar_h}, (vec4){0.0f, 0.0f, radii[2], radii[3]}, 0.0f, bar, bar);
    }
}

static void panel_draw_titlebar(Panel* p, float x, float y, float w, float h) {
    vec4 radii = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (p->side) {
        case PANEL_BOTTOM: radii[0] = PANEL_RADIUS; radii[1] = PANEL_RADIUS; break;
        case PANEL_TOP:    radii[2] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        case PANEL_LEFT:   radii[1] = PANEL_RADIUS; radii[2] = PANEL_RADIUS; break;
        case PANEL_RIGHT:  radii[0] = PANEL_RADIUS; radii[3] = PANEL_RADIUS; break;
        default: break;
    }

    Color bar = CT.bg_alt;
    float bar_h = TITLE_H;
    float tx = x + PAD;
    float ty;

    switch (p->side) {
        case PANEL_TOP:
            ty = y + bar_h  * 0.5f;
            break;
        default:
            exQuad2D((vec2){x, y + h - bar_h}, (vec2){w, bar_h}, (vec4){radii[0], radii[1], 0.0f, 0.0f}, 0.0f, bar, bar);
            ty = y + h - bar_h * 0.5f;
            break;
    }

    if (p->side == PANEL_BOTTOM && p->t > 0.0f) {
        if (g_anim_editor.visible) {
            extern void anim_editor_draw_titlebar(float x, float y, float w, float h, float mx, float my);
            anim_editor_draw_titlebar(x, y + h - bar_h, w, bar_h, (float)s_ui_mx, (float)s_ui_my);
        } else {
            extern void text_editor_draw_titlebar(float x, float y, float w, float h, float mx, float my);
            text_editor_draw_titlebar(x, y + h - bar_h, w, bar_h, (float)s_ui_mx, (float)s_ui_my);
        }
    } else if (p->title && p->title[0] != '\0') {
        text(editor.font, p->title, tx, ty, CT.text);
    }
}

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

static void draw_field_icon(float cx, float row, const char* label, int32_t icon_id) {
    if (icon_id >= 0) {
        float lw = measure_text_width(editor.font, label, 1.0f);
        Texture2D* tex = texture_pool_get(icon_id);
        if (tex && tex->loaded) {
            float icon_y = row - (editor.font->ascent - editor.font->descent) * 0.5f - 1.0f;
            texture2D((vec2){cx + lw + 8.0f, icon_y}, (vec2){16, 16}, tex, CT.text_dim);
        }
    }
}
typedef struct {
    void* id;
    float t;
    float vel;
    float color_t;
} ToggleAnimState;
static ToggleAnimState s_toggles[128];

static bool field_toggle(float cx, float row, float col2_x, float cw, const char* label, bool* val, int32_t icon_id) {
    (void)cw;
    bool changed = false;
    text(editor.font, label, cx, row, CT.text_dim);
    draw_field_icon(cx, row, label, icon_id);

    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    float track_w = 34.0f;
    float track_h = box_h * 0.7f;
    float track_y = box_y + (box_h - track_h) * 0.5f;
    float track_x = col2_x + 8.0f;

    bool hovered = (s_ui_mx >= track_x && s_ui_mx <= track_x + track_w &&
                    s_ui_my >= track_y && s_ui_my <= track_y + track_h);

    if (hovered && s_ui_mclicked) {
        *val = !(*val);
        changed = true;
    }

    ToggleAnimState* anim = NULL;
    for (int i = 0; i < 128; i++) {
        if (s_toggles[i].id == (void*)val) { anim = &s_toggles[i]; break; }
        if (s_toggles[i].id == NULL) {
            s_toggles[i].id = (void*)val;
            s_toggles[i].t = *val ? 1.0f : 0.0f;
            s_toggles[i].vel = 0.0f;
            s_toggles[i].color_t = *val ? 1.0f : 0.0f;
            anim = &s_toggles[i];
            break;
        }
    }

    if (anim) {
        float target = *val ? 1.0f : 0.0f;

        // Bouncy physics for the thumb
        float tension = 600.0f;
        float damp = 22.0f;
        float accel = (target - anim->t) * tension - anim->vel * damp;
        anim->vel += accel * s_ui_dt;
        anim->t += anim->vel * s_ui_dt;

        // Exponential smoothing for the color to prevent green flashes during bounce
        anim->color_t += (target - anim->color_t) * 15.0f * s_ui_dt;

        Color track_col = lerp_color(CT.bg_deep, CT.success, anim->color_t);
        vec4 track_rad = {track_h*0.5f, track_h*0.5f, track_h*0.5f, track_h*0.5f};
        exQuad2D((vec2){track_x, track_y}, (vec2){track_w, track_h}, track_rad, 0.0f, track_col, track_col);

        float thumb_size = track_h - 4.0f;
        float thumb_min_x = track_x + 2.0f;
        float thumb_max_x = track_x + track_w - 2.0f - thumb_size;
        float thumb_x = thumb_min_x + (thumb_max_x - thumb_min_x) * anim->t;

        vec4 thumb_rad = {thumb_size*0.5f, thumb_size*0.5f, thumb_size*0.5f, thumb_size*0.5f};
        exQuad2D((vec2){thumb_x, track_y + 2.0f}, (vec2){thumb_size, thumb_size}, thumb_rad, 0.0f, CT.text, CT.text);
    }
    return changed;
}

static bool field_vec3(float cx, float row, float col2_x, float cw,
                       const char* label, float* v, const char* unit, float speed, int32_t icon_id) {
    if (!editor.font) return false;
    bool changed = false;
    text(editor.font, label, cx, row, CT.text_dim);
    if (icon_id >= 0) draw_field_icon(cx, row, label, icon_id);

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

        if (icon_id < 0 && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
            s_ui_active_id = id;
            s_ui_drag_start_x = (float)s_ui_mx;
        }

        if (icon_id < 0 && s_ui_active_id == id) {
            float delta = ((float)s_ui_mx - s_ui_drag_start_x) * speed;
            GLFWwindow* win = glfwGetCurrentContext();
            if (win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)) {
                delta *= 0.1f;
            }
            v[i] += delta;
            s_ui_drag_start_x = (float)s_ui_mx;
            changed = true;
        }

        Color label_col = ((icon_id < 0) && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? axis_colors_active[i] : axis_colors[i];

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
                             const char* label, float* c, int32_t icon_id) {
    if (!editor.font) return false;
    bool changed = false;
    text(editor.font, label, cx, row, CT.text_dim);
    if (icon_id >= 0) draw_field_icon(cx, row, label, icon_id);

    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    float swatch_size = box_h - 4.0f;
    float swatch_x = col2_x - 25.0f;
    Color actual_color = {c[0], c[1], c[2], c[3]};
    exQuad2D((vec2){swatch_x, box_y + 2.0f}, (vec2){swatch_size, swatch_size}, (vec4){3.0f, 3.0f, 3.0f, 3.0f}, 0.0f, actual_color, actual_color);

    bool swatch_hovered = (s_ui_mx >= swatch_x && s_ui_mx <= swatch_x + swatch_size &&
                           s_ui_my >= box_y + 2.0f && s_ui_my <= box_y + 2.0f + swatch_size);

    if (icon_id < 0 && swatch_hovered && s_ui_mclicked) {
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

        if (icon_id < 0 && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
            s_ui_active_id = id;
            s_ui_drag_start_x = (float)s_ui_mx;
        }

        if (icon_id < 0 && s_ui_active_id == id) {
            float delta = ((float)s_ui_mx - s_ui_drag_start_x) * 0.005f;
            GLFWwindow* win = glfwGetCurrentContext();
            if (win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)) {
                delta *= 0.1f;
            }
            c[i] = clampf(c[i] + delta, 0.0f, 1.0f);
            s_ui_drag_start_x = (float)s_ui_mx;
            changed = true;
        }

        Color label_col = ((icon_id < 0) && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? axis_colors_active[i] : axis_colors[i];

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
                        bool clamp_val, float min_v, float max_v, int32_t icon_id) {
    if (!editor.font) return false;
    bool changed = false;

    bool hovered = (s_ui_mx >= cx && s_ui_mx <= cx + cw &&
                    s_ui_my >= row - editor.font->descent - 3.0f &&
                    s_ui_my <= row + editor.font->ascent + 3.0f);
    void* id = (void*)val;

    if (icon_id < 0 && hovered && s_ui_mclicked && s_ui_active_id == NULL) {
        s_ui_active_id = id;
        s_ui_drag_start_x = (float)s_ui_mx;
    }

    if (icon_id < 0 && s_ui_active_id == id) {
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

    Color label_col = ((icon_id < 0) && (s_ui_active_id == id || (hovered && s_ui_active_id == NULL))) ? CT.text : CT.text_dim;
    text(editor.font, label, cx, row, label_col);
    if (icon_id >= 0) draw_field_icon(cx, row, label, icon_id);

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
                       const char* label, const char* value, int32_t icon_id) {
    (void)cw;
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);
    if (icon_id >= 0) draw_field_icon(cx, row, label, icon_id);

    // Read-only text fields (like Geometry) no longer have heavy backgrounds!
    text(editor.font, value, col2_x + 8.0f, row, CT.text);
}

static bool begin_section(const char* label, bool* is_open, float cx, float* row, float cw, float lh) {
    float hit_y = *row - editor.font->descent - 3.0f;
    float header_h = lh;
    bool hovered = (s_ui_mx >= cx - 4.0f && s_ui_mx <= cx + cw + 4.0f &&
                    s_ui_my >= hit_y && s_ui_my <= hit_y + header_h);

    if (hovered && s_ui_mclicked) {
        *is_open = !(*is_open);
    }

    int32_t icon = *is_open ? editor.file_manager.icon_arrow_down : editor.file_manager.icon_arrow_right;
    Texture2D* tex = texture_pool_get(icon);
    if (tex && tex->loaded) {
        float icon_y = *row - (editor.font->ascent - editor.font->descent) * 0.5f - 1.0f;
        texture2D((vec2){cx, icon_y}, (vec2){16, 16}, tex, hovered ? CT.text : CT.accent);
    }

    Color text_col = hovered ? CT.text : CT.accent;
    text(editor.font, label, cx + 20.0f, *row, text_col);

    *row -= lh + 6.0f;
    return *is_open;
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

    static bool s_sec_file_info = true;
    static bool s_sec_dimensions = true;
    static bool s_sec_texture_data = true;

    // ── File Info ──────────────────────────────────────────────────────────
    if (begin_section("File Info", &s_sec_file_info, cx, &row, cw, lh)) {
        field_text(cx, row, col2, cw, "Name", iv->filename, -1);
        row -= lh + 6.0f;

        const char* ext = strrchr(iv->filename, '.');
        field_text(cx, row, col2, cw, "Type", ext ? ext : "Unknown", -1);
        row -= lh + 6.0f;

        char size_str[64];
        if (iv->file_size < 1024) snprintf(size_str, sizeof(size_str), "%zu Bytes", iv->file_size);
        else if (iv->file_size < 1024 * 1024) snprintf(size_str, sizeof(size_str), "%.2f KB", iv->file_size / 1024.0f);
        else snprintf(size_str, sizeof(size_str), "%.2f MB", iv->file_size / (1024.0f * 1024.0f));
        field_text(cx, row, col2, cw, "Size on Disk", size_str, -1);
        row -= lh + 6.0f;
        row -= 6.0f;
    }

    // ── Dimensions ─────────────────────────────────────────────────────────
    if (begin_section("Dimensions", &s_sec_dimensions, cx, &row, cw, lh)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d px", tex->width);
        field_text(cx, row, col2, cw, "Width", buf, -1);
        row -= lh + 6.0f;

        snprintf(buf, sizeof(buf), "%d px", tex->height);
        field_text(cx, row, col2, cw, "Height", buf, -1);
        row -= lh + 6.0f;

        float aspect = (float)tex->width / (float)tex->height;
        snprintf(buf, sizeof(buf), "%.3f", aspect);
        field_text(cx, row, col2, cw, "Aspect Ratio", buf, -1);
        row -= lh + 6.0f;
        row -= 6.0f;
    }

    // ── Texture Data ───────────────────────────────────────────────────────
    if (begin_section("Texture Data", &s_sec_texture_data, cx, &row, cw, lh)) {
        char buf[64];
        char size_str[64];
        // Exact mathematical VRAM usage for an uncompressed RGBA8 texture
        size_t mem_size = (size_t)tex->width * (size_t)tex->height * 4;
        snprintf(buf, sizeof(buf), "%zu Bytes", mem_size);
        field_text(cx, row, col2, cw, "Memory Size", buf, -1);
        row -= lh + 6.0f;

        if (mem_size < 1024) snprintf(size_str, sizeof(size_str), "%zu B", mem_size);
        else if (mem_size < 1024 * 1024) snprintf(size_str, sizeof(size_str), "%.2f KB", mem_size / 1024.0f);
        else snprintf(size_str, sizeof(size_str), "%.2f MB", mem_size / (1024.0f * 1024.0f));

        field_text(cx, row, col2, cw, "Formatted Mem", size_str, -1);
        row -= lh + 6.0f;

        field_text(cx, row, col2, cw, "Internal Format", "RGBA8 Unorm", -1);
        row -= lh + 6.0f;
        field_text(cx, row, col2, cw, "Color Space", "sRGB", -1);
        row -= lh + 6.0f;
        field_text(cx, row, col2, cw, "Bit Depth", "8-bit", -1);
        row -= lh + 6.0f;
        field_text(cx, row, col2, cw, "Has Alpha", "Yes", -1);
        row -= lh + 6.0f;
        field_text(cx, row, col2, cw, "Alpha Mode", "Straight", -1);
        row -= lh + 6.0f;
        row -= 6.0f;
    }
}


static void bone_on_select(int index, void* user_data) {
    s_bone_tree_state.selected_index = index;
    editor_selected_bone = user_data;
}

static void bone_on_toggle_expand(int index, void* user_data) {
    (void)index;
    OmdlNode* node = (OmdlNode*)user_data;
    if (node) node->expanded = !node->expanded;
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

    static bool s_sec_transform = true;
    static bool s_sec_material = true;
    static bool s_sec_geometry = true;
    static bool s_sec_bounds = true;
    static bool s_sec_animation = true;

    // ── Transform ──────────────────────────────────────────────────────────
    if (begin_section("Transform", &s_sec_transform, cx, &row, cw, lh)) {
        vec3 pos = {m->local_transform[3][0], m->local_transform[3][1], m->local_transform[3][2]};
        vec3 euler;
        glm_euler_angles(m->local_transform, euler);
        vec3 rot_deg = {glm_deg(euler[0]), glm_deg(euler[1]), glm_deg(euler[2])};
        vec3 scale = {
            glm_vec3_norm((vec3){m->local_transform[0][0], m->local_transform[1][0], m->local_transform[2][0]}),
            glm_vec3_norm((vec3){m->local_transform[0][1], m->local_transform[1][1], m->local_transform[2][1]}),
            glm_vec3_norm((vec3){m->local_transform[0][2], m->local_transform[1][2], m->local_transform[2][2]})
        };

        bool transform_changed = false;
        transform_changed |= field_vec3(cx, row, col2, cw, "Position", pos, "m", 0.05f, -1);
        row -= lh + 6.0f;
        transform_changed |= field_vec3(cx, row, col2, cw, "Rotation", rot_deg, "°", 0.5f, -1);
        row -= lh + 6.0f;
        transform_changed |= field_vec3(cx, row, col2, cw, "Scale", scale, "x", 0.02f, -1);
        row -= lh + 6.0f;
        row -= 6.0f;

        if (transform_changed) {
            glm_mat4_identity(m->local_transform);
            glm_translate(m->local_transform, pos);
            glm_rotate_z(m->local_transform, glm_rad(rot_deg[2]), m->local_transform);
            glm_rotate_y(m->local_transform, glm_rad(rot_deg[1]), m->local_transform);
            glm_rotate_x(m->local_transform, glm_rad(rot_deg[0]), m->local_transform);
            glm_scale(m->local_transform, scale);

            // Sync to model matrix for immediate rendering
            glm_mat4_copy(m->local_transform, m->model);
            markMeshesSSBODirty(&context);
        }
    }

    // ── Material ───────────────────────────────────────────────────────────
    if (begin_section("Material", &s_sec_material, cx, &row, cw, lh)) {
        static vec4 s_proxy_emissive = {0};
        static vec4 s_proxy_attenuation = {0};
        static int s_proxy_mesh_idx = -1;

        if (s_proxy_mesh_idx != s->selected_mesh_index || (!s_color_picker.visible && s_ui_active_id == NULL)) {
            s_proxy_mesh_idx = s->selected_mesh_index;
            s_proxy_emissive[0] = m->emissiveFactor[0];
            s_proxy_emissive[1] = m->emissiveFactor[1];
            s_proxy_emissive[2] = m->emissiveFactor[2];
            s_proxy_emissive[3] = 1.0f;

            s_proxy_attenuation[0] = m->attenuationColor[0];
            s_proxy_attenuation[1] = m->attenuationColor[1];
            s_proxy_attenuation[2] = m->attenuationColor[2];
            s_proxy_attenuation[3] = 1.0f;
        }

        bool mat_changed = false;
        mat_changed |= field_vec4_color(cx, row, col2, cw, "Color", m->baseColorFactor, -1);
        row -= lh + 6.0f;

        mat_changed |= field_vec4_color(cx, row, col2, cw, "Emissive", s_proxy_emissive, -1);
        if (m->emissiveFactor[0] != s_proxy_emissive[0] || m->emissiveFactor[1] != s_proxy_emissive[1] || m->emissiveFactor[2] != s_proxy_emissive[2]) {
            m->emissiveFactor[0] = s_proxy_emissive[0];
            m->emissiveFactor[1] = s_proxy_emissive[1];
            m->emissiveFactor[2] = s_proxy_emissive[2];
            mat_changed = true;
        }
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Emissive Strength", &m->emissiveStrength, 0.1f, true, 0.0f, 1e6f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Metallic", &m->metallicFactor, 0.01f, true, 0.0f, 1.0f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Roughness", &m->roughnessFactor, 0.01f, true, 0.0f, 1.0f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Transmission", &m->transmissionFactor, 0.01f, true, 0.0f, 1.0f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "IOR", &m->ior, 0.01f, true, 1.0f, 3.0f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Thickness", &m->thicknessFactor, 0.01f, true, 0.0f, 100.0f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_vec4_color(cx, row, col2_mat, cw, "Atten. Color", s_proxy_attenuation, -1);
        if (m->attenuationColor[0] != s_proxy_attenuation[0] || m->attenuationColor[1] != s_proxy_attenuation[1] || m->attenuationColor[2] != s_proxy_attenuation[2]) {
            m->attenuationColor[0] = s_proxy_attenuation[0];
            m->attenuationColor[1] = s_proxy_attenuation[1];
            m->attenuationColor[2] = s_proxy_attenuation[2];
            mat_changed = true;
        }
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Atten. Distance", &m->attenuationDistance, 0.1f, true, 0.0f, 1e6f, -1);
        row -= lh + 6.0f;

        mat_changed |= field_float(cx, row, col2_mat, cw, "Dispersion", &m->dispersion, 0.01f, true, 0.0f, 1.0f, -1);
        row -= lh + 6.0f;
        row -= 6.0f;

        if (mat_changed) {
            markMeshesSSBODirty(&context);
        }
    }

    // ── Geometry ───────────────────────────────────────────────────────────
    if (begin_section("Geometry", &s_sec_geometry, cx, &row, cw, lh)) {
        char buf[64];
        snprintf(buf, sizeof buf, "%u", m->vertexCount);
        field_text(cx, row, col2_geo, cw, "Vertices", buf, -1);
        row -= lh + 6.0f;

        snprintf(buf, sizeof buf, "%u", m->indexCount);
        field_text(cx, row, col2_geo, cw, "Indices", buf, -1);
        row -= lh + 6.0f;

        if (field_toggle(cx, row, col2_geo, cw, "Visible", &m->visible, -1)) {
            if (m->node) {
                scene.tree.nodes[(uint32_t)(uintptr_t)m->node].visible = m->visible;
            }
            markMeshesSSBODirty(&context);
        }
        row -= lh + 6.0f;

        const char* amode = (m->alpha_mode == 0) ? "Opaque" : (m->alpha_mode == 1) ? "Mask" : "Blend";
        field_text(cx, row, col2_geo, cw, "Alpha Mode", amode, -1);
        row -= lh + 6.0f;

        if (m->alpha_mode == 1) {
            if (field_float(cx, row, col2_geo, cw, "Alpha Cutoff", &m->alpha_cutoff, 0.01f, true, 0.0f, 1.0f, -1)) {
                markMeshesSSBODirty(&context);
            }
            row -= lh + 6.0f;
        }

        if (field_toggle(cx, row, col2_geo, cw, "Unlit", &m->is_unlit, -1)) {
            markMeshesSSBODirty(&context);
        }
        row -= lh + 6.0f;

        if (field_toggle(cx, row, col2_geo, cw, "Wireframe", &m->wireframe, -1)) {
            markMeshesSSBODirty(&context);
        }
        row -= lh + 6.0f;

        if (m->name) {
            field_text(cx, row, col2_geo, cw, "Name", m->name, -1);
            row -= lh + 6.0f;
        }
        row -= 6.0f;
    }

    // ── Bounds ─────────────────────────────────────────────────────────────
    if (begin_section("Bounds", &s_sec_bounds, cx, &row, cw, lh)) {
        vec3 bmin = {m->aabbMin[0], m->aabbMin[1], m->aabbMin[2]};
        vec3 bmax = {m->aabbMax[0], m->aabbMax[1], m->aabbMax[2]};
        field_vec3(cx, row, col2, cw, "AABB Min", bmin, "m", 0.0f, s_icon_lock);
        row -= lh + 6.0f;
        field_vec3(cx, row, col2, cw, "AABB Max", bmax, "m", 0.0f, s_icon_lock);
        row -= lh + 6.0f;
        row -= 6.0f;
    }

    // ── Skeleton ──────────────────────────────────────────────────────────
    if (m->jointCount > 0 || m->morphCount > 0) {
        if (begin_section("Skeleton", &s_sec_animation, cx, &row, cw, lh)) {

            field_toggle(cx, row, col2_anim, cw, "Show Bones", &editor_show_bones, -1);
            row -= lh + 6.0f;

            if (m->jointCount > 0) {
                GLTFInstance* inst = NULL;
                for (size_t i = 0; i < scene.gltf_instance_count; i++) {
                    if (s->selected_mesh_index >= (int)scene.gltf_instances[i].mesh_start_index &&
                        s->selected_mesh_index < (int)(scene.gltf_instances[i].mesh_start_index + scene.gltf_instances[i].mesh_count)) {
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
                        bool is_joint[4096] = {false};
                        for (uint32_t j = 0; j < skin->joints_count; j++) {
                            uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                            if (j_node < 4096) is_joint[j_node] = true;
                        }

                        static TreeViewItem bone_items[1024];
                        int b_item_count = 0;

                        typedef struct { int32_t idx; int32_t depth; } BStackEntry;
                        static BStackEntry bstack[1024];
                        int btop = 0;

                        int32_t rev_roots[256];
                        int rev_rcount = 0;
                        for (uint32_t j = 0; j < skin->joints_count; j++) {
                            uint32_t j_node = osg->skin_joints[skin->joints_offset + j];
                            int32_t p = osg->nodes[j_node].parent;
                            if (p < 0 || !is_joint[p]) {
                                rev_roots[rev_rcount++] = j_node;
                            }
                        }

                        for (int i = rev_rcount - 1; i >= 0; i--) {
                            bstack[btop++] = (BStackEntry){rev_roots[i], 0};
                        }

                        while (btop > 0 && b_item_count < 1024) {
                            BStackEntry e = bstack[--btop];
                            int32_t curr_idx = e.idx;
                            int32_t d = e.depth;
                            OmdlNode* curr_node = &osg->nodes[curr_idx];

                            TreeViewItem* tv   = &bone_items[b_item_count++];
                            tv->name           = curr_node->name;
                            tv->type           = TREE_ITEM_FILE;
                            tv->depth          = d;
                            tv->expanded       = curr_node->expanded;

                            if (curr_node == editor_selected_bone) {
                                s_bone_tree_state.selected_index = b_item_count - 1;
                            }

                            tv->selected       = (s_bone_tree_state.selected_index == b_item_count - 1);
                            tv->icon_expanded  = editor.file_manager.icon_arrow_down;
                            tv->icon_collapsed = editor.file_manager.icon_arrow_right;
                            tv->icon_leaf = s_icon_bone;
                            tv->icon_tint = (Color){1.0f, 1.0f, 1.0f, 1.0f};
                            tv->show_dot = false;
                            tv->has_visibility = false;
                            tv->user_data = curr_node;

                            int32_t rev_children[256];
                            int rev_ccount = 0;
                            for (uint32_t j = 0; j < skin->joints_count; j++) {
                                uint32_t c_node = osg->skin_joints[skin->joints_offset + j];
                                if (osg->nodes[c_node].parent == curr_idx) {
                                    rev_children[rev_ccount++] = c_node;
                                }
                            }

                            if (rev_ccount > 0) tv->type = TREE_ITEM_GROUP;

                            if (curr_node->expanded) {
                                for (int i = rev_ccount - 1; i >= 0; i--) {
                                    if (btop < 1024) {
                                        bstack[btop++] = (BStackEntry){rev_children[i], d + 1};
                                    }
                                }
                            }
                        }

                        float tree_h = 320.0f;
                        row -= tree_h;
                        float tab_y = row + tree_h;

                        exQuad2D((vec2){cx, row}, (vec2){cw, tree_h}, (vec4){4,4,4,4}, 0.0f, CT.bg_deepest, CT.bg_deepest);

                        static const TreeViewCallbacks bone_cb = {
                            .on_select = bone_on_select,
                            .on_toggle_expand = bone_on_toggle_expand,
                        };

                        float tree_pad = 8.0f;
                        tree_view_render(
                            &s_bone_tree_state, bone_items, b_item_count,
                            cx + tree_pad, row, cw - tree_pad * 2.0f, tree_h, tab_y + 6.0f, editor.font,
                            s_ui_mx, s_ui_my, s_ui_mdown, s_ui_mclicked, NULL, &bone_cb
                        );
                        row -= 24.0f; // Added extra vertical padding before Transform fields

                        if (s_bone_tree_state.selected_index >= 0 && s_bone_tree_state.selected_index < b_item_count) {
                            OmdlNode* bnode = (OmdlNode*)bone_items[s_bone_tree_state.selected_index].user_data;

                            vec3 pos; glm_vec3_copy(bnode->translation, pos);
                            vec3 scale; glm_vec3_copy(bnode->scale, scale);

                            vec3 euler;
                            mat4 rmat; glm_quat_mat4(bnode->rotation, rmat);
                            glm_euler_angles(rmat, euler);
                            vec3 rot_deg = {glm_deg(euler[0]), glm_deg(euler[1]), glm_deg(euler[2])};

                            bool b_changed = false;
                            b_changed |= field_vec3(cx, row, col2, cw, "Position", pos, "m", 0.05f, -1); row -= lh + 6.0f;
                            b_changed |= field_vec3(cx, row, col2, cw, "Rotation", rot_deg, "°", 0.5f, -1); row -= lh + 6.0f;
                            b_changed |= field_vec3(cx, row, col2, cw, "Scale", scale, "x", 0.02f, -1); row -= lh + 6.0f;

                            if (b_changed) {
                                // Calculate a delta matrix for the world offset
                                mat4 new_local; glm_mat4_identity(new_local);
                                glm_translate(new_local, pos);
                                mat4 new_rmat; glm_mat4_identity(new_rmat);
                                glm_rotate_z(new_rmat, glm_rad(rot_deg[2]), new_rmat);
                                glm_rotate_y(new_rmat, glm_rad(rot_deg[1]), new_rmat);
                                glm_rotate_x(new_rmat, glm_rad(rot_deg[0]), new_rmat);
                                glm_mat4_mul(new_local, new_rmat, new_local);
                                glm_scale(new_local, scale);

                                mat4 old_local; glm_mat4_identity(old_local);
                                glm_translate(old_local, bnode->translation);
                                mat4 old_r; glm_quat_mat4(bnode->rotation, old_r);
                                glm_mat4_mul(old_local, old_r, old_local);
                                glm_scale(old_local, bnode->scale);

                                int32_t bnode_idx = (int32_t)(bnode - osg->nodes);
                                int32_t p_idx = bnode->parent;
                                mat4 p_world; glm_mat4_identity(p_world);
                                if (p_idx >= 0) glm_mat4_copy(osg->world_transforms[p_idx], p_world);

                                mat4 old_world; glm_mat4_mul(p_world, old_local, old_world);
                                mat4 new_world; glm_mat4_mul(p_world, new_local, new_world);

                                mat4 inv_old_world; glm_mat4_inv(old_world, inv_old_world);
                                mat4 world_delta; glm_mat4_mul(new_world, inv_old_world, world_delta);

                                // Find this bone's override index (j)
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
                                    glm_mat4_mul(world_delta, m->bone_overrides[local_j].world_offset, m->bone_overrides[local_j].world_offset);
                                }

                                // Also sync to local transform for un-animated bones so inspector displays it right
                                glm_vec3_copy(pos, bnode->translation);
                                glm_vec3_copy(scale, bnode->scale);
                                glm_mat4_quat(new_rmat, bnode->rotation);
                            }
                        }
                        row -= 6.0f;
                    }
                }
            }

            if (m->morphCount > 0) {
                text(editor.font, "Morph Targets", cx, row, CT.accent);
                row -= lh + 6.0f;
                if (m->morph_data) {
                    for (int i = 0; i < m->morphCount && i < 8; i++) {
                        char wlbl[32];
                        snprintf(wlbl, sizeof wlbl, "  Target %d", i);
                        if (field_float(cx, row, col2_anim, cw, wlbl, &m->morph_data->weights[i], 0.01f, true, 0.0f, 1.0f, -1)) {
                            markMeshesSSBODirty(&context);
                        }
                        row -= lh + 6.0f;
                    }
                }
            }
        }
    }
}

void file_manager_refresh(void);

static void render_emacs_input_box(float box_x, float box_y, float box_w, float box_h,
                                   const char* query, int cursor_pos, double last_key_time) {
    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);
    if (!editor.font) return;

    float cursor_w = font_width(editor.font);
    float cursor_h = box_h;
    float ty = box_y + (editor.font->descent * 2);

    double time_since_key = glfwGetTime() - last_key_time;
    bool cursor_visible = (time_since_key < 0.5) || (fmod(time_since_key, 1.0) < 0.5);

    float cur_draw_x = box_x + 8.0f;
    int len = strlen(query);

    for (int i = 0; i <= len; i++) {
        bool is_cursor_pos = (i == cursor_pos);
        char c = (i < len) ? query[i] : '\0';

        if (is_cursor_pos && cursor_visible) {
            exQuad2D((vec2){cur_draw_x, box_y}, (vec2){cursor_w, cursor_h}, (vec4){2.0f, 2.0f, 2.0f, 2.0f}, 0.0f, CT.accent, CT.accent);
        }

        if (c != '\0') {
            Color col = (is_cursor_pos && cursor_visible) ? CT.bg_deep : CT.accent;
            float adv = character(editor.font, (uint32_t)c, cur_draw_x, ty, col);
            cur_draw_x += adv > 0.0f ? adv : cursor_w;
        } else if (is_cursor_pos) {
            break;
        }
    }
}

static void fs_on_select(int index, void* user_data) {
    FileManagerState* s = &editor.file_manager;
    s->selected_index = index;
    s_fs_tree_state.selected_index = index;

    FileItem* item = (FileItem*)user_data;
    double now = glfwGetTime();
    if (s_last_clicked_item == index && (now - s_last_item_click_time) < 0.3) {
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
                    if (!text_editor_open(item->full_path)) {
                        message(MSG_ERROR, "Failed to open file in text editor");
                    }
                    editor_open_panel(PANEL_BOTTOM);
                }
            }
        }
        s_last_clicked_item = -1;
    } else {
        s_last_clicked_item = index;
        s_last_item_click_time = now;
    }
}

static void fs_on_toggle_expand(int index, void* user_data) {
    FileManagerState* s = &editor.file_manager;
    FileItem* item = (FileItem*)user_data;
    s->selected_index = index;
    s_fs_tree_state.selected_index = index;
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
        if (s->expanded_count < 64)
            strcpy(s->expanded_paths[s->expanded_count++], item->full_path);
    }
    file_manager_refresh();
}

static void render_filesystem_content(float cx, float cy, float cw, float ch) {
    FileManagerState* s = &editor.file_manager;

    exQuad2D((vec2){cx - PAD, cy}, (vec2){cw + PAD*2.0f, ch}, (vec4){0,0,0,0}, 0.0f, CT.bg, CT.bg);

    float tab_h = TITLE_H;
    float tab_y = cy + ch - tab_h;

    if (!editor.fs_collapsed || editor.fs_anim_t < 1.0f) {
        static TreeViewItem tv_items[FILE_MANAGER_MAX_ITEMS];
        for (int i = 0; i < s->item_count; i++) {
            FileItem* fi       = &s->items[i];
            TreeViewItem* tv   = &tv_items[i];
            tv->name           = fi->name;
            tv->type           = (fi->type == FILE_ITEM_DIR) ? TREE_ITEM_DIR : TREE_ITEM_FILE;
            tv->depth          = fi->depth;
            tv->expanded       = fi->expanded;
            tv->selected       = (s->selected_index == i);
            tv->icon_expanded  = s->icon_arrow_down;
            tv->icon_collapsed = s->icon_arrow_right;
            tv->icon_leaf      = (fi->type == FILE_ITEM_DIR) ? s->icon_folder : s->icon_file;
            tv->icon_tint      = (fi->type == FILE_ITEM_DIR) ? CT.accent : (Color){1.0f, 1.0f, 1.0f, 1.0f};
            tv->show_dot       = false;
            tv->dot_color      = (Color){0, 0, 0, 0};
            tv->user_data      = fi;
        }

        s_fs_tree_state.selected_index = s->selected_index;

        int beam_current = (s_is_searching && s_search_current_idx >= 0)
            ? s_search_matches[s_search_current_idx] : -1;
        TreeViewSearchOverlay search_overlay = {
            .active              = s_is_searching,
            .current_match_index = beam_current,
            .prev_match_index    = s_search_prev_match_idx,
            .anim_t              = s_search_anim_t,
        };

        static const TreeViewCallbacks fs_cb = {
            .on_select        = fs_on_select,
            .on_toggle_expand = fs_on_toggle_expand,
        };

        tree_view_render(
            &s_fs_tree_state,
            tv_items, s->item_count,
            cx, cy, cw, ch,
            tab_y,
            editor.font,
            s_ui_mx, s_ui_my,
            s_ui_mdown,
            s_ui_mclicked,
            &search_overlay,
            &fs_cb
        );
    }

    exQuad2D((vec2){cx - PAD, tab_y}, (vec2){cw + PAD*2.0f, tab_h},
             (vec4){8.0f, 8.0f, 0.0f, 0.0f}, 0.0f, CT.bg_alt, CT.bg_alt);
    text(editor.font, "FileSystem", cx, tab_y + tab_h * 0.5f - 2.0f, CT.text);

    if (s_is_searching) {
        char counter_str[32] = "0/0";
        if (s_search_match_count > 0)
            snprintf(counter_str, sizeof(counter_str), "%d/%d",
                     s_search_current_idx + 1, s_search_match_count);
        float counter_text_w = measure_text_width(editor.font, counter_str, 1.0f);
        float mc_w  = counter_text_w + 16.0f;
        float sb_x  = cx + measure_text_width(editor.font, "FileSystem", 1.0f) + 20.0f;
        float sb_w  = cw - (sb_x - cx) - PAD - mc_w - 8.0f;
        float sb_h  = editor.font->ascent - editor.font->descent + 8.0f;
        float sb_y  = tab_y + (tab_h - sb_h) * 0.5f;
        render_emacs_input_box(sb_x, sb_y, sb_w, sb_h, s_search_query,
                               s_search_cursor, s_search_last_key_time);
        float mc_h = tab_h - 8.0f;
        float mc_x = sb_x + sb_w + 8.0f;
        float mc_y = tab_y + 4.0f;
        exQuad2D((vec2){mc_x, mc_y}, (vec2){mc_w, mc_h},
                 (vec4){6.0f, 6.0f, 6.0f, 6.0f}, 0.0f, CT.accent, CT.accent);
        text(editor.font, counter_str, mc_x + 8.0f, mc_y + mc_h * 0.5f - 2.0f, CT.bg);
    }
}

extern size_t vertico_cursor;
extern double vertico_last_key_time;

static void render_text_with_highlights(Font* font, const char* inputText, float x, float y,
                                        const char* pattern, Color default_color) {
    if (!pattern || pattern[0] == '\0') {
        text(font, inputText, x, y, default_color);
        return;
    }

    char text_lower[256], pattern_lower[256];
    strncpy(text_lower, inputText, sizeof(text_lower) - 1);
    strncpy(pattern_lower, pattern, sizeof(pattern_lower) - 1);
    text_lower[sizeof(text_lower) - 1] = '\0';
    pattern_lower[sizeof(pattern_lower) - 1] = '\0';

    for (char* p = text_lower; *p; p++) *p = tolower(*p);
    for (char* p = pattern_lower; *p; p++) *p = tolower(*p);

    char* pattern_words[4];
    int word_count = 0;
    char* pattern_copy = strdup(pattern_lower);
    char* token = strtok(pattern_copy, " ");

    while (token && word_count < 4) {
        pattern_words[word_count++] = token;
        token = strtok(NULL, " ");
    }

    int highlight[256] = {0};

    for (int word_idx = 0; word_idx < word_count; word_idx++) {
        const char* word = pattern_words[word_idx];
        int word_len = strlen(word);
        const char* pos = text_lower;
        while ((pos = strstr(pos, word)) != NULL) {
            int offset = pos - text_lower;
            for (int i = 0; i < word_len; i++) {
                if (highlight[offset + i] == 0) {
                    highlight[offset + i] = word_idx + 1;
                }
            }
            pos++;
        }
    }
    free(pattern_copy);

    Color match_colors_fg[4] = { CT.orderless_match_face_0_fg, CT.orderless_match_face_1_fg, CT.orderless_match_face_2_fg, CT.orderless_match_face_3_fg };
    Color match_colors_bg[4] = { CT.orderless_match_face_0_bg, CT.orderless_match_face_1_bg, CT.orderless_match_face_2_bg, CT.orderless_match_face_3_bg };

    float current_x = x;
    for (size_t i = 0; inputText[i] != '\0'; i++) {
        float char_width = character_width(font, inputText[i]);
        if (highlight[i] > 0) {
            int color_idx = highlight[i] - 1;
            float bg_height = font->ascent - font->descent + 4.0f;
            float bg_y = y - font->descent - 2.0f;
            exQuad2D((vec2){current_x, bg_y}, (vec2){char_width, bg_height}, (vec4){2.0f, 2.0f, 2.0f, 2.0f}, 0.0f, match_colors_bg[color_idx], match_colors_bg[color_idx]);

            char single_char[2] = {inputText[i], '\0'};
            text(font, single_char, current_x, y, match_colors_fg[color_idx]);
        } else {
            char single_char[2] = {inputText[i], '\0'};
            text(font, single_char, current_x, y, default_color);
        }
        current_x += char_width;
    }
}

static void render_vertico_panel(Panel* panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    panel->title = ""; // Re-point to an empty string literal to suppress the chrome title

    float lh = editor.font->ascent - editor.font->descent + LH_EXTRA;

    // The title bar is physically located at the BOTTOM of the top panel
    float tab_y = py;
    float tab_h = TITLE_H;

    // Render the dynamic prompt inside the title bar (without the match count)
    char header[128];
    if (vertico.is_active) {
        snprintf(header, sizeof(header), "%s:", vertico.category);
    } else {
        snprintf(header, sizeof(header), "Console:");
    }

    float header_w = measure_text_width(editor.font, header, 1.0f);

    // Match the FileSystem titlebar text baseline
    float text_y = tab_y + tab_h * 0.5f;
    text(editor.font, header, px + PAD, text_y, CT.prompt);

    // Calculate Match Counter Badge
    char counter_str[32] = "0/0";
    if (vertico.is_active && vertico.filtered_count > 0) {
        snprintf(counter_str, sizeof(counter_str), "%zu/%zu", (size_t)(vertico.selected_index + 1), vertico.filtered_count);
    }
    float counter_text_w = measure_text_width(editor.font, counter_str, 1.0f);
    float mc_w = counter_text_w + 16.0f;

    // Position input box immediately after the prompt, leaving room for the badge
    float input_h = editor.font->ascent - editor.font->descent + 8.0f;
    float input_y = tab_y + (tab_h - input_h) * 0.5f;
    float box_x = px + PAD + header_w + 8.0f;
    float box_w = pw - (box_x - px) - PAD - mc_w - 8.0f;

    render_emacs_input_box(box_x, input_y, box_w, input_h, vertico.input, vertico_cursor, vertico_last_key_time);

    // Draw the Match Counter Badge
    float mc_h = tab_h - 8.0f;
    float mc_x = box_x + box_w + 8.0f;
    float mc_y = tab_y + 4.0f;
    exQuad2D((vec2){mc_x, mc_y}, (vec2){mc_w, mc_h}, (vec4){6.0f, 6.0f, 6.0f, 6.0f}, 0.0f, CT.accent, CT.accent);
    text(editor.font, counter_str, mc_x + 8.0f, mc_y + mc_h * 0.5f - 2.0f, CT.bg);

    // Candidates list - render downwards starting from the top of the content area
    int visible_count = vertico.count;
    if (vertico.filtered_count < (size_t)vertico.count) visible_count = vertico.filtered_count;

    float row_y = cy + ch - lh;
    for (int i = 0; i < visible_count; i++) {
        int idx = vertico.scroll_offset + i;
        if (idx >= (int)vertico.filtered_count) break;

        bool is_selected = ((int)idx == (int)vertico.selected_index);
        if (is_selected) {
            exQuad2D((vec2){cx - 4.0f, row_y - editor.font->descent - 2.0f}, (vec2){cw + 8.0f, lh}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.vertico_current, CT.vertico_current);
        }

        VerticoCandidate* candidate = &vertico.filtered[idx];
        render_text_with_highlights(editor.font, candidate->text, cx, row_y, vertico.input, is_selected ? CT.text : CT.text_dim);

        if (candidate->annotation[0] != '\0') {
            float ann_w = measure_text_width(editor.font, candidate->annotation, 1.0f);
            text(editor.font, candidate->annotation, cx + cw - ann_w, row_y, CT.comment);
        }

row_y -= lh;
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
            editor.fs_collapsed = false;
            editor.fs_split_start = editor.inspector_fs_split;
            editor.fs_split_target = editor.fs_split_saved > 0.0f ? editor.fs_split_saved : 0.5f;
            editor.fs_anim_t = 0.0f;
        } else if (now - editor.last_tab_click_time < 0.3) {
            editor.fs_collapsed = true;
            editor.fs_split_saved = editor.inspector_fs_split;
            editor.fs_split_start = editor.inspector_fs_split;
            editor.fs_split_target = 1.0f - (TITLE_H / ch);
            editor.fs_anim_t = 0.0f;
        } else {
            s_ui_active_id = &editor.inspector_fs_split;
            s_ui_drag_start_val = editor.inspector_fs_split;
            s_ui_drag_start_y = (float)s_ui_my;
        }
        editor.last_tab_click_time = now;
    }

    if (s_ui_active_id == &editor.inspector_fs_split) {
        float delta = ((float)s_ui_my - s_ui_drag_start_y) / ch;
        float max_split = 1.0f - (TITLE_H / ch);
        editor.inspector_fs_split = clampf(s_ui_drag_start_val - delta, 0.1f, max_split);
        editor.fs_split_target = editor.inspector_fs_split;
        editor.fs_anim_t = 1.0f;
        editor.fs_collapsed = false;
        split_y = cy + ch * (1.0f - editor.inspector_fs_split);
    }

    float insp_ch = (cy + ch) - split_y;
    if (insp_ch > 20.0f) {
        render_inspector_content(cx, split_y, cw, insp_ch);
    }

    float fs_ch = split_y - cy;
    if (fs_ch > 20.0f) {
        render_filesystem_content(cx, cy, cw, fs_ch);
    }
}

static void hier_on_select(int index, void* user_data) {
    (void)index;
    SceneNode* node = (SceneNode*)user_data;
    if (!node) return;
    if (node->mesh_index >= 0)
        inspector_select_mesh(node->mesh_index);
}

static void hier_on_toggle_expand(int index, void* user_data) {
    (void)index;
    SceneNode* node = (SceneNode*)user_data;
    if (node) node->expanded = !node->expanded;
}

static void update_effective_visibility(int32_t node_idx, bool parent_visible) {
    if (node_idx < 0 || node_idx >= scene.tree.count) return;
    SceneNode* node = &scene.tree.nodes[node_idx];
    bool effective = parent_visible && node->visible;

    if (node->mesh_index >= 0 && node->mesh_index < (int)scene.meshes.count) {
        scene.meshes.items[node->mesh_index].visible = effective;
    }

    int32_t child = node->first_child;
    while (child >= 0) {
        update_effective_visibility(child, effective);
        child = scene.tree.nodes[child].next_sibling;
    }
}

static void hier_on_toggle_visibility(int index, void* user_data) {
    (void)index;
    SceneNode* node = (SceneNode*)user_data;
    if (node) {
        node->visible = !node->visible;
        update_effective_visibility(0, true);
        markMeshesSSBODirty(&context);
    }
}

static void render_hierarchy(Panel* panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    if (scene.tree.count <= 1) {
        text(editor.font, "Scene is empty", cx, cy + ch - PAD, CT.text_dim);
        return;
    }

    // ── Build flat display list via iterative DFS from root's children ───
    // We store (node_index, depth) pairs on the stack. The root node (index 0)
    // is virtual and never displayed — we start from its children.
    static TreeViewItem hier_items[SCENE_TREE_MAX_NODES];
    int item_count = 0;

    typedef struct { int32_t idx; int32_t depth; bool eff_vis; } StackEntry;
    static StackEntry stack[SCENE_TREE_MAX_NODES];
    int top = 0;

    // Push root's children in reverse sibling order so first child renders first
    {
        // Collect siblings of root's first_child into a temp array
        int32_t rev[256];
        int rev_count = 0;
        int32_t c = scene.tree.nodes[0].first_child;
        while (c >= 0 && rev_count < 256) {
            rev[rev_count++] = c;
            c = scene.tree.nodes[c].next_sibling;
        }
        // Push in reverse so first child is popped first
        for (int i = rev_count - 1; i >= 0; i--) {
            if (top < SCENE_TREE_MAX_NODES) {
                stack[top].idx   = rev[i];
                stack[top].depth = 0;
                stack[top].eff_vis = true;
                top++;
            }
        }
    }

    while (top > 0 && item_count < SCENE_TREE_MAX_NODES) {
        StackEntry e  = stack[--top];   // pop — read BEFORE any push
        int32_t node_idx = e.idx;
        int32_t d        = e.depth;
        bool parent_eff_vis = e.eff_vis;
        SceneNode* node  = &scene.tree.nodes[node_idx];
        bool is_group    = (node->mesh_index < 0);
        bool curr_eff_vis = parent_eff_vis && node->visible;

        TreeViewItem* tv   = &hier_items[item_count++];
        tv->name           = node->name;
        tv->type           = is_group ? TREE_ITEM_GROUP : TREE_ITEM_FILE;
        tv->depth          = d;
        tv->expanded       = node->expanded;
        tv->selected       = (!is_group &&
                              node->mesh_index == editor.inspector.selected_mesh_index);
        tv->icon_expanded  = editor.file_manager.icon_arrow_down;
        tv->icon_collapsed = editor.file_manager.icon_arrow_right;

        if (is_group) {
            tv->icon_leaf = (node->parent == 0) ? s_icon_world : editor.file_manager.icon_folder;
        } else {
            tv->icon_leaf = s_icon_mesh;
        }

        tv->icon_tint      = is_group ? CT.accent : (Color){1.0f, 1.0f, 1.0f, 1.0f};
        tv->show_dot       = false;
        tv->has_visibility = true;
        tv->is_visible     = node->visible;
        tv->effective_visible = curr_eff_vis;
        tv->icon_visible   = s_icon_visible;
        tv->icon_hidden    = s_icon_hidden;
        tv->user_data      = node;

        if (!is_group && node->mesh_index >= 0 &&
            node->mesh_index < (int)scene.meshes.count) {
            Mesh* m = &scene.meshes.items[node->mesh_index];
            tv->dot_color = (m->alpha_mode == 0) ? CT.success
                          : (m->alpha_mode == 1) ? CT.error
                          :                        CT.warning;
        } else {
            tv->dot_color = CT.accent;
        }

        // Push children in reverse sibling order when this node is expanded
        if (node->expanded && node->first_child >= 0) {
            int32_t children[256];
            int child_count = 0;
            int32_t cc = node->first_child;
            while (cc >= 0 && child_count < 256) {
                children[child_count++] = cc;
                cc = scene.tree.nodes[cc].next_sibling;
            }
            for (int i = child_count - 1; i >= 0; i--) {
                if (top < SCENE_TREE_MAX_NODES) {
                    stack[top].idx   = children[i];
                    stack[top].depth = d + 1;
                    stack[top].eff_vis = curr_eff_vis;
                    top++;
                }
            }
        }
    }

    // Sync selected index into hier state
    s_hier_tree_state.selected_index = -1;
    for (int i = 0; i < item_count; i++) {
        if (hier_items[i].selected) {
            s_hier_tree_state.selected_index = i;
            break;
        }
    }

    static const TreeViewCallbacks hier_cb = {
        .on_select        = hier_on_select,
        .on_toggle_expand = hier_on_toggle_expand,
        .on_toggle_visibility = hier_on_toggle_visibility,
    };

    tree_view_render(
            &s_hier_tree_state,
            hier_items, item_count,
            cx, cy, cw, ch,
            cy + ch,
            editor.font,
            s_ui_mx, s_ui_my,
            s_ui_mdown,
            s_ui_mclicked,
            NULL,
            &hier_cb
    );

    if (s_hier_needs_scroll) {
        s_hier_needs_scroll = false;
        for (int i = 0; i < item_count; i++) {
            if (hier_items[i].selected) {
                float lh = editor.font->ascent - editor.font->descent + 11.0f;
                float target_y_pos = i * lh;
                float view_h = ch;
                float scroll_y = s_hier_tree_state.scroll_target;

                if (target_y_pos < scroll_y || target_y_pos > scroll_y + view_h - lh) {
                    s_hier_tree_state.scroll_start  = s_hier_tree_state.scroll_y;
                    s_hier_tree_state.scroll_target = target_y_pos - (view_h * 0.5f);
                    if (s_hier_tree_state.scroll_target < 0.0f) s_hier_tree_state.scroll_target = 0.0f;
                    s_hier_tree_state.scroll_t = 0.0f;
                }
                break;
            }
        }
    }
}

static void render_file_manager(Panel* panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);
    text(editor.font, "— bottom panel coming soon —", cx, cy + ch - PAD, CT.text_dim);
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

static void open_animation_editor(void) {
    int idx = editor.inspector.selected_mesh_index;
    if (idx < 0) { message(MSG_WARNING, "Select a mesh first"); return; }
    if (!anim_editor_open(idx)) { message(MSG_WARNING, "No animations on this mesh"); return; }
    editor_open_panel(PANEL_BOTTOM);
}
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
        s_fs_tree_state.selected_index = s->selected_index;
        float lh = editor.font->ascent - editor.font->descent + LH_EXTRA + 6.0f;
        float target_y_pos = s->selected_index * lh;
        float ch = editor.panels[PANEL_RIGHT].size * editor.inspector_fs_split - TITLE_H - PAD;
        s_fs_tree_state.scroll_start  = s_fs_tree_state.scroll_y;
        s_fs_tree_state.scroll_target = target_y_pos - (ch * 0.5f);
        if (s_fs_tree_state.scroll_target < 0.0f) s_fs_tree_state.scroll_target = 0.0f;
        s_fs_tree_state.scroll_t = 0.0f;
        s_search_anim_t = 0.0f;
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

void editor_search_cursor_left(void) {
    if (s_search_cursor > 0) s_search_cursor--;
    s_search_last_key_time = glfwGetTime();
}

void editor_search_cursor_right(void) {
    if (s_search_cursor < (int)strlen(s_search_query)) s_search_cursor++;
    s_search_last_key_time = glfwGetTime();
}

void editor_search_cursor_start(void) {
    s_search_cursor = 0;
    s_search_last_key_time = glfwGetTime();
}

void editor_search_cursor_end(void) {
    s_search_cursor = strlen(s_search_query);
    s_search_last_key_time = glfwGetTime();
}

void editor_search_delete_char(void) {
    int len = strlen(s_search_query);
    if (s_search_cursor < len) {
        memmove(&s_search_query[s_search_cursor], &s_search_query[s_search_cursor+1], len - s_search_cursor);
        search_execute();
        s_search_last_key_time = glfwGetTime();
    }
}

void editor_search_kill_line(void) {
    if (s_search_cursor < (int)strlen(s_search_query)) {
        s_search_query[s_search_cursor] = '\0';
        search_execute();
        s_search_last_key_time = glfwGetTime();
    }
}

static void search_start(void) {
    FileManagerState* s = &editor.file_manager;
    if (s_is_searching) return;

    if (!editor.panels[PANEL_RIGHT].open) {
        editor_open_panel(PANEL_RIGHT);
    }

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
    s_saved_scroll_y      = s_fs_tree_state.scroll_y;
    s_saved_scroll_target = s_fs_tree_state.scroll_target;
    s_fs_tree_state.scroll_start = s_fs_tree_state.scroll_y;
    s_fs_tree_state.scroll_t     = 1.0f;

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

    s_fs_tree_state.scroll_y      = s_saved_scroll_y;
    s_fs_tree_state.scroll_target = s_saved_scroll_target;
    s_fs_tree_state.scroll_start  = s_saved_scroll_y;
    s_fs_tree_state.scroll_t      = 1.0f;

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
        message(MSG_WARNING, "No mesh selected! Cannot pick color.");
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

    // ── Bottom — Text Editor ──────────────────────────────────────────────
    editor.panels[PANEL_BOTTOM] = (Panel){
        .side           = PANEL_BOTTOM,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 280.0f,
        .min_size       = 140.0f,
        .max_size       = 600.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Text Editor",
        .render_content = render_text_editor_panel,
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

    // ── Top — Vertico ─────────────────────────────────────────────────────
    editor.panels[PANEL_TOP] = (Panel){
        .side           = PANEL_TOP,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 380.0f,
        .min_size       = 100.0f,
        .max_size       = 800.0f,
        .ease_fn        = ease_quart_out,
        .title          = "Console",
        .render_content = render_vertico_panel,
    };

    // ── Left — Hierarchy ──────────────────────────────────────────────────
    editor.panels[PANEL_LEFT] = (Panel){
        .side           = PANEL_LEFT,
        .open           = false,
        .t              = 0.0f,
        .target_t       = 0.0f,
        .size           = 320.0f,
        .min_size       = 200.0f,
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
    s_icon_mesh = texture_pool_add_svg(&context, "./assets/icons/MeshItem.svg", 16, 16);
    s_icon_world = texture_pool_add_svg(&context, "./assets/icons/WorldEnvironment.svg", 16, 16);
    s_icon_visible = texture_pool_add_svg(&context, "./assets/icons/GuiVisibilityVisible.svg", 16, 16);
    s_icon_hidden = texture_pool_add_svg(&context, "./assets/icons/GuiVisibilityHidden.svg", 16, 16);
    s_icon_bone = texture_pool_add_svg(&context, "./assets/icons/Bone.svg", 16, 16);

    file_manager_navigate("./assets");

    // ── Font ──────────────────────────────────────────────────────────────
    editor.font = load_font("./assets/fonts/MapleMono-NF-Regular.ttf", 18);

    // ── Keybindings ───────────────────────────────────────────────────────
    /* keychord_bind(&keymap, "M-j", toggle_bottom,        "Toggle file manager",   PRESS); */
    /* keychord_bind(&keymap, "M-k", toggle_top,            "Toggle Vertico",        PRESS); */
    keychord_bind(&keymap, "M-l", toggle_right,          "Toggle Inspector",      PRESS);
    keychord_bind(&keymap, "M-h", toggle_left,           "Toggle Hierarchy",      PRESS);
    keychord_bind(&keymap, "C-s", search_start,          "Start search",          PRESS);
    keychord_bind(&keymap, "a",   open_animation_editor, "Open Animation Editor", PRESS);
    keychord_bind(&keymap, "c",   cb_open_color_picker,  "Pick Mesh Color",       PRESS);

    keymap_init(&editor_search_keymap);
    keychord_bind(&editor_search_keymap, "C-g", search_cancel, "Cancel search", PRESS);
    keychord_bind(&editor_search_keymap, "RET", search_commit, "Commit search", PRESS);
    keychord_bind(&editor_search_keymap, "C-n", search_next, "Next search match", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "C-p", search_prev, "Prev search match", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "DEL", editor_search_backspace, "Search backspace", PRESS | REPEAT);

    keychord_bind(&editor_search_keymap, "C-b", editor_search_cursor_left, "Cursor left", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "C-f", editor_search_cursor_right, "Cursor right", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "C-a", editor_search_cursor_start, "Cursor start", PRESS);
    keychord_bind(&editor_search_keymap, "C-e", editor_search_cursor_end, "Cursor end", PRESS);
    keychord_bind(&editor_search_keymap, "C-d", editor_search_delete_char, "Delete char", PRESS | REPEAT);
    keychord_bind(&editor_search_keymap, "C-k", editor_search_kill_line, "Kill line", PRESS);

    colorpicker_init(&s_color_picker);
    image_viewer_init(&s_image_viewer);
    tree_view_state_init(&s_fs_tree_state);
    tree_view_state_init(&s_hier_tree_state);
    tree_view_state_init(&s_bone_tree_state);

    editor.last_time   = glfwGetTime();
    editor.initialized = true;

    text_editor_init();
    anim_editor_init();

    fprintf(stdout, "[Editor] Initialized — easeOutExpo panels, MapleMono 18px\n");
}

void editor_cleanup(void) {
    if (!editor.initialized) return;
    text_editor_cleanup();
    anim_editor_cleanup();
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
    s_ui_dt = dt;

    if (s_ui_active_id == &editor.panels[PANEL_BOTTOM].size) {
        float sh = (float)context.swapChainExtent.height;
        float delta = (float)s_ui_my - s_ui_drag_start_y;
        float new_size = s_ui_drag_start_val + delta;
        if (new_size < TEXT_EDITOR_TITLE_H) new_size = TEXT_EDITOR_TITLE_H;
        if (new_size > sh - TITLE_H) new_size = sh - TITLE_H;
        editor.panels[PANEL_BOTTOM].size = new_size;
    } else if (s_bottom_anim_t < 1.0f) {
        s_bottom_anim_t += 5.0f * dt;
        if (s_bottom_anim_t >= 1.0f) {
            s_bottom_anim_t = 1.0f;
            editor.panels[PANEL_BOTTOM].size = s_bottom_target_size;
        } else {
            editor.panels[PANEL_BOTTOM].size = s_bottom_start_size + (s_bottom_target_size - s_bottom_start_size) * ease_quart_in_out(s_bottom_anim_t);
        }
    }

    if (s_color_picker.visible) {
        colorpicker_update(&s_color_picker, dt, s_ui_mx, s_ui_my);
    }

    if (s_image_viewer.visible) {
            image_viewer_update(&s_image_viewer, dt, s_ui_mx, s_ui_my);
        }

        float sh = (float)context.swapChainExtent.height;
        text_editor_update(dt, s_ui_mx, sh - s_ui_my);

        if (s_is_searching) {
        if (s_search_anim_t < 1.0f) {
            s_search_anim_t += 3.0f * dt;
        }
    }

    tree_view_update_scroll(&s_fs_tree_state, dt);
    tree_view_update_scroll(&s_hier_tree_state, dt);
    anim_editor_update(dt, s_ui_mx, s_ui_my);

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

    // Explicit Z-order: render BOTTOM panel last so it sits on top of the side panels
    int render_order[] = { PANEL_TOP, PANEL_LEFT, PANEL_RIGHT, PANEL_BOTTOM };
    for (int i = 0; i < PANEL_COUNT; i++) {
        Panel* p = &editor.panels[render_order[i]];

        // Skip fully-closed, non-animating panels
        if (p->t < 0.0005f && p->target_t <= 0.0f) continue;

        float x, y, w, h;
        panel_get_rect(p, &x, &y, &w, &h);

        panel_draw_bg(p, x, y, w, h);

        if (p->render_content)
            p->render_content(p, x, y, w, h);

        panel_draw_titlebar(p, x, y, w, h);
    }

    colorpicker_render(&s_color_picker, editor.font);
    image_viewer_render(&s_image_viewer, editor.font);
    text_editor_render(editor.font);

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
    editor_selected_bone = NULL;
    s_bone_tree_state.selected_index = -1;

    for (int i = 0; i < scene.tree.count; i++) {
        if (scene.tree.nodes[i].mesh_index == index) {
            int p = scene.tree.nodes[i].parent;
            while (p >= 0) {
                scene.tree.nodes[p].expanded = true;
                p = scene.tree.nodes[p].parent;
            }
            break;
        }
    }
    s_hier_needs_scroll = true;
}

void inspector_deselect(void) {
    editor.inspector.selected_mesh_index = -1;
    editor.hierarchy.selected_index      = -1;
    gizmo.active = false;
    editor_selected_bone = NULL;
    s_bone_tree_state.selected_index = -1;
}

/// Hierarchy API

void hierarchy_select(int index) {
    editor.hierarchy.selected_index = index;
    if (index >= 0 && index < (int)scene.meshes.count) {
        editor.inspector.selected_mesh_index = index;
        gizmo.active = true;

        for (int i = 0; i < scene.tree.count; i++) {
            if (scene.tree.nodes[i].mesh_index == index) {
                int p = scene.tree.nodes[i].parent;
                while (p >= 0) {
                    scene.tree.nodes[p].expanded = true;
                    p = scene.tree.nodes[p].parent;
                }
                break;
            }
        }
        s_hier_needs_scroll = true;
    } else {
        editor.inspector.selected_mesh_index = -1;
        gizmo.active = false;
    }
}
