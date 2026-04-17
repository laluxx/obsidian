#include "vertico.h"
#include "editor.h"
#include "keychords.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern void set_active_text_input(void (*cb)(char));

Vertico vertico = {0};
double vertico_last_key_time = 0.0;
size_t vertico_cursor = 0;

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
    vertico.count = 19;
    vertico.cycle = false;

    keymap_init(&vertico.vertico_keymap);

    keychord_bind(&vertico.vertico_keymap, "C-n",    vertico_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-p",    vertico_previous,   "Previous candidate", PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "DEL",    vertico_backspace,  "Vertico backspace",  PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "RET",    vertico_select,     "Select candidate",   PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-g",    vertico_quit,       "Vertico quit",       PRESS);
    keychord_bind(&vertico.vertico_keymap, "<down>", vertico_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "<up>",   vertico_previous,   "Previous candidate", PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "M-=",    nextTheme,          "Next theme",         PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "M--",    previousTheme,      "Previous theme",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-c p",  vertico_first,      "Vertico first",      PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-c n",  vertico_last,       "Vertico last",       PRESS);

    keychord_bind(&vertico.vertico_keymap, "C-b", vertico_cursor_left,  "Cursor left",  PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-f", vertico_cursor_right, "Cursor right", PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-a", vertico_cursor_start, "Cursor start", PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-e", vertico_cursor_end,   "Cursor end",   PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-d", vertico_delete_char,  "Delete char",  PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-k", vertico_kill_line,    "Kill line",    PRESS);
}

void vertico_cleanup() {
    keymap_free(&vertico.vertico_keymap);
}

void vertico_activate(const char* category, VerticoSelectCallback callback) {
    vertico.is_active = true;
    vertico.on_select = callback;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
    vertico.input_length = 0;
    vertico.input[0] = '\0';
    vertico_cursor = 0;

    if (category) {
        strncpy(vertico.category, category, sizeof(vertico.category) - 1);
        vertico.category[sizeof(vertico.category) - 1] = '\0';
    } else {
        strcpy(vertico.category, "select");
    }

    // Save current keymap and swap to vertico keymap
    vertico.saved_keymap = &keymap;

    // Copy the global keymap pointer to swap it
    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = vertico.vertico_keymap;
    vertico.vertico_keymap = temp;

    set_active_text_input(vertico_insert_char);

    // Filter candidates initially
    vertico_filter_candidates();
    vertico_last_key_time = glfwGetTime();
    editor_open_panel(PANEL_TOP);
}

void vertico_quit() {
    if (!vertico.is_active) return;

    vertico.is_active = false;
    set_active_text_input(NULL);

    // Restore the global keymap
    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = vertico.vertico_keymap;
    vertico.vertico_keymap = temp;

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

    // Convert both to lowercase for case-insensitive matching
    char pattern_lower[256], text_lower[256];
    strncpy(pattern_lower, pattern, sizeof(pattern_lower) - 1);
    strncpy(text_lower, text, sizeof(text_lower) - 1);
    pattern_lower[sizeof(pattern_lower) - 1] = '\0';
    text_lower[sizeof(text_lower) - 1] = '\0';

    for (char* p = pattern_lower; *p; p++) *p = tolower(*p);
    for (char* p = text_lower; *p; p++) *p = tolower(*p);

    // Split pattern into words
    char* pattern_words[32];
    int word_count = 0;
    char* pattern_copy = strdup(pattern_lower);
    char* token = strtok(pattern_copy, " ");

    while (token && word_count < 32) {
        pattern_words[word_count++] = token;
        token = strtok(NULL, " ");
    }

    if (word_count == 0) {
        free(pattern_copy);
        return 1.0f;
    }

    // Check if all words are present in text
    float score = 1.0f;
    int matches = 0;

    for (int i = 0; i < word_count; i++) {
        if (strstr(text_lower, pattern_words[i]) != NULL) {
            matches++;
            // Bonus for earlier matches
            char* pos = strstr(text_lower, pattern_words[i]);
            float position_bonus = 1.0f - ((float)(pos - text_lower) / strlen(text_lower)) * 0.3f;
            score += position_bonus;
        }
    }

    free(pattern_copy);

    // Only return non-zero score if all words matched
    if (matches != word_count) return 0.0f;

    // Normalize score
    return score / word_count;
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

    // Filter and score candidates
    for (size_t i = 0; i < vertico.candidate_count; i++) {
        float score = vertico_fuzzy_match(vertico.input, vertico.candidates[i].text);

        if (score > 0.0f) {
            vertico.filtered[vertico.filtered_count] = vertico.candidates[i];
            vertico.filtered[vertico.filtered_count].score = score;
            vertico.filtered_count++;
        }
    }

    // Sort by score
    qsort(vertico.filtered, vertico.filtered_count, sizeof(VerticoCandidate), compare_candidates);

    // Reset selection if out of bounds
    if (vertico.selected_index >= (int)vertico.filtered_count) {
        vertico.selected_index = vertico.filtered_count > 0 ? vertico.filtered_count - 1 : 0;
    }

    // Adjust scroll to keep selection visible
    if (vertico.selected_index < vertico.scroll_offset) {
        vertico.scroll_offset = vertico.selected_index;
    }
    if (vertico.selected_index >= vertico.scroll_offset + vertico.count) {
        vertico.scroll_offset = vertico.selected_index - vertico.count + 1;
    }
}

void vertico_previous() {
    if (vertico.filtered_count == 0) return;

    if (vertico.selected_index > 0) {
        vertico.selected_index--;
        if (vertico.selected_index < vertico.scroll_offset) {
            vertico.scroll_offset--;
        }
    } else if (vertico.cycle) {
        // Cycle to end
        vertico.selected_index = vertico.filtered_count - 1;
        vertico.scroll_offset = vertico.filtered_count - vertico.count;
        if (vertico.scroll_offset < 0) vertico.scroll_offset = 0;
    }
}

void vertico_next() {
    if (vertico.filtered_count == 0) return;

    if (vertico.selected_index < (int)vertico.filtered_count - 1) {
        vertico.selected_index++;
        if (vertico.selected_index >= vertico.scroll_offset + vertico.count) {
            vertico.scroll_offset++;
        }
    } else if (vertico.cycle) {
        // Cycle to beginning
        vertico.selected_index = 0;
        vertico.scroll_offset = 0;
    }
}

void vertico_first() {
    if (vertico.filtered_count == 0) return;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
}

void vertico_last() {
    if (vertico.filtered_count == 0) return;
    vertico.selected_index = vertico.filtered_count - 1;
    vertico.scroll_offset = vertico.filtered_count - vertico.count;
    if (vertico.scroll_offset < 0) vertico.scroll_offset = 0;
}

void vertico_select() {
    if (!vertico.is_active || vertico.filtered_count == 0) return;

    if (vertico.on_select && vertico.selected_index >= 0 &&
        vertico.selected_index < (int)vertico.filtered_count) {
        vertico.on_select(vertico.filtered[vertico.selected_index].data);
    }

    vertico_quit();
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
    KeyChordBinding* binding = (KeyChordBinding*)data;
    if (binding) {
        printf("Selected keybinding: %s - %s\n",
               binding->notation,
               binding->description ? binding->description : "No description");

        // Execute the action
        if (binding->action.c_action) {
            binding->action.c_action();
        }
    }
}

void vertico_show_keybindings() {
    vertico_clear_candidates();
    printf("IT WORKED!\n");

    // Add all keybindings as candidates
    for (size_t i = 0; i < keymap.count; i++) {
        KeyChordBinding* binding = &keymap.bindings[i];

        const char* key_text = binding->notation ? binding->notation : "???";
        const char* description = binding->description ? binding->description : "";

        vertico_add_candidate(key_text, description, binding);
    }

    vertico_activate("keymap", keybinding_selected);
}
