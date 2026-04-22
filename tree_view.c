#include "tree_view.h"
#include "easing.h"
#include <string.h>
#include <math.h>

void tree_view_state_init(TreeViewState* s) {
    memset(s, 0, sizeof(TreeViewState));
    s->selected_index = -1;
    s->scroll_t       = 1.0f;
}

void tree_view_update_scroll(TreeViewState* s, float dt) {
    if (s->scroll_t < 1.0f) {
        s->scroll_t += 3.0f * dt;
        if (s->scroll_t >= 1.0f) {
            s->scroll_t = 1.0f;
            s->scroll_y = s->scroll_target;
        } else {
            s->scroll_y = s->scroll_start +
                (s->scroll_target - s->scroll_start) *
                ease_cubic_out(s->scroll_t);
        }
    }
}

void tree_view_render(
    TreeViewState*               state,
    const TreeViewItem*          items,
    int                          count,
    float                        cx,
    float                        cy,
    float                        cw,
    float                        ch,
    float                        tab_y,
    Font*                        font,
    double                       mx,
    double                       my,
    bool                         mdown,
    bool                         clicked,
    const TreeViewSearchOverlay* search,
    const TreeViewCallbacks*     cb)
{
    (void)ch;
    if (!font || count == 0) return;

    float lh     = font->ascent - font->descent + 5.0f + 6.0f;
    float list_y = tab_y - 14.0f + state->scroll_y;

    // ── Build selected-path ancestor table ───────────────────────────────
    int selected_ancestors[64];
    for (int i = 0; i < 64; i++) selected_ancestors[i] = -1;

    if (state->selected_index >= 0 && state->selected_index < count) {
        int trace_idx = state->selected_index;
        while (trace_idx >= 0) {
            int d = items[trace_idx].depth;
            if (d < 64) selected_ancestors[d] = trace_idx;
            if (d == 0) break;
            int parent = -1;
            for (int p = trace_idx - 1; p >= 0; p--) {
                if (items[p].depth < d) { parent = p; break; }
            }
            trace_idx = parent;
        }
    }

    bool searching = (search && search->active);

    int current_ancestors[64];
    for (int i = 0; i < 64; i++) current_ancestors[i] = -1;

    for (int i = 0; i < count; i++) {
        const TreeViewItem* item = &items[i];
        if (list_y - lh < cy) break;

        int   D      = item->depth;
        if (D < 64) current_ancestors[D] = i;

        float indent = D * 16.0f;
        float row_x  = cx + indent;

        bool hovered  = (mx >= cx && mx <= cx + cw &&
                         my >= list_y - lh && my <= list_y);
        bool selected = (state->selected_index == i) || item->selected;

        float sel_x = cx + D * 16.0f + 14.0f;
        float sel_w = cw - (sel_x - cx) - lh - 4.0f;

        // is_visible: don't draw above the title bar
        bool is_visible = (list_y <= tab_y + 4.0f);

        bool row_hovered = hovered;
        if (item->has_visibility && mx >= cx + cw - lh) {
            row_hovered = false;
        }

        if (is_visible) {
            if (selected) {
                exQuad2D((vec2){sel_x, list_y - lh}, (vec2){sel_w, lh},
                         (vec4){4,4,4,4}, 0.0f, CT.fs_hovered, CT.fs_hovered);
            } else if (row_hovered) {
                exQuad2D((vec2){sel_x, list_y - lh}, (vec2){sel_w, lh},
                         (vec4){4,4,4,4}, 0.0f, CT.fs_selected, CT.fs_selected);
            }
        }

        // ── Input ────────────────────────────────────────────────────────
        if (hovered && clicked && is_visible && cb) {
            float vis_size = lh;
            float vis_x = cx + cw - vis_size;
            if (item->has_visibility && mx >= vis_x && mx <= vis_x + vis_size) {
                if (cb->on_toggle_visibility)
                    cb->on_toggle_visibility(i, item);
            } else if ((item->type == TREE_ITEM_DIR || item->type == TREE_ITEM_GROUP) &&
                mx < row_x + 18.0f) {
                if (cb->on_toggle_expand)
                    cb->on_toggle_expand(i, item);
            } else {
                if (cb->on_select)
                    cb->on_select(i, item);
            }
        }

        // ── Search beam anim_y ───────────────────────────────────────────
        // Exactly matches the original: lerp prev→current match index,
        // shift down half a line so the beam intersects the spur geometry.
        float anim_y = -9999.0f;
        if (searching && search->current_match_index >= 0) {
            float t = ease_cubic_out(clampf(search->anim_t, 0.0f, 1.0f));
            float beam_index = search->prev_match_index +
                (search->current_match_index - search->prev_match_index) * t;
            anim_y = tab_y - 14.0f + state->scroll_y
                     - (beam_index * lh) - (lh * 0.5f);
        }

        // ── Godot-style tree lines + accent beam overlay ─────────────────
        for (int d = 0; d < D; d++) {
            float line_x = cx + d * 16.0f + 8.0f;

            bool parent_continues = false;
            for (int j = i + 1; j < count; j++) {
                if (items[j].depth <= d) break;
                if (items[j].depth == d + 1) { parent_continues = true; break; }
            }

            Color upper_col = CT.fs_tree_dimmed;
            Color lower_col = CT.fs_tree_dimmed;
            Color spur_col  = CT.fs_tree_dimmed;

            bool in_selected_folder = (current_ancestors[d] == selected_ancestors[d]);
            int  target             = selected_ancestors[d + 1];

            // Bright selected-path lines — suppressed during search
            // (the accent beam overlay takes over instead)
            if (in_selected_folder && target != -1 && !searching) {
                bool is_final_folder = (target == state->selected_index);

                if (i < target) {
                    upper_col = CT.fs_tree;
                    lower_col = CT.fs_tree;
                    if (is_final_folder) spur_col = (Color){0, 0, 0, 0};
                } else if (i == target) {
                    upper_col = CT.fs_tree;
                    spur_col  = CT.fs_tree;
                    if (is_final_folder) lower_col = (Color){0, 0, 0, 0};
                } else {
                    if (is_final_folder) {
                        upper_col = (Color){0, 0, 0, 0};
                        lower_col = (Color){0, 0, 0, 0};
                        spur_col  = (Color){0, 0, 0, 0};
                    }
                }
            }

            if (is_visible) {
                // Base dimmed/bright tree lines
                if (d == D - 1) {
                    if (spur_col.a > 0.0f)
                        quad2D((vec2){line_x, list_y - lh * 0.5f},
                               (vec2){10.0f, 1.0f}, spur_col);
                    if (upper_col.a > 0.0f)
                        quad2D((vec2){line_x, list_y - lh * 0.5f},
                               (vec2){1.0f, lh * 0.5f}, upper_col);
                    if (parent_continues && lower_col.a > 0.0f)
                        quad2D((vec2){line_x, list_y - lh},
                               (vec2){1.0f, lh * 0.5f}, lower_col);
                } else {
                    if (parent_continues) {
                        if (upper_col.a > 0.0f)
                            quad2D((vec2){line_x, list_y - lh * 0.5f},
                                   (vec2){1.0f, lh * 0.5f}, upper_col);
                        if (lower_col.a > 0.0f)
                            quad2D((vec2){line_x, list_y - lh},
                                   (vec2){1.0f, lh * 0.5f}, lower_col);
                    }
                }

                // ── Accent beam overlay (search mode) ────────────────────
                // Exact port of the original OVERLAY ACTIVE SEARCH BEAM block.
                // Draws CT.accent over the tree lines from the beam position
                // upward, animating as the match index changes.
                if (searching && in_selected_folder && target != -1) {
                    float y_top = list_y;
                    float y_mid = list_y - lh * 0.5f;
                    float y_bot = list_y - lh;

                    if (d == D - 1) {
                        if (i < target) {
                            float cy_bot = fmaxf(y_mid, anim_y);
                            if (y_top > cy_bot)
                                quad2D((vec2){line_x, cy_bot},
                                       (vec2){1.0f, y_top - cy_bot}, CT.accent);
                            if (parent_continues) {
                                float c_bot = fmaxf(y_bot, anim_y);
                                if (y_mid > c_bot)
                                    quad2D((vec2){line_x, c_bot},
                                           (vec2){1.0f, y_mid - c_bot}, CT.accent);
                            }
                        } else if (i == target) {
                            float cy_bot = fmaxf(y_mid, anim_y);
                            if (y_top > cy_bot)
                                quad2D((vec2){line_x, cy_bot},
                                       (vec2){1.0f, y_top - cy_bot}, CT.accent);
                            if (y_mid >= anim_y)
                                quad2D((vec2){line_x, y_mid},
                                       (vec2){10.0f, 1.0f}, CT.accent);
                        }
                    } else {
                        if (parent_continues && i < target) {
                            float cy_bot_u = fmaxf(y_mid, anim_y);
                            if (y_top > cy_bot_u)
                                quad2D((vec2){line_x, cy_bot_u},
                                       (vec2){1.0f, y_top - cy_bot_u}, CT.accent);
                            float cy_bot_l = fmaxf(y_bot, anim_y);
                            if (y_mid > cy_bot_l)
                                quad2D((vec2){line_x, cy_bot_l},
                                       (vec2){1.0f, y_mid - cy_bot_l}, CT.accent);
                        }
                    }
                }
            }
        }

        // ── Icons + label ────────────────────────────────────────────────
        if (is_visible) {
            float icon_y    = list_y - lh * 0.5f - 8.0f;
            float current_x = row_x;

            if (item->type == TREE_ITEM_DIR || item->type == TREE_ITEM_GROUP) {
                int32_t arrow_idx = item->expanded
                    ? item->icon_expanded : item->icon_collapsed;
                Texture2D* arrow = texture_pool_get(arrow_idx);
                if (arrow)
                    texture2D((vec2){current_x, icon_y}, (vec2){16, 16},
                              arrow, (Color){1.0f, 1.0f, 1.0f, 1.0f});
                current_x += 18.0f;

                Texture2D* folder = texture_pool_get(item->icon_leaf);
                if (folder)
                    texture2D((vec2){current_x, icon_y}, (vec2){16, 16},
                              folder, item->icon_tint);
                current_x += 20.0f;
            } else {
                current_x += 18.0f;
                if (item->show_dot) {
                    float dot_y = list_y - lh * 0.5f - 3.0f;
                    quad2D((vec2){current_x, dot_y}, (vec2){6.0f, 6.0f},
                           item->dot_color);
                    current_x += 14.0f;
                } else {
                    Texture2D* file_icon = texture_pool_get(item->icon_leaf);
                    if (file_icon)
                        texture2D((vec2){current_x, icon_y}, (vec2){16, 16},
                                  file_icon, (Color){1.0f, 1.0f, 1.0f, 1.0f});
                    current_x += 20.0f;
                }
            }

            Color label_col = selected ? CT.text : CT.text_dim;

            char display_name[128];
            strncpy(display_name, item->name, sizeof(display_name) - 1);
            display_name[sizeof(display_name) - 1] = '\0';

            float vis_size = lh;
            float max_w = (item->has_visibility ? (cx + cw - vis_size) : (cx + cw)) - current_x - 8.0f;
            if (measure_text_width(font, display_name, 1.0f) > max_w) {
                int len = strlen(display_name);
                float dots_w = measure_text_width(font, "..", 1.0f);
                while (len > 0 && measure_text_width(font, display_name, 1.0f) > max_w - dots_w) {
                    display_name[--len] = '\0';
                }
                strcat(display_name, "...");
            }

            text(font, display_name, current_x,
                 list_y - lh * 0.5f - 2.0f, label_col);

            if (item->has_visibility) {
                float vis_x = cx + cw - vis_size;
                bool vis_hovered = (mx >= vis_x && mx <= vis_x + vis_size && my >= list_y - lh && my <= list_y);
                if (vis_hovered) {
                    Color bg_col = mdown ? CT.button_pressed : CT.button;
                    exQuad2D((vec2){vis_x, list_y - lh}, (vec2){vis_size, vis_size}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, bg_col, bg_col);
                }
                int32_t vis_icon = item->is_visible ? item->icon_visible : item->icon_hidden;
                Texture2D* tex = texture_pool_get(vis_icon);
                if (tex) {
                    Color vis_tint = item->effective_visible ? CT.text : CT.shadow;
                    float ix = vis_x + (vis_size - 16.0f) * 0.5f;
                    float iy = list_y - lh + (vis_size - 16.0f) * 0.5f;
                    texture2D((vec2){ix, iy}, (vec2){16, 16}, tex, vis_tint);
                }
            }
        }

        list_y -= lh;

        if (list_y < cy - lh) break;
    }
}
