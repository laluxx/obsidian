#include "text_editor.h"
#include "editor.h"
#include "renderer.h"
#include "context.h"
#include "font.h"
#include "theme.h"
#include "keychords.h"
#include "easing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <GLFW/glfw3.h>

/// Globals

TextEditorState g_text_editor = {0};

/* Kill ring — single entry for now (yank / C-y) */
static char  *s_kill_ring     = NULL;
static size_t s_kill_ring_len = 0;
static bool s_te_selecting = false;


/* Undo stack — lightweight linear history */
#define UNDO_MAX 512

typedef enum {
    UNDO_INSERT,
    UNDO_DELETE,
} UndoKind;

typedef struct {
    UndoKind kind;
    size_t   char_pos;
    char    *text;       /* heap-allocated snapshot of changed chars */
    size_t   text_len;
} UndoRecord;

static UndoRecord s_undo_stack[UNDO_MAX];
static int        s_undo_top = 0;       /* next free slot */
static int        s_undo_save_point = 0;

extern KeyChordMap keymap;              /* main keymap from keychords.c */
extern void set_active_text_input(void (*cb)(char));

static KeyChordMap s_te_keymap;

//// Forward declarations

static float te_compute_target_height(void);
static void  te_push_undo(UndoKind kind, size_t char_pos, const char *text, size_t len);
static void  te_kill_ring_put(const char *text, size_t len);
static bool  te_is_word_char(uint32_t cp);

static void  te_scroll_to_cursor(void);

/// Buffer helpers

static void buf_init(TextBuffer *b) {
    memset(b, 0, sizeof *b);
    b->rope = rope_new();
    b->pt   = 0;
    b->mark = 0;
    b->mark_active = false;
    b->modified = false;
    b->read_only = false;
}

static void buf_free(TextBuffer *b) {
    if (b->rope) { rope_free(b->rope); b->rope = NULL; }
}

static size_t buf_len(const TextBuffer *b) {
    return rope_char_length(b->rope);
}

/* Clamp point to valid range */
static void buf_clamp_pt(TextBuffer *b) {
    size_t len = buf_len(b);
    if (b->pt > len) b->pt = len;
}

/* Insert a UTF-8 string at point, advance point */
static void buf_insert(TextBuffer *b, const char *str, size_t byte_len) {
    if (!str || !byte_len || b->read_only) return;
    size_t char_count = 0;
    {
        size_t i = 0;
        while (i < byte_len) {
            size_t cl = utf8_char_len((uint8_t)str[i]);
            if (cl > byte_len - i) cl = byte_len - i;
            i += cl; char_count++;
        }
    }
    te_push_undo(UNDO_INSERT, b->pt, str, byte_len);
    b->rope = rope_insert_chars(b->rope, b->pt, str, byte_len);
    b->pt  += char_count;
    b->modified = true;
}

/* Delete `char_count` chars starting at `char_pos` */
static void buf_delete(TextBuffer *b, size_t char_pos, size_t char_count) {
    if (!char_count || b->read_only) return;
    size_t len = buf_len(b);
    if (char_pos >= len) return;
    if (char_pos + char_count > len) char_count = len - char_pos;

    /* Snapshot deleted text for undo */
    {
        char tmp[4096];
        size_t copied = rope_copy_chars(b->rope, char_pos, char_count, tmp, sizeof tmp - 1);
        tmp[copied] = '\0';
        te_push_undo(UNDO_DELETE, char_pos, tmp, copied);
    }

    b->rope = rope_delete_chars(b->rope, char_pos, char_count);
    if (b->pt > char_pos) {
        if (b->pt >= char_pos + char_count)
            b->pt -= char_count;
        else
            b->pt = char_pos;
    }
    b->modified = true;
}

/* Column (0-based chars since last newline) of a given char_pos */
static size_t buf_column_at(const TextBuffer *b, size_t char_pos) {
    if (char_pos == 0) return 0;
    size_t col = 0, i = char_pos;
    while (i > 0) {
        uint32_t cp = rope_char_at(b->rope, i - 1);
        if (cp == '\n') break;
        col++; i--;
    }
    return col;
}

/* Return char_pos of the start of the line containing char_pos */
static size_t buf_line_start(const TextBuffer *b, size_t char_pos) {
    while (char_pos > 0 && rope_char_at(b->rope, char_pos - 1) != '\n')
        char_pos--;
    return char_pos;
}

/* Return char_pos of the end of the line (before newline or at EOF) */
static size_t buf_line_end(const TextBuffer *b, size_t char_pos) {
    size_t len = buf_len(b);
    while (char_pos < len && rope_char_at(b->rope, char_pos) != '\n')
        char_pos++;
    return char_pos;
}

/// Undo

static void te_push_undo(UndoKind kind, size_t char_pos, const char *text, size_t len) {
    if (s_undo_top >= UNDO_MAX) {
        /* Evict oldest record */
        free(s_undo_stack[0].text);
        memmove(&s_undo_stack[0], &s_undo_stack[1], (UNDO_MAX - 1) * sizeof(UndoRecord));
        s_undo_top = UNDO_MAX - 1;
        if (s_undo_save_point > 0) s_undo_save_point--;
        else s_undo_save_point = -1;
    }
    UndoRecord *rec = &s_undo_stack[s_undo_top++];
    rec->kind      = kind;
    rec->char_pos  = char_pos;
    rec->text      = malloc(len + 1);
    memcpy(rec->text, text, len);
    rec->text[len] = '\0';
    rec->text_len  = len;
}

void te_undo(void) {
    if (s_undo_top == 0) return;
    TextBuffer *b = &g_text_editor.buf;
    if (b->read_only) return;

    UndoRecord *rec = &s_undo_stack[--s_undo_top];
    if (rec->kind == UNDO_INSERT) {
        /* Undo an insert: delete what was inserted */
        size_t char_count = 0;
        const char *p = rec->text;
        const char *end = p + rec->text_len;
        while (p < end) { p += utf8_char_len((uint8_t)*p); char_count++; }
        b->rope = rope_delete_chars(b->rope, rec->char_pos, char_count);
        b->pt   = rec->char_pos;
    } else {
        /* Undo a delete: re-insert */
        b->rope = rope_insert_chars(b->rope, rec->char_pos, rec->text, rec->text_len);
        b->pt   = rec->char_pos;
    }
    free(rec->text);
    rec->text = NULL;
    b->modified = (s_undo_top != s_undo_save_point);
    buf_clamp_pt(b);
    te_scroll_to_cursor();
}

/// Kill ring

static void te_kill_ring_put(const char *text, size_t len) {
    free(s_kill_ring);
    s_kill_ring = malloc(len + 1);
    memcpy(s_kill_ring, text, len);
    s_kill_ring[len] = '\0';
    s_kill_ring_len = len;
}

/// Geometry

/*
 * te_compute_target_height — height in pixels needed to display every line
 * of the current buffer, plus padding and title bar.
 * Clamped to TEXT_EDITOR_MAX_HEIGHT_FRAC of the viewport so the panel
 * never grows taller than 60% of the screen.
 */
static float te_compute_target_height(void) {
    Font *font = editor.font;
    if (!font) return 200.0f;

    float lh      = (float)(font->ascent + font->descent);
    size_t lines  = rope_line_count(g_text_editor.buf.rope);
    float content = (float)lines * lh + TEXT_EDITOR_PAD_Y * 2.0f;
    float total   = content + TEXT_EDITOR_TITLE_H;

    float sh      = (float)context.swapChainExtent.height;
    float max_h   = sh * TEXT_EDITOR_MAX_HEIGHT_FRAC;

    return fminf(total, max_h);
}

/// Scroll

static void te_scroll_to_cursor(void) {
    TextEditorState *te = &g_text_editor;
    Font *font = editor.font;
    if (!font) return;

    /* Secure geometry before evaluating math, catching initial un-rendered frames */
    te->panel_h = editor.panels[PANEL_BOTTOM].size;

    float lh        = (float)(font->ascent + font->descent);
    size_t line     = rope_char_to_line(te->buf.rope, te->buf.pt);

    /* Vertical scrolling boundaries */
    float cursor_top_y    = (float)line * lh;
    float cursor_bottom_y = cursor_top_y + lh;

    /* Match exact rendering height (ch) which subtracts PAD_Y only once */
    float visible_h = te->panel_h - TEXT_EDITOR_TITLE_H - TEXT_EDITOR_PAD_Y;
    if (visible_h < lh) visible_h = lh;

    float window_bottom_buffer = te->scroll_target;
    float window_top_buffer    = te->scroll_target + visible_h;

    /* Already visible? Do not disrupt the view */
    if (cursor_top_y >= window_bottom_buffer && cursor_bottom_y <= window_top_buffer)
        return;

    /* Emacs style: center cursor when moving out of bounds */
    float half_window_lines = (visible_h / lh) / 2.0f;
    float new_target = cursor_top_y - (half_window_lines * lh);

    /* Snap to line boundary for clean alignment */
    new_target = floorf(new_target / lh) * lh;

    if (new_target < 0.0f) new_target = 0.0f;

    te->scroll_start  = te->scroll_y;
    te->scroll_target = new_target;
    te->scroll_t      = 0.0f;
}

/// Lifecycle

void text_editor_init(void) {
    TextEditorState *te = &g_text_editor;
    memset(te, 0, sizeof *te);
    buf_init(&te->buf);

    te->visible   = false;
    te->t         = 0.0f;
    te->target_t  = 0.0f;
    te->scroll_y  = 0.0f;
    te->scroll_t  = 1.0f;

    te->keymap = &s_te_keymap;
    keymap_init(&s_te_keymap);

    /* Movement */
    keychord_bind(&s_te_keymap, "C-n",     next_line,           "Next line",            PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-p",     previous_line,       "Previous line",        PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-f",     forward_char,        "Forward char",         PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-b",     backward_char,       "Backward char",        PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "<down>",  next_line,           "Next line",            PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "<up>",    previous_line,       "Previous line",        PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "<right>", forward_char,        "Forward char",         PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "<left>",  backward_char,       "Backward char",        PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "M-f",     forward_word,        "Forward word",         PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "M-b",     backward_word,       "Backward word",        PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-a",     beginning_of_line,   "Beginning of line",    PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-e",     end_of_line,         "End of line",          PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-c p",   beginning_of_buffer, "Beginning of buffer",  PRESS);
    keychord_bind(&s_te_keymap, "C-c n",   end_of_buffer,       "End of buffer",        PRESS);
    keychord_bind(&s_te_keymap, "M-<",     beginning_of_buffer, "Beginning of buffer",  PRESS);
    keychord_bind(&s_te_keymap, "M->",     end_of_buffer,       "End of buffer",        PRESS);

    /* Deletion */
    keychord_bind(&s_te_keymap, "C-d",   delete_char,          "Delete char forward",  PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "DEL",   backward_delete_char, "Delete char backward", PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-k",   kill_line,            "Kill line",            PRESS);
    keychord_bind(&s_te_keymap, "M-d",   kill_word,            "Kill word",            PRESS | REPEAT);

    /* Mark / region */
    keychord_bind(&s_te_keymap, "C-SPC", set_mark,                  "Set mark",             PRESS);
    keychord_bind(&s_te_keymap, "C-w",   kill_region,               "Kill region",          PRESS);
    keychord_bind(&s_te_keymap, "M-w",   kill_region_save,          "Kill region save",     PRESS);
    keychord_bind(&s_te_keymap, "C-y",   yank,                      "Yank",                 PRESS);

    /* Misc */
    keychord_bind(&s_te_keymap, "C-/",      te_undo,               "Undo",           PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-l",      recenter,              "Recenter",       PRESS);
    keychord_bind(&s_te_keymap, "C-v",      scroll_up_command,     "Scroll up",      PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "M-v",      scroll_down_command,   "Scroll down",    PRESS | REPEAT);
    keychord_bind(&s_te_keymap, "C-x k",    kill_buffer,           "Close buffer",   PRESS);
    keychord_bind(&s_te_keymap, "C-x C-s",  save_buffer,           "Save buffer",    PRESS);
}

void text_editor_cleanup(void) {
    buf_free(&g_text_editor.buf);
    free(s_kill_ring);
    s_kill_ring = NULL;
    for (int i = 0; i < s_undo_top; i++) free(s_undo_stack[i].text);
    s_undo_top = 0;
}

/// Open / close

bool text_editor_open(const char *filepath) {
    TextEditorState *te = &g_text_editor;

    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    char *raw = malloc(fsize + 1);
    if (!raw) { fclose(f); return false; }
    fread(raw, 1, fsize, f);
    raw[fsize] = '\0';
    fclose(f);

    /* Replace buffer */
    buf_free(&te->buf);
    buf_init(&te->buf);
    te->buf.rope = rope_new_from_str(raw, (size_t)fsize);
    free(raw);

    strncpy(te->buf.filepath, filepath, sizeof te->buf.filepath - 1);
    /* Basename */
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    strncpy(te->buf.filename, base, sizeof te->buf.filename - 1);
    te->buf.modified = false;

    /* Reset view */
    te->buf.pt        = 0;
    te->scroll_y      = 0.0f;
    te->scroll_target = 0.0f;
    te->scroll_t      = 1.0f;

    /* Clear undo */
    for (int i = 0; i < s_undo_top; i++) free(s_undo_stack[i].text);
    s_undo_top = 0;
    s_undo_save_point = 0;

    te->visible = true;

    /* Titlebar is rendered dynamically now */
    Panel *p = &editor.panels[PANEL_BOTTOM];
    p->title = "";

    /* Resize the panel to fit the buffer, clamped to dynamic max */
    float sh = (float)context.swapChainExtent.height;
    float max_allowed = sh - 32.0f; /* TITLE_H approximation */
    float needed = te_compute_target_height();
    if (needed > max_allowed) needed = max_allowed;
    if (needed < p->min_size) needed = p->min_size;
    p->size = needed;

    /* Swap keymaps */
    KeyChordMap tmp = keymap;
    keymap = s_te_keymap;
    s_te_keymap = tmp;
    set_active_text_input(self_insert_char);

    return true;
}

void text_editor_close(void) {
    TextEditorState *te = &g_text_editor;
    if (!te->visible) return;

    te->visible = false;
    editor_close_panel(PANEL_BOTTOM);

    /* Reset the panel title back to the default */
    editor.panels[PANEL_BOTTOM].title = "Text Editor";

    /* Swap keymaps back */
    KeyChordMap tmp = keymap;
    keymap = s_te_keymap;
    s_te_keymap = tmp;
    set_active_text_input(NULL);
}

/// Per-frame

void text_editor_update(float dt, double mx, double my) {
    TextEditorState *te = &g_text_editor;
    if (!te->visible) return;

    if (s_te_selecting) {
        size_t idx = te_get_char_at_mouse(mx, my);
        if (idx != (size_t)-1 && idx <= buf_len(&te->buf) && te->buf.pt != idx) {
            te->buf.pt = idx;
            te->last_key_time = glfwGetTime();
            te_scroll_to_cursor();
        }
    }

    /* Titlebar is rendered dynamically now */

    /* Smooth scroll */
    if (te->scroll_t < 1.0f) {
        te->scroll_t += 3.0f * dt;
        if (te->scroll_t >= 1.0f) {
            te->scroll_t = 1.0f;
            te->scroll_y = te->scroll_target;
        } else {
            te->scroll_y = te->scroll_start +
                (te->scroll_target - te->scroll_start) * ease_cubic_out(te->scroll_t);
        }
    }
}

/// Render

static void te_draw_buffer(Font *font, float cx, float cy, float cw, float ch) {
    TextEditorState *te = &g_text_editor;
    if (!te->visible || ch < 1.0f) return;

    float lh       = (float)(font->ascent + font->descent);
    float line_top = cy + ch;

    rope_t  *rope  = te->buf.rope;
    size_t   total = buf_len(&te->buf);
    double   tnow  = glfwGetTime();
    bool     cursor_visible = ((tnow - te->last_key_time) < 0.5) ||
                              (fmod(tnow - te->last_key_time, 1.0) < 0.5);

    size_t region_lo = 0, region_hi = 0;
    bool   has_region = te->buf.mark_active;
    if (has_region) {
        region_lo = te->buf.pt < te->buf.mark ? te->buf.pt : te->buf.mark;
        region_hi = te->buf.pt > te->buf.mark ? te->buf.pt : te->buf.mark;
    }

    size_t  char_idx = 0;
    size_t  line_idx = 0;
    float   draw_x   = cx;

    float   base_y   = line_top - (float)font->ascent;
    base_y += te->scroll_y;

    rope_iter_t iter;
    rope_iter_init(&iter, rope, 0);
    uint32_t cp;

    float space_w = character_width(font, ' ');
    if (space_w <= 0.0f) space_w = (float)font->ascent;

/* 1. FAST FORWARD: Skip lines completely above the view */
    size_t start_line = 0;
    if (te->scroll_y > 0.0f) {
        start_line = (size_t)(te->scroll_y / lh);
        if (start_line > 0) start_line--; /* Step back to catch partially visible lines */
    }

    while (line_idx < start_line && rope_iter_next_char(&iter, &cp)) {
        char_idx++;
        if (cp == '\n') line_idx++;
    }

    float panel_top = line_top + TEXT_EDITOR_PAD_Y + TEXT_EDITOR_TITLE_H;

    while (rope_iter_next_char(&iter, &cp)) {
        float cur_line_y = base_y - (float)line_idx * lh;

        /* 2. EARLY EXIT: Stop processing if we passed the bottom of the view */
        if (cur_line_y + (float)font->ascent < cy - lh) {
            break;
        }

        /* 3. PANEL TOP CLIPPING: Skip lines where the highest pixel breaches the absolute top of the panel */
        if (cur_line_y + (float)font->ascent > panel_top) {
            char_idx++;
            if (cp == '\n') { line_idx++; draw_x = cx; }
            continue;
        }

        bool is_cursor = (char_idx == te->buf.pt);
        bool in_region = has_region && char_idx >= region_lo && char_idx < region_hi;

        float char_width = (cp == '\t') ? space_w * 4.0f : character_width(font, cp);
        float acw = (cp == '\n') ? space_w : char_width;

        float box_y = cur_line_y - (float)font->descent * 2.0f;
        float box_h = lh;

        if (cp == '\n') {
            if (in_region) {
                float ext_width = (cx + cw) - draw_x;
                if (ext_width > 0) {
                    Color region_bg = {CT.accent.r, CT.accent.g, CT.accent.b, 0.25f};
                    exQuad2D((vec2){draw_x, cur_line_y - lh + ((float)font->descent * 3.4f)}, (vec2){ext_width, lh},
                             (vec4){1,1,1,1}, 0.0f, region_bg, region_bg);
                }
            }

            if (is_cursor && cursor_visible) {
                exQuad2D((vec2){draw_x, box_y}, (vec2){acw, box_h},
                         (vec4){2,2,2,2}, 0.0f, CT.accent, CT.accent);
            }
            line_idx++;
            draw_x = cx;
            char_idx++;
            continue;
        }

        if (in_region) {
            Color region_bg = {CT.accent.r, CT.accent.g, CT.accent.b, 0.25f};
            exQuad2D((vec2){draw_x, box_y}, (vec2){acw, box_h},
                     (vec4){1,1,1,1}, 0.0f, region_bg, region_bg);
        }

        if (is_cursor && cursor_visible) {
            exQuad2D((vec2){draw_x, box_y}, (vec2){acw, box_h},
                     (vec4){2,2,2,2}, 0.0f, CT.accent, CT.accent);
        }

        Color col = (is_cursor && cursor_visible) ? CT.bg : CT.text;
        character(font, cp, draw_x, cur_line_y, col);
        draw_x += acw;
        char_idx++;
    }
    rope_iter_destroy(&iter);

    /* Cursor at end of buffer */
    if (te->buf.pt == total && cursor_visible) {
        size_t final_line = rope_line_count(rope) > 0 ? rope_line_count(rope) - 1 : 0;
        float cur_line_y  = base_y - (float)final_line * lh;
        if (cur_line_y + (float)font->ascent >= cy - lh && cur_line_y + (float)font->ascent <= panel_top) {
            float box_y = cur_line_y - (float)font->descent * 2.0f;
            exQuad2D((vec2){draw_x, box_y}, (vec2){space_w, lh},
                     (vec4){2,2,2,2}, 0.0f, CT.accent, CT.accent);
        }
    }
}

static int32_t s_te_icon_file = -1;
static int32_t s_te_icon_close = -1;
static int32_t s_te_icon_save = -1;

void text_editor_draw_titlebar(float x, float y, float w, float h, float mx, float my) {
    TextEditorState *te = &g_text_editor;
    Font* font = editor.font;
    if (!font || !te->visible) return;

    if (s_te_icon_file < 0) {
        extern VulkanContext context;
        s_te_icon_file = texture_pool_add_svg(&context, "./assets/icons/File.svg", 16, 16);
        s_te_icon_close = texture_pool_add_svg(&context, "./assets/icons/Close.svg", 16, 16);
        s_te_icon_save = texture_pool_add_svg(&context, "./assets/icons/Save.svg", 16, 16);
    }

    float cx = x + TEXT_EDITOR_PAD_X;
    float cy = y + h * 0.5f;

    float icon_draw_size = 14.0f;
    float icon_y = cy - icon_draw_size * 0.5f - 1.0f;

    Texture2D* tex_file = texture_pool_get(s_te_icon_file);
    if (tex_file) {
        texture2D((vec2){cx, icon_y}, (vec2){icon_draw_size, icon_draw_size}, tex_file, CT.text_dim);
        cx += icon_draw_size + 8.0f;
    }

    if (te->buf.modified) {
        Texture2D* tex_save = texture_pool_get(s_te_icon_save);
        if (tex_save) {
            texture2D((vec2){cx, icon_y}, (vec2){icon_draw_size, icon_draw_size}, tex_save, CT.warning);
            cx += icon_draw_size + 8.0f;
        }
    }

    char dir_buf[256] = "";
    strncpy(dir_buf, te->buf.filepath, sizeof(dir_buf) - 1);
    char* last_slash = strrchr(dir_buf, '/');
    if (last_slash) {
        *last_slash = '\0';
        char* second_last_slash = strrchr(dir_buf, '/');
        if (second_last_slash) {
            memmove(dir_buf, second_last_slash + 1, strlen(second_last_slash + 1) + 1);
        }
    } else {
        dir_buf[0] = '\0';
    }

    float ty = cy - 2.0f;
    Color dir_col = te->buf.modified ? CT.warning : CT.text_dim;
    Color file_col = te->buf.modified ? CT.warning : CT.text;

    if (dir_buf[0] != '\0') {
        text(font, dir_buf, cx, ty, dir_col);
        cx += measure_text_width(font, dir_buf, 1.0f);

        text(font, "/", cx, ty, dir_col);
        cx += character_width(font, '/');
    }

    text(font, te->buf.filename, cx, ty, file_col);
    cx += measure_text_width(font, te->buf.filename, 1.0f);

    cx += character_width(font, ' ') * 2.0f;

    size_t line = rope_char_to_line(te->buf.rope, te->buf.pt) + 1;
    char line_buf[32];
    snprintf(line_buf, sizeof(line_buf), "L%zu", line);
    text(font, line_buf, cx, ty, CT.text_dim);
    cx += measure_text_width(font, line_buf, 1.0f) + character_width(font, ' ');

    float lh = (float)(font->ascent + font->descent);
    size_t total_lines = rope_line_count(te->buf.rope);
    float visible_h = te->panel_h - TEXT_EDITOR_TITLE_H - TEXT_EDITOR_PAD_Y;
    if (visible_h < lh) visible_h = lh;

    int top_line = (int)(te->scroll_y / lh);
    int bot_line = (int)((te->scroll_y + visible_h) / lh);

    char pct_buf[16];
    if (top_line <= 0 && bot_line >= (int)total_lines) {
        strcpy(pct_buf, "All");
    } else if (top_line <= 0) {
        strcpy(pct_buf, "Top");
    } else if (bot_line >= (int)total_lines) {
        strcpy(pct_buf, "Bot");
    } else {
        float pct = ((float)top_line / (float)total_lines) * 100.0f;
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)pct);
    }
    text(font, pct_buf, cx, ty, CT.text_dim);

    float close_x = x + w - TEXT_EDITOR_PAD_X - icon_draw_size;
    float close_y = y + (h - icon_draw_size) * 0.5f - 1.0f;

    bool hovered = (mx >= close_x - 8.0f && mx <= close_x + icon_draw_size + 8.0f &&
                    my >= y && my <= y + h);

    Texture2D* tex_close = texture_pool_get(s_te_icon_close);
    if (tex_close) {
        Color close_col = hovered ? CT.error : CT.border;
        texture2D((vec2){close_x, close_y}, (vec2){icon_draw_size, icon_draw_size}, tex_close, close_col);
    }
}

void text_editor_render(Font *font) {
    (void)font;
    /* Drawing now happens through render_text_editor_panel */
}

void render_text_editor_panel(Panel *panel, float px, float py, float pw, float ph) {
    if (!editor.font) return;
    TextEditorState *te = &g_text_editor;
    if (!te->visible) return;

    te->panel_x = px;
    te->panel_y = py;
    te->panel_w = pw;
    te->panel_h = ph;

    float cx, cy, cw, ch;
    /* Reuse content_area from editor.c — declared extern here */
    /* content_area is static in editor.c so we replicate the math inline */
    cx = px + TEXT_EDITOR_PAD_X;
    cw = pw - TEXT_EDITOR_PAD_X * 2.0f;
    cy = py + TEXT_EDITOR_PAD_Y;
    ch = ph - TEXT_EDITOR_TITLE_H - TEXT_EDITOR_PAD_Y;

    te_draw_buffer(editor.font, cx, cy, cw, ch);
}

/// Input routing

bool text_editor_wants_mouse(double mx, double my) {
    TextEditorState *te = &g_text_editor;
    if (!te->visible) return false;
    float sh = (float)context.swapChainExtent.height;
    float ry = sh - (float)my; /* Y-up */
    return ((float)mx >= te->panel_x && (float)mx <= te->panel_x + te->panel_w &&
            ry >= te->panel_y && ry <= te->panel_y + te->panel_h);
}

void text_editor_scroll(double dy) {
    TextEditorState *te = &g_text_editor;
    Font *font = editor.font;
    if (!font || !te->visible) return;
    float lh = (float)(font->ascent + font->descent);
    te->scroll_start  = te->scroll_y;
    te->scroll_target = te->scroll_y - (float)dy * lh * 3.0f;
    if (te->scroll_target < 0.0f) te->scroll_target = 0.0f;
    te->scroll_t = 0.0f;
}

size_t te_get_char_at_mouse(double mx, double my) {
    TextEditorState *te = &g_text_editor;
    float sh = (float)context.swapChainExtent.height;
    float ry = sh - (float)my;

    float cx = te->panel_x + TEXT_EDITOR_PAD_X;
    float cy = te->panel_y + TEXT_EDITOR_PAD_Y;
    float ch = te->panel_h - TEXT_EDITOR_TITLE_H - TEXT_EDITOR_PAD_Y;
    float line_top = cy + ch;

    Font *font = editor.font;
    if (!font) return (size_t)-1;
    float lh = (float)(font->ascent + font->descent);
    float base_y = line_top - (float)font->ascent + te->scroll_y;

    float diff_y = base_y + (float)font->descent - ry;
    int clicked_line = (int)floorf(diff_y / lh);
    if (clicked_line < 0) clicked_line = 0;

    size_t current_line = 0;
    size_t char_idx = 0;
    float draw_x = cx;

    float space_w = character_width(font, ' ');
    if (space_w <= 0.0f) space_w = (float)font->ascent;

    rope_iter_t iter;
    rope_iter_init(&iter, te->buf.rope, 0);
    uint32_t cp;
    size_t best_idx = buf_len(&te->buf);

    while (rope_iter_next_char(&iter, &cp)) {
        if (current_line == (size_t)clicked_line) {
            float char_width = (cp == '\t') ? space_w * 4.0f : character_width(font, cp);
            float acw = (cp == '\n') ? space_w : char_width;

            if (mx >= draw_x && mx <= draw_x + acw * 0.5f) {
                best_idx = char_idx;
                break;
            } else if (mx > draw_x + acw * 0.5f && mx <= draw_x + acw) {
                best_idx = char_idx + 1;
                if (cp == '\n') best_idx = char_idx;
                break;
            }
            draw_x += acw;
            if (cp == '\n') {
                best_idx = char_idx;
                break;
            }
        } else if (current_line > (size_t)clicked_line) {
            break;
        }

        char_idx++;
        if (cp == '\n') current_line++;
    }
    rope_iter_destroy(&iter);
    return best_idx;
}

void text_editor_mouse_button(int button, int action, double mx, double my) {
    TextEditorState *te = &g_text_editor;
    if (!te->visible) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            size_t idx = te_get_char_at_mouse(mx, my);
            if (idx != (size_t)-1 && idx <= buf_len(&te->buf)) {
                te->buf.pt = idx;
                te->buf.mark = idx;
                te->buf.mark_active = true;
                te->last_key_time = glfwGetTime();
                te_scroll_to_cursor();
                s_te_selecting = true;
            }
        } else if (action == GLFW_RELEASE) {
            s_te_selecting = false;
        }
    }
}


/// Self-insert (called by keychords text-input callback)

void self_insert_char(char c) {
    TextBuffer *b = &g_text_editor.buf;
    char s[2] = {c, '\0'};
    buf_insert(b, s, 1);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

/// Movement

void forward_char(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (b->pt < buf_len(b)) b->pt++;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void backward_char(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (b->pt > 0) b->pt--;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

static bool te_is_word_char(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9') || cp == '_' || cp > 127;
}

void forward_word(void) {
    TextBuffer *b = &g_text_editor.buf;
    size_t len = buf_len(b);
    /* Skip non-word */
    while (b->pt < len && !te_is_word_char(rope_char_at(b->rope, b->pt))) b->pt++;
    /* Skip word */
    while (b->pt < len &&  te_is_word_char(rope_char_at(b->rope, b->pt))) b->pt++;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void backward_word(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (b->pt == 0) return;
    b->pt--;
    while (b->pt > 0 && !te_is_word_char(rope_char_at(b->rope, b->pt))) b->pt--;
    while (b->pt > 0 &&  te_is_word_char(rope_char_at(b->rope, b->pt - 1))) b->pt--;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void beginning_of_line(void) {
    TextBuffer *b = &g_text_editor.buf;
    b->pt = buf_line_start(b, b->pt);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void end_of_line(void) {
    TextBuffer *b = &g_text_editor.buf;
    b->pt = buf_line_end(b, b->pt);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void next_line(void) {
    TextBuffer *b  = &g_text_editor.buf;
    size_t col     = buf_column_at(b, b->pt);
    size_t eol     = buf_line_end(b, b->pt);
    size_t len     = buf_len(b);
    if (eol >= len) return;
    size_t next_start = eol + 1; /* skip newline */
    size_t next_end   = buf_line_end(b, next_start);
    size_t next_len   = next_end - next_start;
    b->pt = next_start + (col < next_len ? col : next_len);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void previous_line(void) {
    TextBuffer *b = &g_text_editor.buf;
    size_t col    = buf_column_at(b, b->pt);
    size_t sol    = buf_line_start(b, b->pt);
    if (sol == 0) return;
    size_t prev_end   = sol - 1; /* position of newline ending previous line */
    size_t prev_start = buf_line_start(b, prev_end);
    size_t prev_len   = prev_end - prev_start;
    b->pt = prev_start + (col < prev_len ? col : prev_len);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void beginning_of_buffer(void) {
    g_text_editor.buf.pt = 0;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void end_of_buffer(void) {
    g_text_editor.buf.pt = buf_len(&g_text_editor.buf);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

/// Deletion

void delete_char(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (b->pt < buf_len(b)) buf_delete(b, b->pt, 1);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void backward_delete_char(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (b->pt > 0) {
        b->pt--;
        buf_delete(b, b->pt, 1);
    }
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void kill_line(void) {
    TextBuffer *b = &g_text_editor.buf;
    size_t eol    = buf_line_end(b, b->pt);
    size_t len    = buf_len(b);

    if (eol == b->pt) {
        /* At end of line: kill the newline itself */
        if (b->pt < len) buf_delete(b, b->pt, 1);
    } else {
        size_t count = eol - b->pt;
        char tmp[4096];
        rope_copy_chars(b->rope, b->pt, count, tmp, sizeof tmp);
        te_kill_ring_put(tmp, count); /* byte approximation */
        buf_delete(b, b->pt, count);
    }
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void kill_word(void) {
    TextBuffer *b   = &g_text_editor.buf;
    size_t start    = b->pt;
    forward_word();
    size_t count    = b->pt - start;
    if (count == 0) return;
    char tmp[4096];
    rope_copy_chars(b->rope, start, count, tmp, sizeof tmp - 1);
    te_kill_ring_put(tmp, count);
    b->pt = start;
    buf_delete(b, start, count);
    te_scroll_to_cursor();
}

void backward_kill_word(void) {
    TextBuffer *b   = &g_text_editor.buf;
    size_t end      = b->pt;
    backward_word();
    size_t count    = end - b->pt;
    if (count == 0) return;
    char tmp[4096];
    rope_copy_chars(b->rope, b->pt, count, tmp, sizeof tmp - 1);
    te_kill_ring_put(tmp, count);
    buf_delete(b, b->pt, count);
    te_scroll_to_cursor();
}

void yank(void) {
    if (!s_kill_ring || s_kill_ring_len == 0) return;
    TextBuffer *b = &g_text_editor.buf;
    buf_insert(b, s_kill_ring, s_kill_ring_len);
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

/// Mark / region

void set_mark(void) {
    TextBuffer *b     = &g_text_editor.buf;
    b->mark           = b->pt;
    b->mark_active    = true;
    g_text_editor.last_key_time = glfwGetTime();
}

void exchange_point_and_mark(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (!b->mark_active) return;
    size_t tmp = b->pt;
    b->pt      = b->mark;
    b->mark    = tmp;
    g_text_editor.last_key_time = glfwGetTime();
    te_scroll_to_cursor();
}

void kill_region(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (!b->mark_active) return;
    size_t lo = b->pt < b->mark ? b->pt : b->mark;
    size_t hi = b->pt > b->mark ? b->pt : b->mark;
    size_t count = hi - lo;
    char tmp[16384];
    rope_copy_chars(b->rope, lo, count, tmp, sizeof tmp - 1);
    te_kill_ring_put(tmp, count);
    buf_delete(b, lo, count);
    b->mark_active = false;
    te_scroll_to_cursor();
}

void kill_region_save(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (!b->mark_active) return;
    size_t lo = b->pt < b->mark ? b->pt : b->mark;
    size_t hi = b->pt > b->mark ? b->pt : b->mark;
    size_t count = hi - lo;
    char tmp[16384];
    rope_copy_chars(b->rope, lo, count, tmp, sizeof tmp - 1);
    te_kill_ring_put(tmp, count);
    b->mark_active = false;
}

/// Misc

void recenter(void) {
    TextEditorState *te = &g_text_editor;
    Font *font = editor.font;
    if (!font) return;

    te->panel_h = editor.panels[PANEL_BOTTOM].size;

    float lh        = (float)(font->ascent + font->descent);
    size_t line     = rope_char_to_line(te->buf.rope, te->buf.pt);
    float cursor_py = (float)line * lh;

    /* Match exact rendering height (ch) which subtracts PAD_Y only once */
    float visible_h = te->panel_h - TEXT_EDITOR_TITLE_H - TEXT_EDITOR_PAD_Y;
    if (visible_h < lh) visible_h = lh;

    float new_target = cursor_py - visible_h * 0.5f + lh * 0.5f;
    if (new_target < 0.0f) new_target = 0.0f;

    te->scroll_start  = te->scroll_y;
    te->scroll_target = new_target;
    te->scroll_t      = 0.0f;
}


void scroll_up_command(void) {
    TextEditorState *te = &g_text_editor;
    Font *font = editor.font;
    if (!font) return;
    float page = te->panel_h - TEXT_EDITOR_TITLE_H;
    te->scroll_start  = te->scroll_y;
    te->scroll_target = te->scroll_y + page;
    te->scroll_t      = 0.0f;
}

void scroll_down_command(void) {
    TextEditorState *te = &g_text_editor;
    Font *font = editor.font;
    if (!font) return;
    float page = te->panel_h - TEXT_EDITOR_TITLE_H;
    te->scroll_start  = te->scroll_y;
    te->scroll_target = te->scroll_y - page;
    if (te->scroll_target < 0.0f) te->scroll_target = 0.0f;
    te->scroll_t = 0.0f;
}

void save_buffer(void) {
    TextBuffer *b = &g_text_editor.buf;
    if (!b->filepath[0] || b->read_only) return;
    FILE *f = fopen(b->filepath, "wb");
    if (!f) return;
    size_t len;
    char *str = rope_to_string(b->rope, &len);
    if (str) { fwrite(str, 1, len, f); free(str); }
    fclose(f);
    b->modified = false;
    s_undo_save_point = s_undo_top;
}

void kill_buffer(void) {
    text_editor_close();
}
