/// repl_panel.c — Obsidian's own GPU-rendered embedding client
#include "repl_panel.h"
#include "live_editor.h"
#include "repl_service.h"
#include "repl_model.h"
#include "editor.h"
#include "ui.h"
#include "keychords.h"
#include "input.h"
#include "theme.h"
#include "context.h"
#include "renderer.h"
#include <stdio.h>
#include <string.h>

/// Input ownership and keymap layering
static KeyChordMap s_repl_keys;
static TextCallback s_previous_char;
static UIWindow s_window;
static bool s_active;
static bool s_initialized;
static size_t s_our_stack_depth;

bool obs_repl_panel_active(void) { return s_active; }
static void char_input(unsigned int cp) {
    if (s_active) (void)obs_repl_model_insert(obs_monad_repl_model(), cp);
}
static void erase(void) {
    if (s_active) (void)obs_repl_model_backspace(obs_monad_repl_model());
}
static void submit(void) {
    if (s_active) (void)obs_repl_model_submit(obs_monad_repl_model());
}
static void newline(void) {
    if (s_active) (void)obs_repl_model_insert(obs_monad_repl_model(), '\n');
}
static void history_prev(void) {
    if (s_active) (void)obs_repl_model_history(obs_monad_repl_model(), true);
}
static void history_next(void) {
    if (s_active) (void)obs_repl_model_history(obs_monad_repl_model(), false);
}
static void close_repl(void) {
    if (!s_active) return;
    /* A different modal owner may have been pushed on top. Never pop it. */
    if (keymap_stack_count != s_our_stack_depth + 1 ||
        keymap_stack[keymap_stack_count - 1] != &s_repl_keys)
        return;
    keymap_stack_pop();
    if (currentTextCallback == char_input)
        registerTextCallback(s_previous_char);
    s_previous_char = NULL;
    s_active = false;
    if (s_window.visible && !s_window.closing)
        ui_window_close(&s_window);
}
void obs_repl_panel_toggle(void) {
    if (s_active) { close_repl(); return; }
    /* A modal editor/input owner takes precedence. Never steal its callbacks. */
    if (!s_initialized || !editor.initialized || editor_text_input_is_active() ||
        currentTextCallback || keymap_stack_count >= MAX_KEYMAP_STACK)
        return;
    s_our_stack_depth = keymap_stack_count;
    s_previous_char = currentTextCallback;
    keymap_stack_push(&s_repl_keys);
    if (keymap_stack_count != s_our_stack_depth + 1) return;
    registerTextCallback(char_input);
    s_active = true;
    const float screen_w = (float)context.drawExtent.width;
    const float screen_h = (float)context.drawExtent.height;
    float content_w = screen_w - 80.0f;
    if (content_w > 900.0f) content_w = 900.0f;
    if (content_w < 280.0f) content_w = 280.0f;
    float content_h = screen_h - 140.0f;
    if (content_h > 320.0f) content_h = 320.0f;
    if (content_h < 110.0f) content_h = 110.0f;
    ui_window_open(&s_window, screen_w * 0.5f, screen_h * 0.5f,
                   content_w, content_h, 16.0f, 16.0f,
                   screen_w - 16.0f, screen_h - 16.0f);
}
void obs_repl_panel_init(void) {
    if (s_initialized) return;
    keymap_init(&s_repl_keys);
    ui_window_init(&s_window, "Monad REPL");
    if (!obs_monad_live_editor_register_map("repl", &s_repl_keys))
        fprintf(stderr, "Could not register live REPL keymap\n");
#define REGISTER_REPL_ACTION(name, fn) \
    if (!obs_monad_live_editor_register_action(name, fn)) \
        fprintf(stderr, "Could not register live editor action: %s\n", name)
    REGISTER_REPL_ACTION("repl-submit", submit);
    REGISTER_REPL_ACTION("repl-newline", newline);
    REGISTER_REPL_ACTION("repl-backspace", erase);
    REGISTER_REPL_ACTION("repl-history-previous", history_prev);
    REGISTER_REPL_ACTION("repl-history-next", history_next);
    REGISTER_REPL_ACTION("repl-close", close_repl);
#undef REGISTER_REPL_ACTION
    s_initialized = true;
    (void)obs_monad_repl_model();
}
void obs_repl_panel_cleanup(void) {
    if (!s_initialized) return;
    close_repl();
    /* Caller must close the modal input stack before destroying its map. */
    for (size_t i = 0; i < keymap_stack_count; ++i)
        if (keymap_stack[i] == &s_repl_keys) return; /* no dangling map */
    keymap_free(&s_repl_keys);
    s_initialized = false;
}

void obs_repl_panel_update(float dt, double mx, double my) {
    if (s_window.visible) ui_window_update(&s_window, dt, mx, my);
}

bool obs_repl_panel_mouse_move(double mx, double my) {
    if (!s_window.visible) return false;
    if (ui_window_mouse_move(&s_window, mx, my)) return true;
    return ui_window_wants_mouse(&s_window, mx, my);
}

bool obs_repl_panel_mouse_button(int button, int action, double mx, double my) {
    if (!s_window.visible) return false;
    const bool consumed = ui_window_mouse_button(&s_window, button, action, mx, my);
    if (s_window.closing) close_repl();
    return consumed || ui_window_wants_mouse(&s_window, mx, my);
}

/// GPU text submission: uses the existing Vulkan 2D batch and glyph atlas
static void fit_line(char *out, size_t cap, const char *text_value, size_t columns) {
    size_t i = 0, used = 0, input_bytes = strlen(text_value);
    /* Preserve UTF-8 scalar boundaries, and render newlines as spaces. */
    while (i < input_bytes && used < columns && i + 4 < cap) {
        unsigned char c = (unsigned char)text_value[i];
        size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
                   (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        if (i + n >= cap || i + n > input_bytes) break;
        if (c == '\n' || c == '\r') { out[i++] = ' '; ++used; continue; }
        memcpy(out + i, text_value + i, n);
        i += n; ++used;
    }
    out[i] = '\0';
}
void obs_repl_panel_render(Font *font) {
    if (!s_window.visible || !font || !ui_window_begin_render(&s_window)) return;
    const float x = ui_window_content_x(&s_window);
    const float y = ui_window_content_y(&s_window);
    const float width = s_window.content_w;
    const float height = s_window.content_h;
    const float row_height = 22.0f;
    float cw = character_width(font, 'M');
    if (cw < 6.0f) cw = 10.0f;
    size_t columns = width > 16.0f ? (size_t)((width - 16.0f) / cw) : 0;
    if (columns > 245) columns = 245;
    char help[OBS_REPL_LINE_CAP];
    fit_line(help, sizeof(help),
        "RET evaluate  |  C-j newline  |  C-p/C-n history  |  C-g close",
        columns);
    ui_window_render_chrome(&s_window, font);
    ui_draw_text(&s_window, font, help, x, y + height - 18.0f, CT.accent);
    ObsReplModel *model = obs_monad_repl_model();
    size_t visible = height > 62.0f ? (size_t)((height - 62.0f) / row_height) : 0;
    if (visible > model->count) visible = model->count;
    char truncated[OBS_REPL_LINE_CAP];
    for (size_t index = 0; index < visible; ++index) {
        const ObsReplLine *line = obs_repl_model_recent(model, visible - index - 1);
        if (!line) continue;
        fit_line(truncated, sizeof(truncated), line->text, columns);
        Color fg = line->tone == OBS_REPL_ERROR ? CT.error :
                   line->tone == OBS_REPL_SUCCESS ? CT.success :
                   line->tone == OBS_REPL_COMMAND ? CT.prompt : CT.text_dim;
        ui_draw_text(&s_window, font, truncated, x,
                     y + height - 42.0f - (float)index * row_height, fg);
    }
    char prompt[OBS_REPL_LINE_CAP];
    const char *status = model->has_pending || model->executing ? "[compiling] " : "> ";
    size_t prefix = strlen(status);
    memcpy(prompt, status, prefix);
    fit_line(prompt + prefix, sizeof(prompt) - prefix, model->input,
             columns > prefix ? columns - prefix : 0);
    ui_draw_line(&s_window, x, y + 34.0f, x + width, y + 34.0f, CT.border);
    ui_draw_text(&s_window, font, prompt, x, y + 12.0f, CT.text);
    ui_window_end_render(&s_window);
}
