#pragma once
#include "renderer.h"
#include "font.h"
#include "context.h"
#include "keychords.h"
#include "scene.h"
#include "common.h"
#include "easing.h"
#include <stdbool.h>
#include <stddef.h>

///  Panel System

typedef enum {
    PANEL_BOTTOM = 0,   // File Manager
    PANEL_RIGHT  = 1,   // Inspector
    PANEL_TOP    = 2,   // Console
    PANEL_LEFT   = 3,   // Hierarchy
    PANEL_COUNT  = 4
} PanelSide;

typedef struct Panel Panel;
struct Panel {
    PanelSide   side;
    bool        open;
    float       t;           // Current animation progress [0.0 … 1.0]
    float       target_t;    // 0 = fully closed, 1 = fully open
    float       size;        // Thickness in pixels (height for TOP/BOTTOM, width for LEFT/RIGHT)
    float       min_size;
    float       max_size;
    EaseFn      ease_fn;
    const char* title;
    void        (*render_content)(struct Panel* panel, float x, float y, float w, float h);
    void*       user_data;
};

///  Sub-states

typedef struct {
    int selected_mesh_index;   // -1 = nothing selected
    bool show_aabb;
    bool aabb_full_lines;
    bool aabb_use_mesh_color;
    float aabb_color[4];
} InspectorState;

typedef struct {
    int  selected_index;       // -1 = nothing selected
    bool show_hidden;
} HierarchyState;

#define FILE_MANAGER_MAX_PATH   512
#define FILE_MANAGER_MAX_ITEMS  1024

typedef enum {
    FILE_ITEM_DIR  = 0,
    FILE_ITEM_FILE = 1,
} FileItemType;

typedef struct {
    char          name[256];
    char          full_path[FILE_MANAGER_MAX_PATH];
    FileItemType  type;
    size_t        size_bytes;
    int           depth;     // Tree indentation level
    bool          expanded;  // Is this folder open?
} FileItem;

typedef struct {
    char      current_path[FILE_MANAGER_MAX_PATH];
    FileItem  items[FILE_MANAGER_MAX_ITEMS];
    int       item_count;
    int       selected_index;
    int       scroll_offset;
    int       visible_rows;

    char      expanded_paths[64][FILE_MANAGER_MAX_PATH]; // Track which folders are open
    int       expanded_count;

    // SVG bindless texture slots
    int32_t   icon_folder;
    int32_t   icon_file;
    int32_t   icon_arrow_right;
    int32_t   icon_arrow_down;
} FileManagerState;

/// Editor (top-level aggregate)

typedef struct {
    Panel            panels[PANEL_COUNT];
    InspectorState   inspector;
    HierarchyState   hierarchy;
    FileManagerState file_manager;
    Font* font;
    bool             initialized;
    double           last_time;
    float            inspector_fs_split; // Dynamic layout splitter [0.0 - 1.0]
    float            fs_split_target;
    float            fs_split_start;
    float            fs_split_saved;
    float            fs_anim_t;
    bool             fs_collapsed;
    double           last_tab_click_time;
} Editor;

extern Editor editor;

/// Message

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

void message(MessageType type, const char* text);

/// Geometry Menu

typedef enum {
    ADJUST_PRIMITIVE_NONE,
    ADJUST_PRIMITIVE_TORUS
} AdjustPrimitiveType;

typedef struct {
    bool active;
    AdjustPrimitiveType type;
    int mesh_index;

    int torus_major_segs;
    int torus_minor_segs;
    float torus_major_rad;
    float torus_minor_rad;

    vec3 position;
    vec3 rotation;
} AdjustPrimitiveState;

extern AdjustPrimitiveState adjust_state;

/// Lifecycle

void editor_init(void);
void editor_cleanup(void);
void editor_update(void);    // Once per frame, before editor_render
void editor_render(void);    // After all 3-D rendering, before present

/// Panel control

void  editor_toggle_panel(PanelSide side);
void  editor_open_panel(PanelSide side);
void  editor_close_panel(PanelSide side);
bool  editor_panel_is_open(PanelSide side);

/// Inspector API

void inspector_select_mesh(int index);
void inspector_deselect(void);

/// Hierarchy API

void hierarchy_select(int index);

/// File Manager API

bool file_manager_navigate(const char* path);
void file_manager_navigate_up(void);
