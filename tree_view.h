#pragma once
#include "renderer.h"
#include "font.h"
#include "theme.h"

#define TREE_VIEW_MAX_ITEMS 2048

typedef enum {
    TREE_ITEM_FILE,
    TREE_ITEM_DIR,
    TREE_ITEM_GROUP
} TreeItemType;

typedef struct {
    const char* name;
    TreeItemType type;
    int          tag;  // Generic integer tag for custom classification
    int          depth;
    bool         expanded;
    bool         selected;
    int32_t      icon_expanded;
    int32_t      icon_collapsed;
    int32_t      icon_leaf;
    Color        icon_tint;
    Color        dot_color;
    bool         show_dot;
    bool         has_visibility;
    bool         is_visible;
    bool         effective_visible;
    int32_t      icon_visible;
    int32_t      icon_hidden;
    void* user_data;
} TreeViewItem;

typedef struct {
    void (*on_select)(int item_index, const TreeViewItem* item);
    void (*on_toggle_expand)(int item_index, const TreeViewItem* item);
    void (*on_toggle_visibility)(int item_index, const TreeViewItem* item);
} TreeViewCallbacks;

typedef struct {
    float scroll_y;
    float scroll_target;
    float scroll_start;
    float scroll_t;
    int   selected_index;
} TreeViewState;

typedef struct {
    bool  active;
    int   current_match_index;
    int   prev_match_index;
    float anim_t;
} TreeViewSearchOverlay;

void tree_view_state_init(TreeViewState* s);
void tree_view_update_scroll(TreeViewState* s, float dt);
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
    const TreeViewCallbacks*     cb
);
