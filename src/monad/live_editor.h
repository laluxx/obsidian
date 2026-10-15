#ifndef OBSIDIAN_MONAD_LIVE_EDITOR_H
#define OBSIDIAN_MONAD_LIVE_EDITOR_H

#include "keychords.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ObsMonadRuntime ObsMonadRuntime;

typedef void (*ObsLiveEditorAction)(void);

typedef struct {
    const char *query;
    const char *context;
    const void *strategy;
    const void *history;
    void *query_value;
    bool failed;
} ObsMonadCompletionContext;

typedef struct {
    char name[96];
    char type[160];
    char value[128];
    char documentation[256];
} ObsMonadVariableView;

typedef struct {
    bool dragging;
    float pan_x;
    float pan_y;
    double last_x;
    double last_y;
} ObsMonadImagePointerState;

typedef enum {
    OBS_MONAD_HIERARCHY_SELECT_SINGLE,
    OBS_MONAD_HIERARCHY_SELECT_TOGGLE,
    OBS_MONAD_HIERARCHY_SELECT_RANGE,
    OBS_MONAD_HIERARCHY_SELECT_APPEND
} ObsMonadHierarchySelectionAction;

typedef enum {
    OBS_MONAD_GLTF_SELECTION_NONE = 0,
    OBS_MONAD_GLTF_SELECTION_DEMOTE = 1,
    OBS_MONAD_GLTF_SELECTION_PROMOTE = 2
} ObsMonadGltfSelectionTransition;

typedef enum {
    OBS_MONAD_TREE_ROW_NONE = 0,
    OBS_MONAD_TREE_ROW_SELECT = 1,
    OBS_MONAD_TREE_ROW_EXPAND = 2,
    OBS_MONAD_TREE_ROW_TOGGLE_VISIBILITY = 3,
    OBS_MONAD_TREE_ROW_DRAG = 4
} ObsMonadTreeRowAction;

typedef enum {
    OBS_MONAD_FILESYSTEM_ACTIVATE_NONE,
    OBS_MONAD_FILESYSTEM_ACTIVATE_SCENE_PREVIEW,
    OBS_MONAD_FILESYSTEM_ACTIVATE_IMAGE_PREVIEW,
    OBS_MONAD_FILESYSTEM_ACTIVATE_TEXT
} ObsMonadFilesystemActivation;

typedef enum {
    OBS_MONAD_INSPECTOR_MODE_DEFAULT,
    OBS_MONAD_INSPECTOR_MODE_SKELETON,
    OBS_MONAD_INSPECTOR_MODE_COLLIDER,
    OBS_MONAD_INSPECTOR_MODE_MATERIAL
} ObsMonadInspectorMode;

typedef enum {
    OBS_MONAD_INSPECTOR_DELETE_WHOLE_OBJECT,
    OBS_MONAD_INSPECTOR_DELETE_COLLIDER,
    OBS_MONAD_INSPECTOR_DELETE_MATERIAL,
    OBS_MONAD_INSPECTOR_DELETE_SKELETON
} ObsMonadInspectorDeleteAction;

typedef enum {
    OBS_MONAD_MARQUEE_TARGET_NONE,
    OBS_MONAD_MARQUEE_TARGET_MESH,
    OBS_MONAD_MARQUEE_TARGET_SDF
} ObsMonadMarqueeTarget;

typedef bool (*ObsMonadVariableVisitor)(const ObsMonadVariableView *variable,
                                        void *context);

/* Editor keymaps are C-owned foreign objects in the persistent live Monad
 * image. Registration is cold-path and may precede runtime startup. */
bool obs_monad_live_editor_register_map(const char *name, KeyChordMap *map);
bool obs_monad_live_editor_register_action(const char *name,
                                           ObsLiveEditorAction action);
bool obs_monad_live_editor_open(ObsMonadRuntime *host, const char **error);
bool obs_monad_live_editor_enabled(void);
KeyChordBinding *obs_monad_live_editor_find_binding(const char *map_name,
                                                     const char *notation);
bool obs_monad_live_editor_copy_map(const char *source_name,
                                    KeyChordMap *destination);
void obs_monad_live_editor_close(ObsMonadRuntime *host);
bool obs_monad_live_editor_load_file(ObsMonadRuntime *host, const char *path,
                                     const char **error);
bool obs_monad_live_editor_reload_file(ObsMonadRuntime *host,
                                      const char *path, const char **error);
void obs_monad_live_editor_clear_map_bindings(void);
bool obs_monad_live_editor_vertico_options(int64_t *width, int64_t *count,
                                           int64_t *scroll_margin, bool *top,
                                           bool *resize_pixelwise);
bool obs_monad_live_editor_completion_begin(
    const char *context, const char *query,
    ObsMonadCompletionContext *completion);
bool obs_monad_live_editor_completion_record(const char *context,
                                              const char *candidate);
bool obs_monad_live_editor_completion_score(
    ObsMonadCompletionContext *completion, const char *candidate,
    float *score);
void obs_monad_live_editor_completion_end(
    ObsMonadCompletionContext *completion);
bool obs_monad_live_editor_marginalia_scene(
    const char *cached, const char *detail, char *out, size_t capacity);
bool obs_monad_live_editor_marginalia_variable(
    const char *type, const char *value, const char *documentation,
    char *out, size_t capacity);
bool obs_monad_live_editor_marginalia_material(
    const char *kind, const char *detail, char *out, size_t capacity);
bool obs_monad_live_editor_filesystem_tree_command(const char *operation,
                                                    const char *path,
                                                    bool *expanded);
bool obs_monad_live_editor_filesystem_tree_selection(int64_t *index,
                                                      bool set);
bool obs_monad_live_editor_filesystem_activation(
    const char *extension, bool is_file, ObsMonadFilesystemActivation *action);
bool obs_monad_live_editor_filesystem_double_click(
    int previous_index, int clicked_index, double previous_time,
    double current_time, bool *is_double_click);
bool obs_monad_live_editor_filesystem_tree_wheel_scroll(
    float current_scroll, float wheel_delta, float item_count,
    float visible_rows, float row_height, float *next_scroll);
bool obs_monad_live_editor_filesystem_search_next_index(
    int current_index, int match_count, int direction, int *next_index);
bool obs_monad_live_editor_text_editor_wheel_scroll_target(
    float current_scroll, float wheel_delta, float row_height,
    float *target_scroll);
bool obs_monad_live_editor_text_pointer_transition(
    bool selecting, bool pressed, bool released, bool valid_position,
    int *action, bool *next_selecting);
bool obs_monad_live_editor_gizmo_rotation_hover_priority(
    int current_part, bool trackball_hit, int *hovered_part);
bool obs_monad_live_editor_gizmo_pointer_transition(
    int current_drag, int hovered_part, bool pressed, bool released,
    bool active, int *next_drag);
bool obs_monad_live_editor_gizmo_press_action(int drag_part,
                                               bool bone_hovered,
                                               int *action);
bool obs_monad_live_editor_ui_window_pointer_action(
    int left_button, int action, bool close_hovered, bool in_title,
    bool inside_window, bool dragging, int *result_action);
bool obs_monad_live_editor_ui_window_animation_time_step(
    float progress, float delta_time, bool closing, float *next_progress);
bool obs_monad_live_editor_ui_window_clamp_axis_position(
    float position, float minimum, float maximum, float lower_extent,
    float upper_extent, float *next_position);
bool obs_monad_live_editor_bottom_panel_title_action(
    bool close_hit, bool title_hit, bool transport_hit, bool collapsed,
    bool double_click, int *action);
bool obs_monad_live_editor_animation_title_action(
    bool stop_hit, bool play_hit, bool close_hit, int *action);
bool obs_monad_live_editor_animation_pointer_action(
    int title_action, bool ruler_hit, bool keyframe_hit, bool track_hit,
    int *action);
bool obs_monad_live_editor_message_drag_transition(
    bool dragging, bool pressed, bool released, bool inside_message,
    bool *next_dragging);
bool obs_monad_live_editor_animation_track_wheel_scroll(
    float current_scroll, float wheel_delta, float track_count,
    float row_height, float *next_scroll);
bool obs_monad_live_editor_animation_view_pan(
    float view_start, float view_end, float total_duration, float wheel_delta,
    float *next_start, float *next_end);
bool obs_monad_live_editor_animation_keyframe_click_action(
    float delta_x, bool selected, int *action);
bool obs_monad_live_editor_animation_transport_transition(
    int action, float current_time, float duration, bool playing,
    float *next_time, bool *next_playing);
bool obs_monad_live_editor_animation_playback_view_transition(
    float time, float total_duration, float view_start, float view_end,
    bool looped, bool visible, float *next_view_start, float *next_view_end);
bool obs_monad_live_editor_animation_pointer_time(
    float mouse_offset, float timeline_width, float view_start,
    float view_end, float duration, float *time);
bool obs_monad_live_editor_animation_playhead_drag_transition(
    bool dragging, bool pressed, bool released, bool in_ruler, bool *next);
bool obs_monad_live_editor_image_pointer_transition(
    int action, const ObsMonadImagePointerState *current,
    double x, double y, ObsMonadImagePointerState *next);
bool obs_monad_live_editor_image_zoom_level(float zoom, float factor,
                                             float *next_zoom);
bool obs_monad_live_editor_image_zoom_pan(
    float pan_x, float pan_y, float old_zoom, float next_zoom,
    float center_x, float center_y, float pointer_x, float pointer_y,
    float *next_pan_x, float *next_pan_y);
bool obs_monad_live_editor_color_picker_pointer_action(
    bool ring_hit, bool triangle_hit, int *action);
bool obs_monad_live_editor_color_picker_drag_transition(
    int current_drag, int pointer_action, bool released, int *next_drag);
bool obs_monad_live_editor_hierarchy_selection(
    int32_t clicked_node, bool control, bool shift, int32_t anchor_row,
    ObsMonadHierarchySelectionAction *action);
bool obs_monad_live_editor_hierarchy_anchor(int32_t *anchor, bool set);
bool obs_monad_live_editor_hierarchy_range_bounds(
    int32_t anchor_row, int32_t target_row, int32_t item_count,
    int32_t *first_row, int32_t *last_row);
bool obs_monad_live_editor_hierarchy_node_selection(
    ObsMonadHierarchySelectionAction action, bool selected, bool is_group,
    bool *clear_others, bool *next_selected, bool *next_selected_group);
bool obs_monad_live_editor_hierarchy_gltf_selection_transition(
    int selected_mesh_count, int mesh_count, bool group_selected,
    ObsMonadGltfSelectionTransition *transition);
bool obs_monad_live_editor_tree_selection_scroll_target(
    float row_top, float row_height, float view_height, float current_scroll,
    float *target_scroll);
bool obs_monad_live_editor_tree_scroll_center_target(
    float row_top, float view_height, float *target_scroll);
bool obs_monad_live_editor_tree_search_beam_row(
    float previous_row, float current_row, float animation_time,
    float *beam_row);
bool obs_monad_live_editor_tree_scroll_animation_step(
    float scroll_start, float scroll_target, float current_scroll,
    float scroll_time, float delta_time, float *next_scroll,
    float *next_scroll_time);
bool obs_monad_live_editor_tree_row_action(
    bool mouse_down, bool clicked, bool has_visibility, double mouse_x,
    double row_x, double visibility_x, double visibility_width,
    bool expandable, ObsMonadTreeRowAction *action);
bool obs_monad_live_editor_viewport_marquee_requires_containment(
    bool left_to_right, bool *requires_containment);
bool obs_monad_live_editor_viewport_marquee_drag_active(
    float delta_x, float delta_y, bool *active);
bool obs_monad_live_editor_viewport_marquee_bounds_match(
    float min_x, float min_y, float max_x, float max_y,
    float selection_min_x, float selection_min_y,
    float selection_max_x, float selection_max_y,
    bool requires_containment, bool *matches);
bool obs_monad_live_editor_viewport_marquee_focus(
    int selected_count, int first_mesh, int first_sdf,
    ObsMonadMarqueeTarget *target, int *target_index, bool *gizmo_active);
bool obs_monad_live_editor_hierarchy_visibility(bool any_visible,
                                                bool *target_visible);
bool obs_monad_live_editor_hierarchy_expansion(bool expanded,
                                               bool *target_expanded);
bool obs_monad_live_editor_inspector_mode(const char *subject,
                                          ObsMonadInspectorMode *mode);
bool obs_monad_live_editor_inspector_current_mode(ObsMonadInspectorMode *mode);
bool obs_monad_live_editor_inspector_delete_action(
    bool has_mesh,
    ObsMonadInspectorDeleteAction *action);
bool obs_monad_live_editor_visit_variables(
    ObsMonadVariableVisitor visitor, void *context, bool *truncated);

#endif
