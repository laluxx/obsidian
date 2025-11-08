#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "keychords.h"
#include "renderer.h"
#include "font.h"

#define VERTICO_MAX_CANDIDATES 1024
#define VERTICO_INPUT_BUFFER_SIZE 256

typedef struct {
    char text[256];       // Display text
    char annotation[256]; // Secondary text (e.g., description)
    void* data;           // Associated data (e.g., KeyChordBinding*)
    float score;          // Match score for ordering
} VerticoCandidate;

typedef void (*VerticoSelectCallback)(void* data);

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
    VerticoSelectCallback on_select;
    
    KeyChordMap* saved_keymap;  // Store the previous keymap
    KeyChordMap vertico_keymap; // Vertico-specific keymap
    
    char category[128];         // Category name (e.g., "keymap", "meshes")
    
    Font* font;                 // Font for rendering
    
    int count;          // Maximal number of candidates to show
    bool cycle;         // Enable cycling for ‘vertico-next’ and ‘vertico-previous’
    bool top;           // If vertico should be rendered at the top of the screen
} Vertico;

extern Vertico vertico;

void vertico_init();
void vertico_cleanup();
void vertico_activate(const char* category, VerticoSelectCallback callback);
void vertico_quit();

// Candidate management
void vertico_clear_candidates();
void vertico_add_candidate(const char* text, const char* annotation, void* data);
void vertico_filter_candidates();

void vertico_next();
void vertico_previous();
void vertico_select();
void vertico_first();
void vertico_last();

// Input handling
void vertico_insert_char(char c);
void vertico_backspace();
void vertico_clear_input();
void vertico_handle_char_input(int key, int mods);

// Rendering
void vertico_render();

// Utility
float vertico_fuzzy_match(const char* pattern, const char* text);
void vertico_show_keybindings();
