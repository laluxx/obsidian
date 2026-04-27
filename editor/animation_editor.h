#pragma once

#include "editor.h"
#include <stdint.h>
#include <stdbool.h>


///  Constants

#define ANIM_EDITOR_TITLE_H      32.0f
#define ANIM_EDITOR_PAD_X        12.0f
#define ANIM_EDITOR_PAD_Y         8.0f
#define ANIM_EDITOR_TRACK_H      28.0f
#define ANIM_EDITOR_HEADER_W    180.0f   // Width of the left label column
#define ANIM_EDITOR_MIN_SIZE    220.0f
#define ANIM_EDITOR_DEFAULT_SIZE 320.0f

#define ANIM_EDITOR_MAX_KEYFRAMES 2048


///  Types

typedef struct {
    float  time;       // Time in seconds
    bool   selected;   // Is this keyframe selected?
} AnimKeyframe;

typedef struct {
    char name[64];             // Track label (e.g. BoneName : Pos)
    AnimKeyframe* keyframes;   // Pointer into flat keyframe pool
    int    kf_count;
    float  duration;    // Total duration of this animation in seconds
} AnimTrack;

typedef struct {
    bool    visible;

    // Playback state
    bool    playing;
    float   time;              // Current playback head position in seconds

    // Source data (points into the selected mesh's gltf instance)
    AnimTrack  tracks[64];
    int        track_count;
    int        selected_track; // -1 = none
    float      total_duration; // Max duration across all tracks

    // Flat keyframe pool — all tracks share this
    AnimKeyframe kf_pool[ANIM_EDITOR_MAX_KEYFRAMES];
    int          kf_pool_used;

    // Viewport
    float  view_start;        // Seconds at left edge of timeline
    float  view_end;          // Seconds at right edge of timeline
    float  scroll_x;          // Horizontal scroll offset in pixels (smooth)
    float  scroll_x_target;
    float  scroll_x_start;
    float  scroll_x_t;

    float  scroll_y;          // Vertical scroll for many tracks
    float  scroll_y_target;
    float  scroll_y_start;
    float  scroll_y_t;

    // Drag state for the playhead
    bool   dragging_playhead;

    // Panel geometry (set each frame by the render call)
    float  panel_x, panel_y, panel_w, panel_h;
} AnimEditorState;


///  Globals

extern AnimEditorState g_anim_editor;


///  Lifecycle

void anim_editor_init(void);
void anim_editor_cleanup(void);

// Open the animation editor for the mesh at `mesh_index`.
// Returns false if the mesh has no animations.
bool anim_editor_open(int mesh_index);
void anim_editor_close(void);


///  Per-frame

void anim_editor_update(float dt, double mx, double my);


///  Render

// Draws the custom titlebar into the bottom panel's title bar strip.
// Called from editor.c's panel_draw_titlebar instead of text_editor_draw_titlebar
// when the animation editor is active.
void anim_editor_draw_titlebar(float x, float y, float w, float h,
                               float mx, float my);

// Draws the timeline content area.
// Called via Panel.render_content.
void render_anim_editor_panel(Panel* panel,
                              float px, float py, float pw, float ph);


///  Input

bool anim_editor_wants_mouse(double mx, double my);
void anim_editor_scroll(double dx, double dy);
void anim_editor_mouse_button(int button, int action, double mx, double my);


///  Keybinding callbacks (also called by keychords)

void anim_editor_play_pause(void);   // SPC
void anim_editor_stop(void);         // also rewinds to t=0
void anim_editor_next_keyframe(void);
void anim_editor_prev_keyframe(void);
void anim_editor_goto_start(void);
void anim_editor_goto_end(void);
