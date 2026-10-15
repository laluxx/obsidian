/// scene_undo_input — attach editor commands to the production undo boundary.
#include "scene_undo_input.h"
#include "sdf_inspector.h"

void obs_scene_undo_on_key(bool pressed, ObsSceneUndoKey kind) {
    if (pressed && kind == OBS_SCENE_KEY_OTHER)
        obs_sdf_inspector_command_boundary();
}

void obs_scene_undo_on_mouse_press(void) {
    obs_sdf_inspector_command_boundary();
}

void obs_scene_undo_on_scroll(double dx, double dy) {
    if (dx != 0.0 || dy != 0.0)
        obs_sdf_inspector_command_boundary();
}

void obs_scene_undo_on_focus_lost(void) {
    obs_sdf_inspector_command_boundary();
}
