#include "editor.h"
#include "renderer.h"
#include "context.h"
#include "scene.h"
#include "font.h"
#include "keychords.h"
#include "theme.h"
#include "gizmo.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <GLFW/glfw3.h>

///  Globals

Editor editor = {0};

// Animation constants
#define EDITOR_MAX_DT      0.05f   // cap dt to avoid jumps after focus loss
#define EDITOR_ANIM_SPEED  12.0f   // how fast t chases target_t

#define PANEL_RADIUS   8.0f  // Corner radius of panels
#define TITLE_H       32.0f  // Title-bar height
#define PAD           14.0f  // Inner padding
#define LH_EXTRA       5.0f  // Line height multiplier


///  Math helpers

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

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

static void field_vec3(float cx, float row, float col2_x, float cw,
                       const char* label, vec3 v, const char* unit) {
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);

    float box_x = col2_x;
    float box_w = (cx + cw) - box_x;
    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    float sec_w = box_w / 3.0f;
    Color axis_colors[3] = {CT.x_dark, CT.y_dark, CT.z_dark};
    const char* axis_labels[3] = {"X", "Y", "Z"};

    for (int i = 0; i < 3; i++) {
        float px = box_x + i * sec_w + 6.0f;
        text(editor.font, axis_labels[i], px, row, axis_colors[i]);

        char num[32];
        // The space flag '% .2f' perfectly aligns positive and negative numbers!
        snprintf(num, sizeof(num), "% .2f", v[i]);
        float num_x = px + 14.0f;
        text(editor.font, num, num_x, row, CT.text);

        if (unit) {
            // Fixed pixel offset perfectly distances the unit from the number
            float unit_x = num_x + 58.0f;
            text(editor.font, unit, unit_x, row, CT.text_dim);
        }
    }
}

static void field_vec4_color(float cx, float row, float col2_x, float cw,
                             const char* label, vec4 c) {
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);

    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    float swatch_size = box_h - 4.0f;
    // Shift swatch and box to the left to pull them closer to the label
    float swatch_x = col2_x - 20.0f;
    Color actual_color = {c[0], c[1], c[2], c[3]};
    exQuad2D((vec2){swatch_x, box_y + 2.0f}, (vec2){swatch_size, swatch_size}, (vec4){3.0f, 3.0f, 3.0f, 3.0f}, 0.0f, actual_color, actual_color);

    float box_x = swatch_x + swatch_size + 8.0f;
    float box_w = (cx + cw) - box_x + 8.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    float sec_w = box_w / 4.0f;
    Color axis_colors[4] = {CT.x_dark, CT.y_dark, CT.z_dark, CT.text_dim};
    const char* axis_labels[4] = {"R", "G", "B", "A"};

    for (int i = 0; i < 4; i++) {
        // Pushed the starting point slightly right to center the whole block in the sector
        float px = box_x + i * sec_w + 8.0f;
        text(editor.font, axis_labels[i], px, row, axis_colors[i]);

        char num[32];
        snprintf(num, sizeof(num), "%.2f", c[i]);
        // Increased the offset from 12.0f to 18.0f to give the number more breathing room
        // from its label and reduce the empty space before the next letter
        text(editor.font, num, px + 18.0f, row, CT.text);
    }
}

static void field_float(float cx, float row, float col2_x, float cw,
                        const char* label, float val) {
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);

    float box_x = col2_x;
    float box_w = 80.0f; // Compact width for single float fields
    float box_h = editor.font->ascent - editor.font->descent + 6.0f;
    float box_y = row - editor.font->descent - 3.0f;

    exQuad2D((vec2){box_x, box_y}, (vec2){box_w, box_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.bg_deep, CT.bg_deep);

    char num[32];
    snprintf(num, sizeof(num), "% .3f", val); // Aligned spacing
    text(editor.font, num, box_x + 8.0f, row, CT.text);
}

static void field_text(float cx, float row, float col2_x, float cw,
                       const char* label, const char* value) {
    if (!editor.font) return;
    text(editor.font, label, cx, row, CT.text_dim);

    // Read-only text fields (like Geometry) no longer have heavy backgrounds!
    text(editor.font, value, col2_x + 8.0f, row, CT.text);
}

static void render_inspector(Panel* panel,
                              float px, float py, float pw, float ph) {
    if (!editor.font) return;

    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    InspectorState* s = &editor.inspector;

    if (s->selected_mesh_index < 0 ||
        s->selected_mesh_index >= (int)scene.meshes.count) {
        text(editor.font, "Nothing selected", cx, cy + ch - PAD, CT.text_dim);
        return;
    }

    Mesh* m = &scene.meshes.items[s->selected_mesh_index];
    float lh  = editor.font->ascent - editor.font->descent + LH_EXTRA;
    float col2 = cx + 120.0f; // Vector fields stay perfectly aligned to 120

    extern float font_width(Font* font);
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
    field_vec3(cx, row, col2, cw, "Position", pos, "m");
    row -= lh + 6.0f;

    vec3 euler;
    glm_euler_angles(m->model, euler);
    vec3 rot_deg = {glm_deg(euler[0]), glm_deg(euler[1]), glm_deg(euler[2])};
    field_vec3(cx, row, col2, cw, "Rotation", rot_deg, "°");
    row -= lh + 6.0f;

    vec3 scale = {
        glm_vec3_norm((vec3){m->model[0][0], m->model[1][0], m->model[2][0]}),
        glm_vec3_norm((vec3){m->model[0][1], m->model[1][1], m->model[2][1]}),
        glm_vec3_norm((vec3){m->model[0][2], m->model[1][2], m->model[2][2]})
    };
    field_vec3(cx, row, col2, cw, "Scale", scale, "m");
    row -= lh + 12.0f;

    // ── Material ───────────────────────────────────────────────────────────
    SECTION("Material");

    field_vec4_color(cx, row, col2, cw, "Color", m->baseColorFactor);
    row -= lh + 6.0f;

    vec4 emissive = {m->emissiveFactor[0], m->emissiveFactor[1], m->emissiveFactor[2], 1.0f};
    field_vec4_color(cx, row, col2, cw, "Emissive", emissive);
    row -= lh + 6.0f;

    field_float(cx, row, col2_mat, cw, "Emissive Strength", m->emissiveStrength);
    row -= lh + 6.0f;

    field_float(cx, row, col2_mat, cw, "Metallic", m->metallicFactor);
    row -= lh + 6.0f;

    field_float(cx, row, col2_mat, cw, "Roughness", m->roughnessFactor);
    row -= lh + 6.0f;

    field_float(cx, row, col2_mat, cw, "Transmission", m->transmissionFactor);
    row -= lh + 6.0f;

    field_float(cx, row, col2_mat, cw, "IOR", m->ior);
    row -= lh + 12.0f;

    // ── Geometry ───────────────────────────────────────────────────────────
    SECTION("Geometry");

    char buf[64];
    snprintf(buf, sizeof buf, "%u", m->vertexCount);
    field_text(cx, row, col2_geo, cw, "Vertices", buf);
    row -= lh + 6.0f;

    snprintf(buf, sizeof buf, "%u", m->indexCount);
    field_text(cx, row, col2_geo, cw, "Indices", buf);
    row -= lh + 6.0f;

    const char* amode = (m->alpha_mode == 0) ? "Opaque" : (m->alpha_mode == 1) ? "Mask" : "Blend";
    field_text(cx, row, col2_geo, cw, "Alpha Mode", amode);
    row -= lh + 6.0f;

    field_text(cx, row, col2_geo, cw, "Unlit", m->is_unlit ? "Yes" : "No");
    row -= lh + 6.0f;

    if (m->name) {
        field_text(cx, row, col2_geo, cw, "Name", m->name);
        row -= lh + 6.0f;
    }

    // ── Bounds ─────────────────────────────────────────────────────────────
    SECTION("Bounds");
    vec3 bmin = {m->aabbMin[0], m->aabbMin[1], m->aabbMin[2]};
    vec3 bmax = {m->aabbMax[0], m->aabbMax[1], m->aabbMax[2]};
    field_vec3(cx, row, col2, cw, "AABB Min", bmin, "m");
    row -= lh + 6.0f;
    field_vec3(cx, row, col2, cw, "AABB Max", bmax, "m");
    row -= lh + 12.0f;

    // ── Animation ──────────────────────────────────────────────────────────
    if (m->jointCount > 0 || m->morphCount > 0) {
        SECTION("Animation");

        if (m->jointCount > 0) {
            snprintf(buf, sizeof buf, "%d (off %d)", m->jointCount, m->jointOffset);
            field_text(cx, row, col2_anim, cw, "Joints", buf);
            row -= lh + 6.0f;
        }
        if (m->morphCount > 0) {
            snprintf(buf, sizeof buf, "%d", m->morphCount);
            field_text(cx, row, col2_anim, cw, "Morphs", buf);
            row -= lh + 6.0f;

            if (m->morph_data) {
                for (int i = 0; i < m->morphCount && i < 8; i++) {
                    char wlbl[16];
                    snprintf(wlbl, sizeof wlbl, "  [%d]", i);
                    field_float(cx, row, col2_anim, cw, wlbl, m->morph_data->weights[i]);
                    row -= lh + 6.0f;
                }
            }
        }
    }

#undef SECTION
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

static void render_file_manager(Panel* panel,
                                 float px, float py, float pw, float ph) {
    if (!editor.font) return;

    FileManagerState* s = &editor.file_manager;

    float cx, cy, cw, ch;
    content_area(panel, px, py, pw, ph, &cx, &cy, &cw, &ch);

    // Path breadcrumb — drawn at the very top of the content area
    float breadcrumb_y = cy + ch - PAD;
    text(editor.font, s->current_path, cx, breadcrumb_y, CT.text_dim);

    float grid_top = breadcrumb_y - (editor.font->ascent - editor.font->descent) - 10.0f;

    if (s->item_count == 0) {
        text(editor.font, "Empty directory", cx, grid_top, CT.text_dim);
        return;
    }

    // How many tiles fit in a row
    int cols = (int)((cw + FM_TILE_GAP) / (FM_TILE_W + FM_TILE_GAP));
    if (cols < 1) cols = 1;

    // Tiles are laid out left-to-right, top-to-bottom (decreasing y)
    for (int i = 0; i < s->item_count; i++) {
        int col = i % cols;
        int row = i / cols;

        float tx = cx + col * (FM_TILE_W + FM_TILE_GAP);
        float ty = grid_top - row * (FM_TILE_H + FM_TILE_GAP) - FM_TILE_H;

        // Clip tiles that scroll out of the content area
        if (ty + FM_TILE_H < cy) break;
        if (ty > grid_top)       continue;

        FileItem* item     = &s->items[i];
        bool      selected = (i == s->selected_index);

        // Tile background (Borders removed entirely)
        Color tile_bg = selected ? CT.bg_alt : CT.bg_deep;
        exQuad2D((vec2){tx, ty}, (vec2){FM_TILE_W, FM_TILE_H}, (vec4){5.0f, 5.0f, 5.0f, 5.0f}, 0.0f, tile_bg, tile_bg);

        // Name label — truncate to fit tile width
        char display_name[32];
        strncpy(display_name, item->name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
        // simple character truncation (font metrics would be ideal but this is fast)
        const int MAX_CHARS = 14;
        if ((int)strlen(display_name) > MAX_CHARS) {
            display_name[MAX_CHARS - 1] = '\xe2'; // UTF-8 ellipsis …
            display_name[MAX_CHARS]     = '\x80';
            display_name[MAX_CHARS + 1] = '\xa6';
            display_name[MAX_CHARS + 2] = '\0';
        }

        Color name_col = selected ? CT.text : CT.text_dim;
        float name_y   = ty + (FM_TILE_H - (editor.font->ascent - editor.font->descent)) * 0.5f;
        text(editor.font, display_name, tx + 8.0f, name_y, name_col);

        // Size label (files only)
        if (item->type == FILE_ITEM_FILE && item->size_bytes > 0) {
            char sz[16];
            if      (item->size_bytes >= 1024 * 1024)
                snprintf(sz, sizeof sz, "%.1fM", (double)item->size_bytes / (1024.0 * 1024.0));
            else if (item->size_bytes >= 1024)
                snprintf(sz, sizeof sz, "%.0fK", (double)item->size_bytes / 1024.0);
            else
                snprintf(sz, sizeof sz, "%zuB", item->size_bytes);

            text(editor.font, sz,
                 tx + FM_TILE_W - 8.0f - 40.0f,   // right-align approximation
                 ty + 6.0f,
                 CT.text_dim);
        }
    }
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

bool file_manager_navigate(const char* path) {
    FileManagerState* s = &editor.file_manager;

    DIR* dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "[Editor] Cannot open directory: %s\n", path);
        return false;
    }

    strncpy(s->current_path, path, FILE_MANAGER_MAX_PATH - 1);
    s->current_path[FILE_MANAGER_MAX_PATH - 1] = '\0';
    s->item_count     = 0;
    s->selected_index = 0;
    s->scroll_offset  = 0;

    // Always include ".." as first entry
    if (s->item_count < FILE_MANAGER_MAX_ITEMS) {
        FileItem* up = &s->items[s->item_count++];
        strncpy(up->name,      "..",  sizeof(up->name)      - 1);
        strncpy(up->full_path, path,  sizeof(up->full_path) - 1);
        up->type       = FILE_ITEM_DIR;
        up->size_bytes = 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL &&
           s->item_count < FILE_MANAGER_MAX_ITEMS) {
        if (entry->d_name[0] == '.') continue;  // skip hidden + . ..

        FileItem* item = &s->items[s->item_count++];
        strncpy(item->name, entry->d_name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
        snprintf(item->full_path, sizeof(item->full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(item->full_path, &st) == 0) {
            item->type       = S_ISDIR(st.st_mode) ? FILE_ITEM_DIR : FILE_ITEM_FILE;
            item->size_bytes = (size_t)st.st_size;
        } else {
            item->type       = FILE_ITEM_FILE;
            item->size_bytes = 0;
        }
    }
    closedir(dir);
    return true;
}

void file_manager_navigate_up(void) {
    FileManagerState* s = &editor.file_manager;
    char parent[FILE_MANAGER_MAX_PATH];
    strncpy(parent, s->current_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';

    char* last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else if (last_slash == parent) {
        parent[1] = '\0';  // filesystem root "/"
    }
    file_manager_navigate(parent);
}

/// Keybinding callbacks

static void toggle_bottom(void) { editor_toggle_panel(PANEL_BOTTOM); }
static void toggle_right(void)  { editor_toggle_panel(PANEL_RIGHT);  }
static void toggle_top(void)    { editor_toggle_panel(PANEL_TOP);    }
static void toggle_left(void)   { editor_toggle_panel(PANEL_LEFT);   }

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
        .render_content = render_inspector,
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
