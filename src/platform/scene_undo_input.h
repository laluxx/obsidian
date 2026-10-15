/// scene_undo_input — editor command boundaries for the SDF undo journal.
#pragma once
#include <stdbool.h>

typedef enum {
    OBS_SCENE_KEY_OTHER = 0,
    OBS_SCENE_KEY_MODIFIER,
    OBS_SCENE_KEY_UNDO
} ObsSceneUndoKey;

/* Input reports semantic command boundaries; the journal owns history. */
void obs_scene_undo_on_key(bool pressed, ObsSceneUndoKey kind);
void obs_scene_undo_on_mouse_press(void);
void obs_scene_undo_on_scroll(double dx, double dy);
void obs_scene_undo_on_focus_lost(void);
