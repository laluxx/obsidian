#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "input.h"
#include <libguile.h>

// Key sequence for chord support (e.g., "C-x C-s")
typedef struct {
    int keys[4];   // Support up to 4 keys in a chord
    int mods[4];   // Modifiers for each key
    size_t length; // Number of keys in the chord
} KeyChord;

typedef void (*KeyChordAction)(void);

typedef enum {
    ACTION_C_FUNCTION,
    ACTION_SCHEME_PROC
} ActionType;

typedef struct {
    KeyChord chord;
    ActionType action_type;
    union {
        KeyChordAction c_action;
        SCM scheme_proc;
    } action;
    char *description;
    char *notation;       // Original notation
    int action_type_flag; // PRESS, RELEASE, or REPEAT (or bitwise combination)
} KeyChordBinding;

typedef struct {
    KeyChordBinding *bindings;
    size_t count;
    size_t capacity;
    KeyChord current_chord;
    double last_key_time;
    double chord_timeout;
} KeyChordMap;

// Global keymap
extern KeyChordMap keymap;

// Keymap stack for layered keymaps
#define MAX_KEYMAP_STACK 8
extern KeyChordMap *keymap_stack[MAX_KEYMAP_STACK];
extern size_t keymap_stack_count;

// Keychord Hook
typedef void (*AfterKeychordHook)(const char *notation, KeyChordBinding *binding);
extern AfterKeychordHook internal_after_keychord_hook;
void register_after_keychord_hook(AfterKeychordHook hook);

typedef void (*BeforeKeychordHook)(const char *notation, KeyChordBinding *binding);
extern BeforeKeychordHook internal_before_keychord_hook;
void register_before_keychord_hook(BeforeKeychordHook hook);

void keymap_init(KeyChordMap *map);
void keymap_free(KeyChordMap *map);
bool parse_keychord_notation(const char *notation, KeyChord *chord);
bool keychord_bind(KeyChordMap *map, const char *notation, KeyChordAction action, 
                   const char *description, int action_type);
bool keychord_bind_scheme(KeyChordMap *map, const char *notation, SCM scheme_proc,
                          const char *description, int action_type);
bool keychord_unbind(KeyChordMap *map, const char *notation);
bool keychord_process_key(KeyChordMap *map, int key, int action, int mods);
KeyChordBinding *keychord_find_binding(KeyChordMap *map, const char *notation);
void keymap_print_bindings(KeyChordMap *map);
bool keychord_equal(const KeyChord *a, const KeyChord *b);
void keychord_reset_state(KeyChordMap *map);
void keymap_reset_state();

// Look up a binding without processing (for fallthrough checking)
KeyChordBinding* keychord_lookup_binding(KeyChordMap *map, const KeyChord *chord, int action_type);

// Keymap stack operations
void keymap_stack_push(KeyChordMap *map);
void keymap_stack_pop(void);
void keymap_stack_clear(void);
KeyChordMap* keymap_stack_get_local(void); // Get the topmost local keymap (or NULL)

// Process key with keymap stack (checks local maps first, then global)
bool keychord_process_key_with_stack(int key, int action, int mods);
