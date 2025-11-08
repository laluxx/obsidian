#include "vertico.h"
#include "keychords.h"
#include "common.h"
#include "context.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Vertico vertico = {0};

void vertico_toggle_top() {
    vertico.top = !vertico.top;
}

void vertico_init() {
    memset(&vertico, 0, sizeof(Vertico));
    vertico.is_active = false;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
    vertico.count = 10;
    vertico.cycle = false;
    vertico.top = true;
    
    vertico.font = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 21);
    if (!vertico.font) {
        fprintf(stderr, "Failed to load vertico font\n");
    }
    
    keymap_init(&vertico.vertico_keymap);
    
    keychord_bind(&vertico.vertico_keymap, "C-n",    vertico_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-p",    vertico_previous,   "Previous candidate", PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "RET",    vertico_select,     "Select candidate",   PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-g",    vertico_quit,       "Vertico quit",       PRESS);
    keychord_bind(&vertico.vertico_keymap, "<down>", vertico_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "<up>",   vertico_previous,   "Previous candidate", PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "M-=",    nextTheme,          "Next theme",         PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "M--",    previousTheme,      "Previous theme",     PRESS | REPEAT);
    keychord_bind(&vertico.vertico_keymap, "C-c p",  vertico_first,      "Vertico first",      PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-c n",  vertico_last,       "Vertico last",       PRESS);
    keychord_bind(&vertico.vertico_keymap, "C-t",    vertico_toggle_top, "Vertico toggle top", PRESS);
}

void vertico_cleanup() {
    if (vertico.font) {
        destroy_font(vertico.font);
        vertico.font = NULL;
    }
    keymap_free(&vertico.vertico_keymap);
}

void vertico_activate(const char* category, VerticoSelectCallback callback) {
    vertico.is_active = true;
    vertico.on_select = callback;
    vertico.selected_index = 0;
    vertico.scroll_offset = 0;
    vertico.input_length = 0;
    vertico.input[0] = '\0';
    
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
    
    // Filter candidates initially
    vertico_filter_candidates();
}

void vertico_quit() {
    if (!vertico.is_active) return;
    
    vertico.is_active = false;
    
    // Restore the global keymap
    extern KeyChordMap keymap;
    KeyChordMap temp = keymap;
    keymap = vertico.vertico_keymap;
    vertico.vertico_keymap = temp;
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
        vertico.input[vertico.input_length++] = c;
        vertico.input[vertico.input_length] = '\0';
        vertico_filter_candidates();
    }
}

void vertico_backspace() {
    if (vertico.input_length > 0) {
        vertico.input[--vertico.input_length] = '\0';
        vertico_filter_candidates();
    }
}

void vertico_clear_input() {
    vertico.input_length = 0;
    vertico.input[0] = '\0';
    vertico_filter_candidates();
}

// Handle GLFW key input and convert to self-insert characters
void vertico_handle_char_input(int key, int mods) {
    char c = 0;
    
    // Letters A-Z
    if (key >= KEY_A && key <= KEY_Z) {
        if (mods & MOD_SHIFT) {
            c = key; // Uppercase (A-Z)
        } else {
            c = key + 32; // Lowercase (a-z)
        }
    }
    // Numbers 0-9
    else if (key >= KEY_0 && key <= KEY_9) {
        if (mods & MOD_SHIFT) {
            // Shifted number keys produce symbols
            const char shifted_nums[] = ")!@#$%^&*(";
            c = shifted_nums[key - KEY_0];
        } else {
            c = key; // Regular digits
        }
    }
    // Space
    else if (key == KEY_SPACE) {
        c = ' ';
    }
    // Punctuation and symbols
    else if (key == KEY_MINUS) {
        c = (mods & MOD_SHIFT) ? '_' : '-';
    }
    else if (key == KEY_EQUAL) {
        c = (mods & MOD_SHIFT) ? '+' : '=';
    }
    else if (key == KEY_LEFT_BRACKET) {
        c = (mods & MOD_SHIFT) ? '{' : '[';
    }
    else if (key == KEY_RIGHT_BRACKET) {
        c = (mods & MOD_SHIFT) ? '}' : ']';
    }
    else if (key == KEY_BACKSLASH) {
        c = (mods & MOD_SHIFT) ? '|' : '\\';
    }
    else if (key == KEY_SEMICOLON) {
        c = (mods & MOD_SHIFT) ? ':' : ';';
    }
    else if (key == KEY_APOSTROPHE) {
        c = (mods & MOD_SHIFT) ? '"' : '\'';
    }
    else if (key == KEY_COMMA) {
        c = (mods & MOD_SHIFT) ? '<' : ',';
    }
    else if (key == KEY_PERIOD) {
        c = (mods & MOD_SHIFT) ? '>' : '.';
    }
    else if (key == KEY_SLASH) {
        c = (mods & MOD_SHIFT) ? '?' : '/';
    }
    else if (key == KEY_GRAVE_ACCENT) {
        c = (mods & MOD_SHIFT) ? '~' : '`';
    }
    
    // Insert the character if we got one
    if (c != 0) {
        vertico_insert_char(c);
    }
}



static void render_text_with_highlights(Font* font, const char* inputText, float x, float y,
                                        const char* pattern, Color default_color) {
    if (!pattern || pattern[0] == '\0') {
        // No pattern, just render normally
        text(font, inputText, x, y, default_color);
        return;
    }
    
    // Convert both to lowercase for case-insensitive matching
    char text_lower[256], pattern_lower[256];
    strncpy(text_lower, inputText, sizeof(text_lower) - 1);
    strncpy(pattern_lower, pattern, sizeof(pattern_lower) - 1);
    text_lower[sizeof(text_lower) - 1] = '\0';
    pattern_lower[sizeof(pattern_lower) - 1] = '\0';
    
    for (char* p = text_lower; *p; p++) *p = tolower(*p);
    for (char* p = pattern_lower; *p; p++) *p = tolower(*p);
    
    // Split pattern into words
    char* pattern_words[4];  // Max 4 words for 4 different colors
    int word_count = 0;
    char* pattern_copy = strdup(pattern_lower);
    char* token = strtok(pattern_copy, " ");
    
    while (token && word_count < 4) {
        pattern_words[word_count++] = token;
        token = strtok(NULL, " ");
    }
    
    // Mark which characters should be highlighted and with which color
    int highlight[256] = {0};  // 0 = no highlight, 1-4 = highlight with color index
    
    for (int word_idx = 0; word_idx < word_count; word_idx++) {
        const char* word = pattern_words[word_idx];
        int word_len = strlen(word);
        
        // Find all occurrences of this word in the text
        const char* pos = text_lower;
        while ((pos = strstr(pos, word)) != NULL) {
            int offset = pos - text_lower;
            for (int i = 0; i < word_len; i++) {
                // Only highlight if not already highlighted
                if (highlight[offset + i] == 0) {
                    highlight[offset + i] = word_idx + 1;
                }
            }
            pos++;
        }
    }
    
    free(pattern_copy);
    
    // Get orderless colors from theme
    Color match_colors_fg[4] = {
        CT.orderless_match_face_0_fg,
        CT.orderless_match_face_1_fg,
        CT.orderless_match_face_2_fg,
        CT.orderless_match_face_3_fg
    };
    
    Color match_colors_bg[4] = {
        CT.orderless_match_face_0_bg,
        CT.orderless_match_face_1_bg,
        CT.orderless_match_face_2_bg,
        CT.orderless_match_face_3_bg
    };
    
    // Render character by character with appropriate colors
    float current_x = x;
    for (size_t i = 0; inputText[i] != '\0'; i++) {
        float char_width = character_width(font, inputText[i]);
        
        if (highlight[i] > 0) {
            int color_idx = highlight[i] - 1;
            
            // STEP 1 FIX: Background highlight height
            // descent is negative, so ascent + descent gives correct height
            float bg_height = font->ascent + font->descent;
            float bg_y = y + font->descent;
            
            quad2D((vec2){current_x, bg_y},
                   (vec2){char_width, bg_height},
                   match_colors_bg[color_idx]);
            
            // Draw character with highlight foreground color
            char single_char[2] = {inputText[i], '\0'};
            text(font, single_char, current_x, y, match_colors_fg[color_idx]);
        } else {
            // Draw character normally
            char single_char[2] = {inputText[i], '\0'};
            text(font, single_char, current_x, y, default_color);
        }
        
        current_x += char_width;
    }
}


void vertico_render() {
    if (!vertico.is_active || !vertico.font) return;
    
    float screen_width = (float)context.swapChainExtent.width;
    float screen_height = (float)context.swapChainExtent.height;
    
    // Line height calculation
    float line_height = vertico.font->ascent + vertico.font->descent;
    float padding = 8.0f;
    
    // Calculate visible count
    int visible_count = vertico.count;
    if (vertico.filtered_count < (size_t)vertico.count) {
        visible_count = vertico.filtered_count;
    }
    
    // Calculate total height (header line + visible candidates)
    float total_height = line_height * (visible_count + 1) + padding * 2;
    
    // Position based on vertico.top setting
    float box_y;
    if (vertico.top) {
        // Top of screen: box starts at screen_height - total_height
        box_y = screen_height - total_height;
    } else {
        // Bottom of screen: box starts at 0
        box_y = 0.0f;
    }
    
    // Background - cover the entire vertico area
    quad2D((vec2){0.0f, box_y}, (vec2){screen_width, total_height}, CT.bg);
    
    // Border
    if (vertico.top) {
        quad2D((vec2){0.0f, box_y}, (vec2){screen_width, 2.0f}, CT.variable);
    } else {
        quad2D((vec2){0.0f, box_y + total_height - 2.0f}, (vec2){screen_width, 2.0f}, CT.variable);
    }
    
    // FIXED: Since (0,0) is bottom-left and Y increases UPWARDS
    // Start from the TOP of the background (highest Y value)
    float current_y = box_y + total_height - padding - vertico.font->ascent;
    
    // HEADER LINE - this should be at the VERY TOP
    char header[256];
    if (vertico.filtered_count > 0) {
        snprintf(header, sizeof(header), "%zu/%zu %s ",
                 vertico.selected_index + 1,
                 vertico.filtered_count,
                 vertico.category);
    } else {
        snprintf(header, sizeof(header), "0/0 %s ", vertico.category);
    }
    
    // Render header text at the TOP position
    text(vertico.font, header, padding, current_y, CT.keyword);
    
    // Calculate input position
    float header_width = 0.0f;
    for (const char* p = header; *p; p++) {
        header_width += character_width(vertico.font, *p);
    }
    
    // Render input text
    float input_x = padding + header_width;
    text(vertico.font, vertico.input, input_x, current_y, CT.text);
    
    // Cursor - should be on the same line as header/input
    float cursor_x = input_x;
    for (size_t i = 0; i < vertico.input_length; i++) {
        cursor_x += character_width(vertico.font, vertico.input[i]);
    }
    
    float cursor_width = character_width(vertico.font, ' ');
    float cursor_height = line_height;
    float cursor_y = current_y + vertico.font->descent;
    
    quad2D((vec2){cursor_x, cursor_y},
           (vec2){cursor_width, cursor_height},
           CT.cursor);
    
    // Move DOWN (decrease Y) to the next line for first candidate
    current_y -= line_height;
    
    // Render candidates
    for (int i = 0; i < visible_count; i++) {
        int idx = vertico.scroll_offset + i;
        if (idx >= (int)vertico.filtered_count) break;
        
        bool is_selected = (idx == vertico.selected_index);
        
        // Selection highlight
        if (is_selected) {
            float highlight_y = current_y + vertico.font->descent;
            quad2D((vec2){0.0f, highlight_y},
                   (vec2){screen_width, line_height},
                   CT.vertico_current);
        }
        
        VerticoCandidate* candidate = &vertico.filtered[idx];
        
        // Render candidate text
        render_text_with_highlights(vertico.font, candidate->text,
                                    padding,
                                    current_y,
                                    vertico.input,
                                    CT.text);
        
        // Render annotation
        if (candidate->annotation[0] != '\0') {
            float annotation_width = 0.0f;
            for (const char* p = candidate->annotation; *p; p++) {
                annotation_width += character_width(vertico.font, *p);
            }
            float annotation_x = screen_width - annotation_width - padding;
            text(vertico.font, candidate->annotation, annotation_x, current_y, CT.comment);
        }
        
        // Move DOWN to next line
        current_y -= line_height;
    }
}

static void keybinding_selected(void* data) {
    KeyChordBinding* binding = (KeyChordBinding*)data;
    if (binding) {
        printf("Selected keybinding: %s - %s\n", 
               binding->notation, 
               binding->description ? binding->description : "No description");
        
        // Execute the action
        if (binding->action) {
            binding->action();
        }
    }
}

void vertico_show_keybindings() {
    vertico_clear_candidates();
    
    // Add all keybindings as candidates
    for (size_t i = 0; i < keymap.count; i++) {
        KeyChordBinding* binding = &keymap.bindings[i];
        
        const char* key_text = binding->notation ? binding->notation : "???";
        const char* description = binding->description ? binding->description : "";
        
        vertico_add_candidate(key_text, description, binding);
    }
    
    vertico_activate("keymap", keybinding_selected);
}
