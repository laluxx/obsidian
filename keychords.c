#include "keychords.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <GLFW/glfw3.h>

#define KEYCHORD_INIT_CAP 64
#define DEFAULT_CHORD_TIMEOUT 1.0  // seconds to complete a chord
static bool should_timeout = false;

KeyChordMap keymap;

// NOTE if we bind let’s say <up> on RELEASE and then we bind <up> again later on PRESS
// The RELEASE one will always take precedence.
// But if we bind <up> on PRESS and then rebind it on PRESS again the second one will take precedence


AfterKeychordHook internal_after_keychord_hook = NULL;
void register_after_keychord_hook(AfterKeychordHook hook) {
    internal_after_keychord_hook = hook;
}

BeforeKeychordHook internal_before_keychord_hook = NULL;
void register_before_keychord_hook(BeforeKeychordHook hook) {
    internal_before_keychord_hook = hook;
}




void keymap_init(KeyChordMap *map) {
    map->bindings = malloc(KEYCHORD_INIT_CAP * sizeof(KeyChordBinding));
    map->count = 0;
    map->capacity = KEYCHORD_INIT_CAP;
    map->current_chord.length = 0;
    map->last_key_time = 0.0;
    map->chord_timeout = DEFAULT_CHORD_TIMEOUT;
}

void keymap_free(KeyChordMap *map) {
    for (size_t i = 0; i < map->count; i++) {
        free(map->bindings[i].description);
        free(map->bindings[i].notation);
    }
    free(map->bindings);
    map->bindings = NULL;
    map->count = map->capacity = 0;
}

// Map of shift characters to their base keys
static const struct {
    char shifted;
    char base;
    int glfw_key;
} shift_char_map[] = {
    {'!', '1', GLFW_KEY_1},
    {'@', '2', GLFW_KEY_2},
    {'#', '3', GLFW_KEY_3},
    {'$', '4', GLFW_KEY_4},
    {'%', '5', GLFW_KEY_5},
    {'^', '6', GLFW_KEY_6},
    {'&', '7', GLFW_KEY_7},
    {'*', '8', GLFW_KEY_8},
    {'(', '9', GLFW_KEY_9},
    {')', '0', GLFW_KEY_0},
    {'_', '-', GLFW_KEY_MINUS},
    {'+', '=', GLFW_KEY_EQUAL},
    {'{', '[', GLFW_KEY_LEFT_BRACKET},
    {'}', ']', GLFW_KEY_RIGHT_BRACKET},
    {'|', '\\', GLFW_KEY_BACKSLASH},
    {':', ';', GLFW_KEY_SEMICOLON},
    {'"', '\'', GLFW_KEY_APOSTROPHE},
    {'<', ',', GLFW_KEY_COMMA},
    {'>', '.', GLFW_KEY_PERIOD},
    {'?', '/', GLFW_KEY_SLASH},
    {'~', '`', GLFW_KEY_GRAVE_ACCENT},
    {0, 0, 0}  // sentinel
};

// Parse a single key with modifiers (e.g., "C-w" or "M-x")
static bool parse_single_key(const char *notation, int *key_out, int *mods_out) {
    int mods = 0;
    const char *key_part = notation;
    size_t len = strlen(notation);
    
    // Parse modifiers
    while (len >= 2 && key_part[1] == '-') {
        if (key_part[0] == 'C') {
            mods |= GLFW_MOD_CONTROL;
            key_part += 2;
            len -= 2;
        } else if (key_part[0] == 'M') {
            mods |= GLFW_MOD_ALT;
            key_part += 2;
            len -= 2;
        } else if (key_part[0] == 'S') {
            mods |= GLFW_MOD_SHIFT;
            key_part += 2;
            len -= 2;
        } else {
            break;
        }
    }
    
    if (len == 0) return false;
    
    // Parse the base key
    if (len == 1) {
        char c = key_part[0];
        
        // Check if this is a shift character (like >, <, +, etc.)
        bool found_shift_char = false;
        for (int i = 0; shift_char_map[i].shifted != 0; i++) {
            if (c == shift_char_map[i].shifted) {
                *key_out = shift_char_map[i].glfw_key;
                mods |= GLFW_MOD_SHIFT;
                found_shift_char = true;
                break;
            }
            // Also handle the base character explicitly
            if (c == shift_char_map[i].base) {
                *key_out = shift_char_map[i].glfw_key;
                found_shift_char = true;
                break;
            }
        }
        
        if (found_shift_char) {
            *mods_out = mods;
            return true;
        }
        
        // Handle letter keys
        if (isalpha(c)) {
            *key_out = toupper(c);  // GLFW always uses uppercase for letter keys
            
            // If the character in notation is uppercase, it means Shift is required
            if (isupper(c)) {
                mods |= GLFW_MOD_SHIFT;
            }
        } else {
            *key_out = c;
        }
    } else {
        // Handle special key names
        if (strcmp(key_part, "SPC") == 0) {
            *key_out = GLFW_KEY_SPACE;
        } else if (strcmp(key_part, "RET") == 0 || strcmp(key_part, "<return>") == 0) {
            *key_out = GLFW_KEY_ENTER;
        } else if (strcmp(key_part, "TAB") == 0 || strcmp(key_part, "<tab>") == 0) {
            *key_out = GLFW_KEY_TAB;
        } else if (strcmp(key_part, "ESC") == 0) {
            *key_out = GLFW_KEY_ESCAPE;
        } else if (strcmp(key_part, "DEL") == 0 || strcmp(key_part, "<backspace>") == 0) {
            *key_out = GLFW_KEY_BACKSPACE;
        } else if (strcmp(key_part, "<up>") == 0) {
            *key_out = GLFW_KEY_UP;
        } else if (strcmp(key_part, "<down>") == 0) {
            *key_out = GLFW_KEY_DOWN;
        } else if (strcmp(key_part, "<left>") == 0) {
            *key_out = GLFW_KEY_LEFT;
        } else if (strcmp(key_part, "<right>") == 0) {
            *key_out = GLFW_KEY_RIGHT;
        } else if (strcmp(key_part, "<delete>") == 0) {
            *key_out = GLFW_KEY_DELETE;
        } else {
            return false;
        }
    }
    
    *mods_out = mods;
    return true;
}

bool parse_keychord_notation(const char *notation, KeyChord *chord) {
    if (!notation || !chord) return false;
    
    memset(chord, 0, sizeof(KeyChord));
    
    // Split by spaces for multi-key chords
    char *notation_copy = strdup(notation);
    char *token = strtok(notation_copy, " ");
    
    while (token && chord->length < 4) {
        int key, mods;
        if (!parse_single_key(token, &key, &mods)) {
            free(notation_copy);
            return false;
        }
        
        chord->keys[chord->length] = key;
        chord->mods[chord->length] = mods;
        chord->length++;
        
        token = strtok(NULL, " ");
    }
    
    free(notation_copy);
    return chord->length > 0;
}

bool keychord_bind(KeyChordMap *map, const char *notation,
                   KeyChordAction action,
                   const char *description,
                   int action_type) {
    if (!map || !notation || !action) return false;
    
    KeyChord chord;
    if (!parse_keychord_notation(notation, &chord)) return false;
    
    // Check if binding already exists with the exact same chord
    for (size_t i = 0; i < map->count; i++) {
        if (keychord_equal(&map->bindings[i].chord, &chord)) {
            // Found existing binding for this chord, merge action types
            map->bindings[i].action_type |= action_type;
            map->bindings[i].action = action;
            free(map->bindings[i].description);
            map->bindings[i].description = description ? strdup(description) : NULL;
            return true;
        }
    }
    
    // Add new binding
    if (map->count >= map->capacity) {
        map->capacity *= 2;
        map->bindings = realloc(map->bindings, map->capacity * sizeof(KeyChordBinding));
    }
    
    KeyChordBinding *binding = &map->bindings[map->count];
    binding->chord = chord;
    binding->action = action;
    binding->description = description ? strdup(description) : NULL;
    binding->notation = strdup(notation);
    binding->action_type = action_type;
    
    map->count++;
    return true;
}

bool keychord_unbind(KeyChordMap *map, const char *notation) {
    if (!map || !notation) return false;
    
    KeyChord chord;
    if (!parse_keychord_notation(notation, &chord)) return false;
    
    for (size_t i = 0; i < map->count; i++) {
        if (keychord_equal(&map->bindings[i].chord, &chord)) {
            free(map->bindings[i].description);
            free(map->bindings[i].notation);
            
            // Move last binding to this position
            if (i < map->count - 1) {
                map->bindings[i] = map->bindings[map->count - 1];
            }
            map->count--;
            return true;
        }
    }
    
    return false;
}

KeyChordAction keychord_lookup(KeyChordMap *map, const KeyChord *chord, int action_type) {
    if (!map || !chord) return NULL;
    
    for (size_t i = 0; i < map->count; i++) {
        if (keychord_equal(&map->bindings[i].chord, chord) &&
            (map->bindings[i].action_type & action_type)) {  // Bitwise check
            return map->bindings[i].action;
        }
    }
    
    return NULL;
}


bool keychord_process_key(KeyChordMap *map, int key, int action, int mods) {
    if (!map) return false;
    
    // Ignore standalone modifier key presses in chord building
    if (key == GLFW_KEY_LEFT_SHIFT   || key == GLFW_KEY_RIGHT_SHIFT   ||
        key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
        key == GLFW_KEY_LEFT_ALT     || key == GLFW_KEY_RIGHT_ALT     ||
        key == GLFW_KEY_LEFT_SUPER   || key == GLFW_KEY_RIGHT_SUPER) {
        return false;
    }
    
    double current_time = glfwGetTime();
    
    // Create a chord for the current key press
    KeyChord single_key_chord;
    single_key_chord.keys[0] = key;
    single_key_chord.mods[0] = mods;
    single_key_chord.length = 1;
    
    // Check what action types are bound to this key
    KeyChordAction press_action = keychord_lookup(map, &single_key_chord, GLFW_PRESS);
    KeyChordAction release_action = keychord_lookup(map, &single_key_chord, GLFW_RELEASE);
    KeyChordAction repeat_action = keychord_lookup(map, &single_key_chord, GLFW_REPEAT);
    
    // If we're on PRESS but this key is ONLY bound to RELEASE or REPEAT, consume it
    if (action == GLFW_PRESS && !press_action && (release_action || repeat_action)) {
        if (map->current_chord.length > 0) {
            keychord_reset_state(map);
        }
        return true;
    }
    
    // Handle single-key bindings (when not building a multi-key chord)
    if (map->current_chord.length == 0) {
        KeyChordAction single_action = keychord_lookup(map, &single_key_chord, action);
        if (single_action) {
            // Find the binding for the hooks
            KeyChordBinding *binding = NULL;
            for (size_t i = 0; i < map->count; i++) {
                if (keychord_equal(&map->bindings[i].chord, &single_key_chord) &&
                    (map->bindings[i].action_type & action)) {
                    binding = &map->bindings[i];
                    break;
                }
            }
            
            // Call BEFORE hook
            if (internal_before_keychord_hook && binding) {
                internal_before_keychord_hook(binding->notation, binding);
            }
            
            // Execute the action
            single_action();
            
            // Call AFTER hook
            if (internal_after_keychord_hook && binding) {
                internal_after_keychord_hook(binding->notation, binding);
            }
            
            return true;
        }
    }
    
    // Only build multi-key chords on PRESS
    if (action != GLFW_PRESS) {
        return false;
    }
    
    // Check for chord timeout
    if (map->current_chord.length > 0) {
        if (current_time - map->last_key_time > map->chord_timeout) {
            printf("Chord timeout, resetting\n");
            keychord_reset_state(map);
        }
    }
    
    // Add key to current chord
    if (map->current_chord.length < 4) {
        map->current_chord.keys[map->current_chord.length] = key;
        map->current_chord.mods[map->current_chord.length] = mods;
        map->current_chord.length++;
        map->last_key_time = current_time;
    }
    
    // Try to find a matching multi-key binding
    KeyChordAction multi_action = keychord_lookup(map, &map->current_chord, GLFW_PRESS);
    
    if (multi_action) {
        // Find the binding for the hooks
        KeyChordBinding *binding = NULL;
        for (size_t i = 0; i < map->count; i++) {
            if (keychord_equal(&map->bindings[i].chord, &map->current_chord) &&
                (map->bindings[i].action_type & GLFW_PRESS)) {
                binding = &map->bindings[i];
                break;
            }
        }
        
        // Call BEFORE hook
        if (internal_before_keychord_hook && binding) {
            internal_before_keychord_hook(binding->notation, binding);
        }
        
        // Execute the action
        multi_action();
        
        // Call AFTER hook
        if (internal_after_keychord_hook && binding) {
            internal_after_keychord_hook(binding->notation, binding);
        }
        
        // Reset chord state
        keychord_reset_state(map);
        return true;
    }
    
    // Check if this could be a prefix for a longer chord
    bool is_prefix = false;
    for (size_t i = 0; i < map->count; i++) {
        KeyChord *bound = &map->bindings[i].chord;
        
        // Check if bound chord is longer than current
        if (bound->length <= map->current_chord.length) {
            continue;
        }
        
        // Check if current chord matches the beginning of this binding
        bool matches = true;
        for (size_t j = 0; j < map->current_chord.length; j++) {
            if (bound->keys[j] != map->current_chord.keys[j] ||
                bound->mods[j] != map->current_chord.mods[j]) {
                matches = false;
                break;
            }
        }
        
        if (matches) {
            is_prefix = true;
            break;
        }
    }
    
    if (is_prefix) {
        // We're in the middle of a chord - consume the key
        return true;
    }
    
    // Not a match and not a prefix - reset and allow normal handling
    keychord_reset_state(map);
    return false;
}

KeyChordBinding *keychord_find_binding(KeyChordMap *map, const char *notation) {
    if (!map || !notation) return NULL;
    
    for (size_t i = 0; i < map->count; i++) {
        if (map->bindings[i].notation && 
            strcmp(map->bindings[i].notation, notation) == 0) {
            return &map->bindings[i];
        }
    }
    
    return NULL;
}

void keymap_print_bindings(KeyChordMap *map) {
    if (!map) return;
    
    printf("Key Chord Bindings:\n");
    printf("===================\n");
    
    for (size_t i = 0; i < map->count; i++) {
        KeyChordBinding *binding = &map->bindings[i];
        printf("%-15s", binding->notation ? binding->notation : "Unknown");
        
        if (binding->description) {
            printf(" - %s", binding->description);
        }
        putchar('\n');
    }
}

bool keychord_equal(const KeyChord *a, const KeyChord *b) {
    if (!a || !b) return false;
    if (a->length != b->length) return false;
    
    for (size_t i = 0; i < a->length; i++) {
        int a_key = a->keys[i];
        int b_key = b->keys[i];
        int a_mods = a->mods[i];
        int b_mods = b->mods[i];
        
        // Keys must match
        if (a_key != b_key) return false;
        
        // For letter keys (A-Z), normalize shift handling
        if (a_key >= GLFW_KEY_A && a_key <= GLFW_KEY_Z) {
            // Extract non-shift modifiers
            int a_other_mods = a_mods & ~GLFW_MOD_SHIFT;
            int b_other_mods = b_mods & ~GLFW_MOD_SHIFT;
            
            // Non-shift modifiers must match
            if (a_other_mods != b_other_mods) return false;
            
            // Shift must match exactly
            bool a_has_shift = (a_mods & GLFW_MOD_SHIFT) != 0;
            bool b_has_shift = (b_mods & GLFW_MOD_SHIFT) != 0;
            
            if (a_has_shift != b_has_shift) return false;
        } else {
            // For non-letter keys, all modifiers must match exactly
            if (a_mods != b_mods) return false;
        }
    }
    
    return true;
}

void keychord_reset_state(KeyChordMap *map) {
    if (map) {
        map->current_chord.length = 0;
        map->last_key_time = 0.0;
    }
}

// Reset the state of the global keymap
void keymap_reset_state() {
    keymap.current_chord.length = 0;
    keymap.last_key_time = 0.0;
}
