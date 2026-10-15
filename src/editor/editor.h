#pragma once
#include "common.h"
#include "colorpicker.h"
#include "context.h"
#include "easing.h"
#include "font.h"
#include "keychords.h"
#include "renderer.h"
#include "scene.h"
#include <stdbool.h>
#include <stddef.h>

///  Panel System

typedef enum {
  PANEL_BOTTOM = 0, // File Manager
  PANEL_RIGHT = 1,  // Inspector
  PANEL_TOP = 2,    // Console
  PANEL_LEFT = 3,   // Hierarchy
  PANEL_COUNT = 4
} PanelSide;

typedef enum {
  EDITOR_TRANSFORM_TRANSLATE = 0,
  EDITOR_TRANSFORM_ROTATE = 1
} EditorTransformMode;

typedef enum {
  EDITOR_TRANSFORM_AXIS_VIEW = 0,
  EDITOR_TRANSFORM_AXIS_X = 1,
  EDITOR_TRANSFORM_AXIS_Y = 2,
  EDITOR_TRANSFORM_AXIS_Z = 3
} EditorTransformAxis;

typedef struct Panel Panel;
struct Panel {
  PanelSide side;
  bool open;
  float t;        // Current animation progress [0.0 … 1.0]
  float target_t; // 0 = fully closed, 1 = fully open
  float
      size; // Thickness in pixels (height for TOP/BOTTOM, width for LEFT/RIGHT)
  float min_size;
  float max_size;
  EaseFn ease_fn;
  const char *title;
  void (*render_content)(struct Panel *panel, float x, float y, float w,
                         float h);
  void *user_data;
};

///  Sub-states

typedef struct {
  int selected_mesh_index; // -1 = nothing selected
  bool show_aabb;
  bool aabb_full_lines;
  bool aabb_use_mesh_color;
  float aabb_color[4];
} InspectorState;

typedef struct {
  int selected_index; // -1 = nothing selected
  bool show_hidden;
} HierarchyState;

#define FILE_MANAGER_MAX_PATH 512
#define FILE_MANAGER_MAX_ITEMS 1024

typedef enum {
  FILE_ITEM_DIR = 0,
  FILE_ITEM_FILE = 1,
} FileItemType;

typedef struct {
  char name[256];
  char full_path[FILE_MANAGER_MAX_PATH];
  FileItemType type;
  size_t size_bytes;
  int depth;     // Tree indentation level
  bool expanded; // Is this folder open?
} FileItem;

typedef struct {
  char current_path[FILE_MANAGER_MAX_PATH];
  FileItem items[FILE_MANAGER_MAX_ITEMS];
  int item_count;
  int selected_index;
  int scroll_offset;
  int visible_rows;

  // SVG bindless texture slots
  int32_t icon_folder;
  int32_t icon_file;
  int32_t icon_arrow_right;
  int32_t icon_arrow_down;
} FileManagerState;

/// Editor (top-level aggregate)

typedef struct {
  Panel panels[PANEL_COUNT];
  InspectorState inspector;
  HierarchyState hierarchy;
  FileManagerState file_manager;
  Font *font;
  bool initialized;
  double last_time;
  float inspector_fs_split; // Dynamic layout splitter [0.0 - 1.0]
  float fs_split_target;
  float fs_split_start;
  float fs_split_saved;
  float fs_anim_t;
  bool fs_collapsed;
  double last_tab_click_time;
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

void message(MessageType type, const char *text);

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
void editor_update(void); // Once per frame, before editor_render
void editor_render(void); // After all 3-D rendering, before present
void editor_mouse_move(double xpos, double ypos);
void editor_mouse_button(int button, int action);
bool editor_scroll(double x, double y, double y_offset);
bool editor_async_gltf_request(const char *path);
bool editor_async_gltf_shutdown_quiesced(void);
bool editor_async_obj_request(const char *path);
bool editor_async_obj_shutdown_quiesced(void);
void editor_selection_box_begin(float x, float y);
void editor_selection_box_update(float x, float y);
void editor_selection_box_end(float x, float y);
bool editor_selection_box_active(void);
ColorPickerState *editor_color_picker(void);
/* Reports whether an existing modal input owns editor key-to-character input.
 */
bool editor_text_input_is_active(void);
/* Reports whether editor UI owns the current pointer location. */
bool editor_wants_mouse(void);

/// Panel control

void editor_toggle_panel(PanelSide side);
void editor_open_panel(PanelSide side);
void editor_close_panel(PanelSide side);
bool editor_panel_is_open(PanelSide side);

/// Inspector API

void inspector_select_mesh(int index);
void inspector_show_collider(void);
void inspector_show_material(void);
void inspector_deselect(void);

/* Monad-facing transform vocabulary. The editor owns the active transaction;
 * language commands select its mode/axis, snap distance, and commit policy. */
bool editor_transform_begin(EditorTransformMode mode);
bool editor_transform_set_mode(EditorTransformMode mode);
bool editor_transform_set_axis(EditorTransformAxis axis);
bool editor_transform_set_snap(float step_meters);
bool editor_transform_reset_origin(void);
bool editor_transform_finish(bool commit);
bool editor_transform_active(void);

/// Hierarchy API

void hierarchy_select(int index);
bool hierarchy_select_additive_mesh(int index);
bool hierarchy_select_additive_sdf(int index);

/// File Manager API

bool file_manager_navigate(const char *path);
void file_manager_navigate_up(void);

void inspector_select_mesh_internal(int index);

/* Cancel an active sidebar-owned speculative SDF gesture on Escape/focus loss. */
bool editor_cancel_sdf_property_gesture(void);
