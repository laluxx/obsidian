#pragma once
#include "rope.h"
#include "font.h"
#include "theme.h"
#include "editor.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/// Configuration

/* Maximum panel height as a fraction of the viewport (0 < x <= 1) */
#ifndef TEXT_EDITOR_MAX_HEIGHT_FRAC
#define TEXT_EDITOR_MAX_HEIGHT_FRAC  0.60f
#endif

/* Vertical padding inside the content area */
#ifndef TEXT_EDITOR_PAD_Y
#define TEXT_EDITOR_PAD_Y  10.0f
#endif

/* Horizontal padding */
#ifndef TEXT_EDITOR_PAD_X
#define TEXT_EDITOR_PAD_X  14.0f
#endif

/* Title bar height (matches TITLE_H in editor.c) */
#ifndef TEXT_EDITOR_TITLE_H
#define TEXT_EDITOR_TITLE_H  32.0f
#endif

/* Corner radius */
#ifndef TEXT_EDITOR_RADIUS
#define TEXT_EDITOR_RADIUS  8.0f
#endif

/// Buffer

typedef struct {
    rope_t  *rope;          /* underlying text storage                        */
    size_t   pt;            /* point (cursor), in chars                       */
    size_t   mark;          /* mark for region selection (-1 == inactive)     */
    bool     mark_active;

    char     filepath[512]; /* absolute path that was loaded                  */
    char     filename[64];  /* basename, shown in title bar                   */
    bool     modified;      /* dirty flag                                     */
    bool     read_only;
} TextBuffer;

/// Editor state

typedef struct {
    TextBuffer   buf;

    /* Visibility & animation */
    bool         visible;
    float        t;            /* animation [0..1], chases target_t           */
    float        target_t;

    /* Geometry (computed each frame) */
    float        panel_x;
    float        panel_y;
    float        panel_w;
    float        panel_h;      /* current animated height                     */
    float        target_h;     /* height needed to fit buffer, clamped        */

    /* Scroll (pixel offset, Y-up) */
    float        scroll_y;
    float        scroll_target;
    float        scroll_start;
    float        scroll_t;

    /* Stable buffer for the panel title pointer */
    char         title_buf[128];

    /* Cursor blink */
    double       last_key_time;

    /* Keymap handle (opaque — set up in text_editor_init) */
    void        *keymap;       /* KeyChordMap*                                */
} TextEditorState;

/// Global state (defined in text_editor.c)
extern TextEditorState g_text_editor;

/// Lifecycle

void text_editor_init(void);
void text_editor_cleanup(void);

/// Open / close

/*
 * text_editor_open — load a file from disk into the buffer and show the
 * panel.  If the panel is already open with a different file the buffer
 * is replaced.  Returns false on I/O error.
 */
bool text_editor_open(const char *filepath);
void text_editor_close(void);

/// Per-frame

void text_editor_update(float dt, double mx, double my);
void text_editor_render(Font *font);
void render_text_editor_panel(Panel *panel, float px, float py, float pw, float ph);

/// Input routing (called by editor.c mouse/key handlers)

bool text_editor_wants_mouse(double mx, double my);
void text_editor_mouse_button(int button, int action, double mx, double my);
void text_editor_scroll(double dy);

/// Editing commands (wired to the keymap)

void self_insert_char(char c); /* called by set_active_text_input cb       */

void forward_char(void);
void backward_char(void);
void forward_word(void);
void backward_word(void);
void beginning_of_line(void);
void end_of_line(void);
void next_line(void);
void previous_line(void);
void beginning_of_buffer(void);
void end_of_buffer(void);

void delete_char(void);
void backward_delete_char(void);
void kill_line(void);
void kill_word(void);
void backward_kill_word(void);
void yank(void);

void set_mark(void);
void exchange_point_and_mark(void);
void kill_region(void);
void kill_region_save(void);

void te_undo(void);

void save_buffer(void);
void kill_buffer(void);

void recenter(void);

void scroll_up_command(void);
void scroll_down_command(void);

size_t te_get_char_at_mouse(double mx, double my);
