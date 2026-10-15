/// vertico — editor subsystem
#include "vertico.h"
#include "editor.h"
#include "editor_frames.h"
#include "scene_buffers.h"
#include "material_browser.h"
#include "text_editor.h"
#include "keychords.h"
#include "live_editor.h"
#include "bindings.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/// Implementation

extern void set_active_text_input(void (*cb)(char));

Vertico vertico = {0};
double vertico_last_key_time = 0.0;
size_t vertico_cursor = 0;
static const ObsInteractiveCommand *s_reading_command;
static size_t s_interactive_spec_cursor;
static size_t s_interactive_text_count;
static char s_interactive_text_args[OBS_COMMAND_SPEC_MAX][VERTICO_INPUT_BUFFER_SIZE];
static bool s_vertico_selected;

static void interactive_text_submitted(const char *text, void *userdata);

static void next_interactive_argument(void) {
    if (!s_reading_command) return;
    const char *spec = s_reading_command->interactive_spec;
    while (spec[s_interactive_spec_cursor]) {
        char code = spec[s_interactive_spec_cursor++];
        if (code == 'p' || code == 'P') continue;
        if (code == 's' || code == 'n' || code == 'b' || code == 'B' ||
            code == 'f') {
            char prompt[128] = {0};
            size_t n = 0;
            while (spec[s_interactive_spec_cursor] &&
                   spec[s_interactive_spec_cursor] != '\n') {
                if (n + 1 < sizeof(prompt))
                    prompt[n++] = spec[s_interactive_spec_cursor];
                ++s_interactive_spec_cursor;
            }
            if (spec[s_interactive_spec_cursor] == '\n')
                ++s_interactive_spec_cursor;
            if (!n) {
                const char *fallback = code == 'n' ? "Number: " :
                    code == 'b' ? "Buffer: " : code == 'B' ?
                    "Switch to buffer: " : code == 'f' ? "File: " : "String: ";
                snprintf(prompt, sizeof(prompt), "%s", fallback);
            }
            if (s_interactive_text_count >= OBS_COMMAND_SPEC_MAX) {
                s_reading_command = NULL;
                return;
            }
            if (code == 'b' || code == 'B' || code == 'f')
                vertico_read_completion(prompt, interactive_text_submitted,
                                        NULL, code);
            else
                vertico_read_text(prompt, interactive_text_submitted, NULL);
            return;
        }

    }
    const ObsInteractiveCommand *command = s_reading_command;
    s_reading_command = NULL;
    const char *text_args[OBS_COMMAND_SPEC_MAX];
    for (size_t i = 0; i < s_interactive_text_count; ++i)
        text_args[i] = s_interactive_text_args[i];
    obs_monad_invoke_interactive_command_text(
        command, text_args, s_interactive_text_count);
}

static void interactive_text_submitted(const char *text, void *userdata) {
    (void)userdata;
    if (!s_reading_command || !text ||
        s_interactive_text_count >= OBS_COMMAND_SPEC_MAX) {
        s_reading_command = NULL;
        return;
    }
    size_t length = strlen(text);
    if (length >= VERTICO_INPUT_BUFFER_SIZE) {
        s_reading_command = NULL;
        return;
    }
    memcpy(s_interactive_text_args[s_interactive_text_count], text, length + 1);
    ++s_interactive_text_count;
    next_interactive_argument();
}

void vertico_read_interactive_command(const ObsInteractiveCommand *command) {
    if (!command) return;
    s_reading_command = command;
    s_interactive_spec_cursor = 0;
    s_interactive_text_count = 0;
    next_interactive_argument();
}

void vertico_cursor_left(void) {
    if (vertico_cursor > 0) vertico_cursor--;
    vertico_last_key_time = glfwGetTime();
}

void vertico_cursor_right(void) {
    if (vertico_cursor < vertico.input_length) vertico_cursor++;
    vertico_last_key_time = glfwGetTime();
}

void vertico_cursor_start(void) {
    vertico_cursor = 0;
    vertico_last_key_time = glfwGetTime();
}

void vertico_cursor_end(void) {
    vertico_cursor = vertico.input_length;
    vertico_last_key_time = glfwGetTime();
}

void vertico_delete_char(void) {
    if (vertico_cursor < vertico.input_length) {
        memmove(&vertico.input[vertico_cursor], &vertico.input[vertico_cursor+1], vertico.input_length - vertico_cursor);
        vertico.input_length--;
        vertico_filter_candidates();
        vertico_last_key_time = glfwGetTime();
    }
}

void vertico_kill_line(void) {
    if (vertico_cursor < vertico.input_length) {
        vertico.input[vertico_cursor] = '\0';
        vertico.input_length = vertico_cursor;
        vertico_filter_candidates();
        vertico_last_key_time = glfwGetTime();
    }
}

void vertico_init() {
    memset(&vertico, 0, sizeof(Vertico));
    vertico.is_active = false;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
    vertico.header_resizing = false;
    vertico.material_dragging = false;
    vertico.count = 10;
    vertico.width = 640.0f;

    keymap_init(&vertico.vertico_keymap);
    if (!obs_monad_live_editor_register_map("vertico", &vertico.vertico_keymap))
        fprintf(stderr, "Could not register live Vertico keymap\n");
#define REGISTER_VERTICO_ACTION(name, fn) \
    if (!obs_monad_live_editor_register_action(name, fn)) \
        fprintf(stderr, "Could not register live editor action: %s\n", name)
    REGISTER_VERTICO_ACTION("vertico-backspace", vertico_backspace);
    REGISTER_VERTICO_ACTION("vertico-select", vertico_select);
    REGISTER_VERTICO_ACTION("vertico-quit", vertico_quit);
    REGISTER_VERTICO_ACTION("vertico-next-theme", nextTheme);
    REGISTER_VERTICO_ACTION("vertico-previous-theme", previousTheme);
    REGISTER_VERTICO_ACTION("vertico-cursor-left", vertico_cursor_left);
    REGISTER_VERTICO_ACTION("vertico-cursor-right", vertico_cursor_right);
    REGISTER_VERTICO_ACTION("vertico-cursor-start", vertico_cursor_start);
    REGISTER_VERTICO_ACTION("vertico-cursor-end", vertico_cursor_end);
    REGISTER_VERTICO_ACTION("vertico-delete-char", vertico_delete_char);
    REGISTER_VERTICO_ACTION("vertico-kill-line", vertico_kill_line);
#undef REGISTER_VERTICO_ACTION
}

void vertico_cleanup() {
    keymap_free(&vertico.vertico_keymap);
}

void vertico_activate(const char* category, VerticoSelectCallback callback) {
    obs_editor_frames_set_vertico_owner();
    int64_t configured_width = 0;
    int64_t configured_count = 0;
    int64_t configured_scroll_margin = 0;
    bool configured_top = false;
    bool configured_resize_pixelwise = false;
    if (obs_monad_live_editor_vertico_options(
            &configured_width, &configured_count, &configured_scroll_margin,
            &configured_top, &configured_resize_pixelwise)) {
        if (configured_width > 0 && configured_width <= INT32_MAX)
            vertico.width = (float)configured_width;
        if (configured_count > 0 && configured_count <= VERTICO_MAX_CANDIDATES)
            vertico.count = (int)configured_count;
        if (configured_scroll_margin >= 0 &&
            configured_scroll_margin <= VERTICO_MAX_CANDIDATES)
            vertico.scroll_margin = (int)configured_scroll_margin;
        vertico.top = configured_top;
        vertico.resize_pixelwise = configured_resize_pixelwise;
    }
    const float line_height = editor.font
        ? editor.font->ascent - editor.font->descent + 5.0f
        : 22.0f;
    vertico.material_preview_mode =
        category && strcmp(category, "materials") == 0;
    const float row_height = vertico.material_preview_mode
        ? obs_material_browser_row_height() : line_height;
    const float screen_height = (float)context.drawExtent.height;
    if (screen_height > 0.0f) {
        const int max_visible =
            (int)((screen_height - 40.0f) / row_height);
        if (max_visible > 0 && vertico.count > max_visible)
            vertico.count = max_visible;
    }
    editor.panels[PANEL_TOP].size =
        40.0f + row_height * (float)vertico.count;

    vertico.is_active = true;
    keychord_set_prefix_capture_enabled(false);
    vertico.text_input_mode = false;
    vertico.on_select = callback;
    vertico.on_text_submit = NULL;
    vertico.text_submit_userdata = NULL;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
    vertico.header_resizing = false;
    vertico.material_dragging = false;
    vertico.input_length = 0;
    vertico.input[0] = '\0';
    vertico_cursor = 0;

    if (category) {
        strncpy(vertico.category, category, sizeof(vertico.category) - 1);
        vertico.category[sizeof(vertico.category) - 1] = '\0';
    } else {
        strcpy(vertico.category, "select");
    }

    keymap_stack_push(&vertico.vertico_keymap);

    set_active_text_input(vertico_insert_char);

    // Filter candidates initially
    vertico_filter_candidates();
    vertico_last_key_time = glfwGetTime();
    editor_open_panel(PANEL_TOP);
}

void vertico_read_text(const char *prompt, VerticoTextSubmitCallback callback,
                       void *userdata) {
    vertico_clear_candidates();
    vertico_activate(prompt && *prompt ? prompt : "Input", NULL);
    vertico.text_input_mode = true;
    vertico.text_completion_mode = false;
    vertico.on_text_submit = callback;
    vertico.text_submit_userdata = userdata;
}

void vertico_read_completion(const char *prompt,
                             VerticoTextSubmitCallback callback,
                             void *userdata, char code) {
    vertico_read_text(prompt, callback, userdata);
    vertico.text_completion_mode = true;
    if (code == 'b' || code == 'B') {
        if (g_text_editor.buf.filename[0])
            vertico_add_candidate(g_text_editor.buf.filename,
                                  "current buffer", NULL);
        return;
    }
    DIR *directory = opendir(".");
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           vertico.candidate_count < VERTICO_MAX_CANDIDATES) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;
        struct stat info;
        if (stat(entry->d_name, &info) != 0) continue;
        char candidate[VERTICO_INPUT_BUFFER_SIZE];
        int length = snprintf(candidate, sizeof(candidate), "%s%s",
                              entry->d_name, S_ISDIR(info.st_mode) ? "/" : "");
        if (length < 0 || (size_t)length >= sizeof(candidate)) continue;
        vertico_add_candidate(candidate, S_ISDIR(info.st_mode) ? "directory" : "file",
                              NULL);
    }
    closedir(directory);
    vertico_filter_candidates();
}

void vertico_quit() {
    if (!vertico.is_active) return;

    vertico.is_active = false;
    vertico.text_input_mode = false;
    vertico.text_completion_mode = false;
    vertico.header_resizing = false;
    vertico.material_dragging = false;
    keychord_set_prefix_capture_enabled(true);
    if (!s_vertico_selected) {
        s_reading_command = NULL;
        keychord_clear_prefix_arg();
    }
    set_active_text_input(NULL);

    keymap_stack_pop();
    obs_editor_frames_clear_vertico_owner();

    editor_close_panel(PANEL_TOP);
}

void vertico_clear_candidates() {
    vertico.candidate_count = 0;
    vertico.filtered_count = 0;
}

void vertico_add_candidate(const char* text, const char* annotation, void* data) {
    if (vertico.candidate_count >= VERTICO_MAX_CANDIDATES) return;

    VerticoCandidate* candidate = &vertico.candidates[vertico.candidate_count++];
    strncpy(candidate->text, text, sizeof(candidate->text) - 1);
    candidate->text[sizeof(candidate->text) - 1] = '\0';

    if (annotation) {
        strncpy(candidate->annotation, annotation, sizeof(candidate->annotation) - 1);
        candidate->annotation[sizeof(candidate->annotation) - 1] = '\0';
    } else {
        candidate->annotation[0] = '\0';
    }

    candidate->data = data;
    candidate->score = 0.0f;
}

float vertico_fuzzy_match(const char* pattern, const char* text) {
    if (!pattern || !text) return 0.0f;
    if (pattern[0] == '\0') return 1.0f; // Empty pattern matches everything

    char pattern_lower[VERTICO_INPUT_BUFFER_SIZE];
    char text_lower[VERTICO_INPUT_BUFFER_SIZE];
    size_t pattern_length = strlen(pattern);
    size_t text_length = strlen(text);
    if (pattern_length >= sizeof(pattern_lower))
        pattern_length = sizeof(pattern_lower) - 1;
    if (text_length >= sizeof(text_lower))
        text_length = sizeof(text_lower) - 1;
    for (size_t i = 0; i < pattern_length; ++i)
        pattern_lower[i] = (char)tolower((unsigned char)pattern[i]);
    for (size_t i = 0; i < text_length; ++i)
        text_lower[i] = (char)tolower((unsigned char)text[i]);
    pattern_lower[pattern_length] = '\0';
    text_lower[text_length] = '\0';

    float score = 1.0f;
    size_t word_count = 0;
    char *word = pattern_lower;
    while (*word) {
        while (*word == ' ') ++word;
        if (!*word) break;
        char *end = word;
        while (*end && *end != ' ') ++end;
        if (*end) *end++ = '\0';
        ++word_count;
        char *position = strstr(text_lower, word);
        if (!position || text_length == 0) return 0.0f;
        score += 1.0f -
            ((float)(position - text_lower) / (float)text_length) * 0.3f;
        word = end;
    }
    if (word_count == 0) return 1.0f;

    return score / (float)word_count;
}

// Compare function for qsort
static int compare_candidates(const void* a, const void* b) {
    const VerticoCandidate* ca = (const VerticoCandidate*)a;
    const VerticoCandidate* cb = (const VerticoCandidate*)b;

    // Higher scores first
    if (ca->score > cb->score) return -1;
    if (ca->score < cb->score) return 1;

    // If scores are equal, sort alphabetically
    return strcmp(ca->text, cb->text);
}

void vertico_filter_candidates() {
    vertico.filtered_count = 0;
    ObsMonadCompletionContext completion = {0};
    const bool has_monad_strategy =
        obs_monad_live_editor_completion_begin(
            vertico.category, vertico.input, &completion);

    for (size_t i = 0; i < vertico.candidate_count; i++) {
        float score = 0.0f;
        const bool scored = has_monad_strategy
            ? obs_monad_live_editor_completion_score(
                  &completion, vertico.candidates[i].text, &score)
            : (score = vertico_fuzzy_match(
                   vertico.input, vertico.candidates[i].text), true);
        if (scored && score > 0.0f) {
            vertico.filtered[vertico.filtered_count] = vertico.candidates[i];
            vertico.filtered[vertico.filtered_count].score = score;
            vertico.filtered_count++;
        }
    }
    if (has_monad_strategy)
        obs_monad_live_editor_completion_end(&completion);

    // Sort by score
    qsort(vertico.filtered, vertico.filtered_count, sizeof(VerticoCandidate), compare_candidates);

    // Reset selection if out of bounds
    if (vertico.selected_index >= vertico.filtered_count) {
        vertico.selected_index = vertico.filtered_count > 0 ? vertico.filtered_count - 1 : 0;
    }

    if (vertico.count <= 0) return;
    size_t offset = (size_t)vertico.scroll_offset;
    if (vertico.selected_index < offset) offset = vertico.selected_index;
    const size_t visible = (size_t)vertico.count;
    const size_t margin = (size_t)vertico.scroll_margin < visible
        ? (size_t)vertico.scroll_margin : visible - 1;
    const size_t keep_position = visible - margin - 1;
    if (vertico.selected_index >= offset + keep_position)
        offset = vertico.selected_index - keep_position;
    const size_t maximum_offset = vertico.filtered_count > visible
        ? vertico.filtered_count - visible : 0;
    if (offset > maximum_offset) offset = maximum_offset;
    vertico.scroll_offset = (int)offset;
}

size_t vertico_filtered_count(void) {
    return vertico.filtered_count;
}

size_t vertico_selection_index(void) {
    return vertico.selected_index;
}

int32_t vertico_scroll_offset(void) {
    return vertico.scroll_offset;
}

int32_t vertico_visible_count(void) {
    return vertico.count > 0 ? vertico.count : 0;
}

bool vertico_set_position(size_t selected_index, int32_t scroll_offset) {
    if (!vertico.is_active || vertico.filtered_count == 0 ||
        vertico.count <= 0 ||
        selected_index >= vertico.filtered_count ||
        scroll_offset < 0)
        return false;

    const size_t visible_count = (size_t)vertico.count;
    const size_t maximum_offset =
        vertico.filtered_count > visible_count
            ? vertico.filtered_count - visible_count : 0;
    const size_t offset = (size_t)scroll_offset;
    if (offset > maximum_offset || selected_index < offset ||
        selected_index - offset >= visible_count)
        return false;

    vertico.selected_index = selected_index;
    vertico.scroll_offset = scroll_offset;
    return true;
}

void vertico_select() {
    if (!vertico.is_active) return;
    if (vertico.text_input_mode) {
        VerticoTextSubmitCallback callback = vertico.on_text_submit;
        void *userdata = vertico.text_submit_userdata;
        char text[VERTICO_INPUT_BUFFER_SIZE];
        memcpy(text, vertico.input, vertico.input_length + 1);
        if (vertico.text_completion_mode &&
            vertico.filtered_count > 0 &&
            vertico.selected_index < vertico.filtered_count) {
            const VerticoCandidate *candidate =
                &vertico.filtered[vertico.selected_index];
            memcpy(text, candidate->text, strlen(candidate->text) + 1);
            (void)obs_monad_live_editor_completion_record(
                vertico.category, text);
        }
        s_vertico_selected = true;
        vertico_quit();
        s_vertico_selected = false;
        if (callback) callback(text, userdata);
        return;
    }
    if (vertico.filtered_count == 0 || !vertico.on_select ||
        vertico.selected_index >= vertico.filtered_count) return;
    VerticoSelectCallback callback = vertico.on_select;
    const VerticoCandidate *candidate =
        &vertico.filtered[vertico.selected_index];
    void *data = candidate->data;
    (void)obs_monad_live_editor_completion_record(
        vertico.category, candidate->text);
    s_vertico_selected = true;
    vertico_quit();
    s_vertico_selected = false;
    callback(data);
}

void vertico_insert_char(char c) {
    if (vertico.input_length < VERTICO_INPUT_BUFFER_SIZE - 1) {
        memmove(&vertico.input[vertico_cursor + 1], &vertico.input[vertico_cursor], vertico.input_length - vertico_cursor + 1);
        vertico.input[vertico_cursor] = c;
        vertico_cursor++;
        vertico.input_length++;
        vertico_filter_candidates();
        vertico_last_key_time = glfwGetTime();
    }
}

void vertico_backspace() {
    if (vertico_cursor > 0) {
        memmove(&vertico.input[vertico_cursor-1], &vertico.input[vertico_cursor], vertico.input_length - vertico_cursor + 1);
        vertico_cursor--;
        vertico.input_length--;
        vertico_filter_candidates();
        vertico_last_key_time = glfwGetTime();
    }
}

void vertico_clear_input() {
    vertico.input_length = 0;
    vertico.input[0] = '\0';
    vertico_cursor = 0;
    vertico_filter_candidates();
    vertico_last_key_time = glfwGetTime();
}

static void keybinding_selected(void* data) {
    if (obs_monad_interactive_command_is(data)) {
        obs_monad_invoke_interactive_command(data);
        return;
    }
    KeyChordBinding* binding = (KeyChordBinding*)data;
    if (binding) {
        keychord_invoke_binding(binding);
    }
}

void vertico_show_commands() {
    vertico_clear_candidates();

    for (size_t i = 0; i < obs_monad_interactive_command_count(); ++i) {
        const ObsInteractiveCommand *command =
            obs_monad_interactive_command_at(i);
        if (command)
            vertico_add_candidate(command->name, command->interactive_spec,
                                  (void *)command);
    }

    KeyChordBinding *active[VERTICO_MAX_CANDIDATES];
    size_t active_count =
        keychord_collect_active_bindings(active, VERTICO_MAX_CANDIDATES);
    for (size_t i = 0; i < active_count; i++) {
        KeyChordBinding* binding = active[i];
        const char* name = binding->description;
        if (!name || !*name) continue;
        const char *command_name = keychord_binding_command_name(binding);
        bool listed_as_interactive = false;
        if (command_name) {
            for (size_t j = 0; j < obs_monad_interactive_command_count(); ++j) {
                const ObsInteractiveCommand *command =
                    obs_monad_interactive_command_at(j);
                if (command && strcmp(command->name, command_name) == 0) {
                    listed_as_interactive = true;
                    break;
                }
            }
        }
        if (listed_as_interactive) continue;
        vertico_add_candidate(name, binding->notation, binding);
    }

    vertico_activate("command", keybinding_selected);
}

void vertico_show_keybindings() {
    vertico_clear_candidates();

    KeyChordBinding *active[VERTICO_MAX_CANDIDATES];
    size_t active_count =
        keychord_collect_active_bindings(active, VERTICO_MAX_CANDIDATES);
    for (size_t i = 0; i < active_count; i++) {
        KeyChordBinding* binding = active[i];
        const char* key_text = binding->notation ? binding->notation : "???";
        const char* description = binding->description ? binding->description : "";

        vertico_add_candidate(key_text, description, binding);
    }

    vertico_activate("keymap", keybinding_selected);
}

static bool s_scene_buffer_create_frame;

static void scene_buffer_selected(void *data) {
    ObsSceneBuffer *buffer = data;
    if (!buffer) return;
    bool ok = s_scene_buffer_create_frame
        ? obs_editor_frame_create(buffer, true)
        : obs_editor_frame_set_buffer(obs_editor_frame_current(), buffer);
    s_scene_buffer_create_frame = false;
    if (!ok)
        message(MSG_ERROR, "Could not activate scene buffer");
}

static void scene_file_submitted(const char *path, void *userdata) {
    const bool create_frame = (uintptr_t)userdata != 0;
    if (!path || !*path) return;
    ObsSceneBuffer *buffer = obs_scene_buffer_open(path);
    if (!buffer) {
        char text[128];
        snprintf(text, sizeof(text), "Could not open scene: %.96s", path);
        message(MSG_ERROR, text);
        return;
    }
    bool ok = create_frame
        ? obs_editor_frame_create(buffer, true)
        : obs_editor_frame_set_buffer(obs_editor_frame_current(), buffer);
    if (!ok) {
        char text[128];
        snprintf(text, sizeof(text), "Could not activate scene: %.96s", path);
        message(MSG_ERROR, text);
    }
}

bool vertico_show_scene_buffers(bool create_frame) {
    if (!obs_scene_buffer_count() || !obs_editor_frame_current()) return false;
    s_scene_buffer_create_frame = create_frame;
    vertico_clear_candidates();
    for (ObsSceneBuffer *buffer = obs_scene_buffer_first(); buffer;
         buffer = obs_scene_buffer_next(buffer)) {
        const ObsSceneCacheState cache = obs_scene_buffer_cache_state(buffer);
        const char *cache_label = cache == OBS_SCENE_CACHE_HIT
            ? "cached" : cache == OBS_SCENE_CACHE_MISS
            ? "not cached" : "not applicable";
        char detail[VERTICO_INPUT_BUFFER_SIZE];
        const char *path = obs_scene_buffer_path(buffer);
        snprintf(detail, sizeof(detail), "%zu meshes · %zu instances · %s",
                 obs_scene_buffer_mesh_count(buffer),
                 obs_scene_buffer_instance_count(buffer),
                 path && *path ? path : "in-memory scene");
        char annotation[sizeof(vertico.candidates[0].annotation)];
        if (!obs_monad_live_editor_marginalia_scene(
                cache_label, detail, annotation, sizeof(annotation)))
            snprintf(annotation, sizeof(annotation), "%.32s · %.218s",
                     cache_label, detail);
        vertico_add_candidate(obs_scene_buffer_name(buffer), annotation,
                              buffer);
    }
    vertico_activate(create_frame ? "scene in new frame" : "scene buffer",
                     scene_buffer_selected);
    return true;
}

bool vertico_show_scene_file(bool create_frame) {
    if (!obs_editor_frame_current()) return false;
    vertico_read_completion(create_frame ? "Open scene in new frame: "
                                         : "Open scene: ",
                            scene_file_submitted,
                            (void *)(uintptr_t)create_frame, 'f');
    return true;
}

static bool variable_candidate(const ObsMonadVariableView *variable,
                               void *context) {
    (void)context;
    if (!variable || vertico.candidate_count >= VERTICO_MAX_CANDIDATES)
        return false;
    char annotation[sizeof(vertico.candidates[0].annotation)];
    if (!obs_monad_live_editor_marginalia_variable(
            variable->type, variable->value, variable->documentation,
            annotation, sizeof(annotation))) {
        snprintf(annotation, sizeof(annotation), "%.48s · %.48s · %.140s",
                 variable->type, variable->value, variable->documentation);
    }
    const size_t index = vertico.candidate_count;
    vertico_add_candidate(variable->name, annotation, NULL);
    if (vertico.candidate_count > index)
        vertico.candidates[index].data = vertico.candidates[index].text;
    return true;
}

static void variable_selected(void *data) {
    const char *name = data;
    if (name && *name) message(MSG_INFO, name);
}

static void image_file_submitted(const char *path, void *userdata) {
    (void)userdata;
    if (!path || !*path) return;
    if (!obs_editor_frame_create_image(path, true))
        message(MSG_ERROR, "Could not open image editor frame");
}

static void material_selected(void *data) {
    if (obs_material_browser_contains(data))
        obs_material_browser_select((const ObsMaterialBrowserEntry *)data);
}

bool vertico_show_materials(void) {
    if (!scene) return false;
    obs_material_browser_begin();
    vertico_clear_candidates();
    for (size_t i = 0; i < scene->meshes.count &&
                        vertico.candidate_count < VERTICO_MAX_CANDIDATES; ++i) {
        const ObsMaterialBrowserEntry *entry =
            obs_material_browser_add_mesh(i);
        if (!entry) continue;
        char annotation[256];
        obs_material_browser_annotation(entry, annotation,
                                        sizeof(annotation));
        vertico_add_candidate(obs_material_browser_name(entry), annotation,
                              (void *)entry);
    }
    for (size_t i = 0; i < scene->materials.count &&
                        vertico.candidate_count < VERTICO_MAX_CANDIDATES; ++i) {
        const ObsMaterialBrowserEntry *entry =
            obs_material_browser_add_asset(i);
        if (!entry) continue;
        char annotation[256];
        obs_material_browser_annotation(entry, annotation,
                                        sizeof(annotation));
        vertico_add_candidate(obs_material_browser_name(entry), annotation,
                              (void *)entry);
    }
    for (size_t i = 0; i < scene->sdf_count &&
                        vertico.candidate_count < VERTICO_MAX_CANDIDATES; ++i) {
        const ObsMaterialBrowserEntry *entry =
            obs_material_browser_add_sdf(i);
        if (!entry) continue;
        char annotation[256];
        obs_material_browser_annotation(entry, annotation,
                                        sizeof(annotation));
        vertico_add_candidate(obs_material_browser_name(entry), annotation,
                              (void *)entry);
    }
    vertico_activate("materials", material_selected);
    return true;
}

bool vertico_show_image_file(void) {
    if (!obs_editor_frame_current()) return false;
    vertico_read_completion("Image file: ", image_file_submitted, NULL, 'f');
    size_t write = 0;
    for (size_t i = 0; i < vertico.candidate_count; ++i) {
        VerticoCandidate *candidate = &vertico.candidates[i];
        const char *extension = strrchr(candidate->text, '.');
        if (!extension ||
            (strcasecmp(extension, ".png") != 0 &&
             strcasecmp(extension, ".jpg") != 0 &&
             strcasecmp(extension, ".jpeg") != 0 &&
             strcasecmp(extension, ".webp") != 0 &&
             strcasecmp(extension, ".bmp") != 0 &&
             strcasecmp(extension, ".tga") != 0 &&
             strcasecmp(extension, ".gif") != 0 &&
             strcasecmp(extension, ".tif") != 0 &&
             strcasecmp(extension, ".tiff") != 0))
            continue;
        if (write != i) vertico.candidates[write] = *candidate;
        ++write;
    }
    vertico.candidate_count = write;
    vertico_filter_candidates();
    return true;
}

bool vertico_mouse_button(int button, int action, double x, double y) {
    if (!vertico.is_active || button != GLFW_MOUSE_BUTTON_LEFT)
        return false;
    if (action == GLFW_RELEASE && vertico.header_resizing) {
        vertico.header_resizing = false;
        return true;
    }
    if (action == GLFW_RELEASE && vertico.material_dragging) {
        vertico.material_dragging = false;
        return true;
    }
    if (action != GLFW_PRESS) return false;
    const float screen_height = (float)context.drawExtent.height;
    const float header_height = 32.0f;
    const float panel_height = editor.panels[PANEL_TOP].size;
    const float panel_width = fminf(
        vertico.width, (float)context.drawExtent.width - 40.0f);
    const float panel_x = ((float)context.drawExtent.width - panel_width) * 0.5f;
    const float header_y = vertico.top
        ? screen_height - header_height : panel_height - header_height;
    if (x >= panel_x && x <= panel_x + panel_width &&
        y >= header_y && y <= header_y + header_height) {
        vertico.header_resizing = true;
        vertico.resize_start_y = y;
        vertico.resize_start_height = panel_height;
        vertico.resize_start_count = vertico.count;
        vertico.resize_row_height = vertico.material_preview_mode
            ? obs_material_browser_row_height()
            : (editor.font
                   ? editor.font->ascent - editor.font->descent + 5.0f
                   : 22.0f);
        return true;
    }
    if (vertico.text_input_mode) return false;
    if (vertico.row_height <= 0.0f) return false;
    if (action != GLFW_PRESS || y < vertico.row_bottom ||
        y >= vertico.row_start_y || x < vertico.row_x ||
        x > vertico.row_x + vertico.row_width)
        return false;
    const size_t row =
        (size_t)((vertico.row_start_y - y) / vertico.row_height);
    const size_t index = (size_t)vertico.scroll_offset + row;
    if (index >= vertico.filtered_count ||
        !vertico_set_position(index, vertico.scroll_offset))
        return false;
    if (vertico.material_preview_mode &&
        x < vertico.row_x + vertico.preview_width) {
        vertico.material_dragging = true;
        vertico.material_drag_index = index;
        vertico.material_drag_x = x;
        vertico.material_drag_y = y;
        return true;
    }
    vertico_select();
    return true;
}

bool vertico_mouse_move(double x, double y) {
    if (vertico.header_resizing) {
        const float row_height = vertico.resize_row_height;
        if (row_height <= 0.0f) return true;
        const float direction = vertico.top ? -1.0f : 1.0f;
        const float delta = direction *
            (float)(y - vertico.resize_start_y);
        const float minimum_height = 40.0f + row_height;
        const float maximum_height =
            fmaxf(minimum_height, (float)context.drawExtent.height);
        float panel_height;
        if (vertico.resize_pixelwise) {
            panel_height = fminf(
                fmaxf(vertico.resize_start_height + delta, minimum_height),
                maximum_height);
            vertico.count = (int)floorf((panel_height - 40.0f) / row_height);
            panel_height = 40.0f +
                fmaxf((panel_height - 40.0f), 0.0f);
        } else {
            const int steps = (int)(delta / row_height);
            int count = vertico.resize_start_count + steps;
            const int maximum_count =
                (int)floorf((maximum_height - 40.0f) / row_height);
            if (count < 1) count = 1;
            if (count > maximum_count) count = maximum_count;
            vertico.count = count;
            panel_height = 40.0f + row_height * (float)count;
        }
        editor.panels[PANEL_TOP].size = panel_height;
        vertico_filter_candidates();
        return true;
    }
    if (!vertico.material_dragging ||
        vertico.material_drag_index >= vertico.filtered_count)
        return false;
    VerticoCandidate *candidate =
        &vertico.filtered[vertico.material_drag_index];
    if (obs_material_browser_contains(candidate->data)) {
        obs_material_browser_drag(
            (ObsMaterialBrowserEntry *)candidate->data,
            (float)(x - vertico.material_drag_x),
            (float)(vertico.material_drag_y - y));
    }
    vertico.material_drag_x = x;
    vertico.material_drag_y = y;
    return true;
}

bool vertico_show_variables(void) {
    vertico_clear_candidates();
    bool truncated = false;
    if (!obs_monad_live_editor_visit_variables(variable_candidate, NULL,
                                               &truncated)) {
        message(MSG_ERROR, "Monad variable catalog is unavailable");
        return false;
    }
    if (truncated)
        message(MSG_WARNING, "Variable list truncated at 1024 entries");
    vertico_activate("variables", variable_selected);
    return true;
}
