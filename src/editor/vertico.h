/// vertico — editor subsystem
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "keychords.h"
#include "renderer.h"
#include "font.h"
#include "command_catalog.h"

/// Public interface

#define VERTICO_MAX_CANDIDATES 1024
#define VERTICO_INPUT_BUFFER_SIZE 256

typedef struct {
    char text[256];       // Display text
    char annotation[256]; // Secondary text (e.g., description)
    void* data;           // Associated data (e.g., KeyChordBinding*)
    float score;          // Match score for ordering
} VerticoCandidate;

typedef void (*VerticoSelectCallback)(void* data);
typedef void (*VerticoTextSubmitCallback)(const char *text, void *userdata);

typedef struct {
    VerticoCandidate candidates[VERTICO_MAX_CANDIDATES];
    size_t candidate_count;

    VerticoCandidate filtered[VERTICO_MAX_CANDIDATES];
    size_t filtered_count;

    char input[VERTICO_INPUT_BUFFER_SIZE];
    size_t input_length;

    size_t selected_index;
    int scroll_offset;

    bool is_active;
    bool text_input_mode;
    bool text_completion_mode;
    VerticoSelectCallback on_select;
    VerticoTextSubmitCallback on_text_submit;
    void *text_submit_userdata;

    KeyChordMap vertico_keymap; // Vertico-specific keymap

    char category[128];         // Category name (e.g., "keymap", "meshes")

    Font* font;                 // For rendering

    int count;          // Number of candidates visible at once
    int scroll_margin;  // Rows retained below the selection before scrolling
    float width;        // Panel width in pixels
    bool top;           // True for top placement; bottom is the default
    bool resize_pixelwise;
    bool material_preview_mode;
    float row_start_y;
    float row_bottom;
    float row_x;
    float row_width;
    float row_height;
    float preview_width;
    bool material_dragging;
    size_t material_drag_index;
    double material_drag_x;
    double material_drag_y;
    bool header_resizing;
    double resize_start_y;
    float resize_start_height;
    int resize_start_count;
    float resize_row_height;
} Vertico;

extern Vertico vertico;

void vertico_init();
void vertico_cleanup();
void vertico_activate(const char* category, VerticoSelectCallback callback);
void vertico_read_text(const char *prompt, VerticoTextSubmitCallback callback,
                       void *userdata);
void vertico_read_completion(const char *prompt, VerticoTextSubmitCallback callback,
                             void *userdata, char code);
void vertico_read_interactive_command(const ObsInteractiveCommand *command);
void vertico_quit();

// Candidate management
void vertico_clear_candidates();
void vertico_add_candidate(const char* text, const char* annotation, void* data);
void vertico_filter_candidates();

void vertico_select();
size_t vertico_filtered_count(void);
size_t vertico_selection_index(void);
int32_t vertico_scroll_offset(void);
int32_t vertico_visible_count(void);
bool vertico_set_position(size_t selected_index, int32_t scroll_offset);

// Input handling
void vertico_insert_char(char c);
void vertico_backspace();
void vertico_clear_input();
void vertico_handle_char_input(int key, int mods);

// Rendering
void vertico_render();

// Utility
float vertico_fuzzy_match(const char* pattern, const char* text);
void vertico_show_commands();
void vertico_show_keybindings();
bool vertico_show_scene_buffers(bool create_frame);
bool vertico_show_scene_file(bool create_frame);
bool vertico_show_image_file(void);
bool vertico_show_materials(void);
bool vertico_show_variables(void);
bool vertico_mouse_button(int button, int action, double x, double y);
bool vertico_mouse_move(double x, double y);
