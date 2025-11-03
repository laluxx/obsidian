#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "input.h"

// Key sequence for chord support (e.g., "C-x C-s")
typedef struct {
    int keys[4];   // Support up to 4 keys in a chord
    int mods[4];   // Modifiers for each key
    size_t length; // Number of keys in the chord
} KeyChord;

// Action callback
typedef void (*KeyChordAction)(void);

typedef struct {
    KeyChord chord;
    KeyChordAction action;
    char *description;
    char *notation; // Original notation
} KeyChordBinding;

typedef struct {
    KeyChordBinding *bindings;
    size_t count;
    size_t capacity;
    
    // Chord state tracking
    KeyChord current_chord;
    double last_key_time;
    double chord_timeout;   // Time in seconds to wait for next key in chord
} KeyChordMap;


extern KeyChordMap keymap;

void keymap_init(KeyChordMap *map);
void keymap_free(KeyChordMap *map);

bool parse_keychord_notation(const char *notation, KeyChord *chord);
bool keychord_bind(KeyChordMap *map, const char *notation, KeyChordAction action, const char *description);
bool keychord_unbind(KeyChordMap *map, const char *notation);
KeyChordAction keychord_lookup(KeyChordMap *map, const KeyChord *chord);
bool keychord_process_key(KeyChordMap *map, int key, int action, int mods);

// Helper to find binding by notation
KeyChordBinding *keychord_find_binding(KeyChordMap *map, const char *notation);
void keymap_print_bindings(KeyChordMap *map);
bool keychord_equal(const KeyChord *a, const KeyChord *b);

// Reset chord state (e.g., on timeout or C-g)
void keychord_reset_state(KeyChordMap *map);
void keymap_reset_state();
