/// keychords — platform subsystem
#include "keychords.h"
#include <GLFW/glfw3.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Implementation

#define KEYCHORD_INIT_CAP 64
#define DEFAULT_CHORD_TIMEOUT 1.0         // seconds to complete a chord
bool skip_next_binding_execution = false; // NOTE EXTERN
bool pending_input = false;               // NOTE EXTERN

KeyChordMap keymap;
static KeyChordMap *frame_local_map;
static KeyChordMap *overriding_terminal_local_map = NULL;
static bool overriding_binding_seen = false;
static KeyChordBinding *overriding_current_binding = NULL;
static uint64_t next_keymap_generation = 1;
static uint64_t keymap_stack_generation = 1;

#define KEYCHORD_LOOKUP_CACHE_CAPACITY 32
typedef struct {
  KeyChordMap *map;
  KeyChord chord;
  int action_type;
  uint64_t generation;
  size_t binding_index;
} KeyChordLookupCacheEntry;

static KeyChordLookupCacheEntry
    keychord_lookup_cache[KEYCHORD_LOOKUP_CACHE_CAPACITY];
#ifdef OBS_KEYCHORD_PROFILE
static uint64_t keychord_lookup_cache_hit_count;
#endif
static bool s_prefix_arg_active;
static bool s_prefix_digits_started;
static bool s_prefix_negative;
static int64_t s_prefix_arg_value;
static bool s_prefix_capture_enabled = true;
static bool s_prefix_capture_priority;

bool keychord_prefix_arg_is_set(void) { return s_prefix_arg_active; }
bool keychord_prefix_arg_has_digits(void) { return s_prefix_digits_started; }
int64_t keychord_prefix_arg_value(void) { return s_prefix_arg_value; }
void keychord_prefix_begin_negative(void) {
  s_prefix_arg_active = true;
  s_prefix_digits_started = false;
  s_prefix_negative = true;
  s_prefix_arg_value = -1;
}
void keychord_clear_prefix_arg(void) {
  s_prefix_arg_active = false;
  s_prefix_digits_started = false;
  s_prefix_negative = false;
  s_prefix_arg_value = 0;
}
void keychord_set_prefix_capture_enabled(bool enabled) {
  s_prefix_capture_enabled = enabled;
}
void keychord_set_prefix_capture_priority(bool enabled) {
  s_prefix_capture_priority = enabled;
}

static bool keychord_consume_prefix_key(int key, int action, int mods) {
  if (action != GLFW_PRESS)
    return false;
  if (key == GLFW_KEY_U && (mods & GLFW_MOD_CONTROL)) {
    if (!s_prefix_arg_active)
      s_prefix_arg_value = 4;
    else if (s_prefix_arg_value <= INT64_MAX / 4 &&
             s_prefix_arg_value >= INT64_MIN / 4)
      s_prefix_arg_value *= 4;
    s_prefix_arg_active = true;
    s_prefix_digits_started = false;
    s_prefix_negative = false;
    return true;
  }
  if (key == GLFW_KEY_MINUS &&
      ((mods & GLFW_MOD_ALT) || s_prefix_capture_priority)) {
    s_prefix_arg_active = true;
    s_prefix_arg_value = -1;
    s_prefix_digits_started = false;
    s_prefix_negative = true;
    return true;
  }
  int digit = key >= GLFW_KEY_0 && key <= GLFW_KEY_9 ? key - GLFW_KEY_0 : -1;
  if (digit < 0 || (!s_prefix_arg_active && !(mods & GLFW_MOD_ALT) &&
                    !s_prefix_capture_priority))
    return false;
  if (!s_prefix_arg_active) {
    s_prefix_arg_active = true;
    s_prefix_arg_value = 0;
    s_prefix_negative = false;
  }
  if (!s_prefix_digits_started) {
    s_prefix_arg_value = s_prefix_negative ? -digit : digit;
    s_prefix_digits_started = true;
  } else if (s_prefix_negative) {
    if (s_prefix_arg_value > INT64_MIN / 10 ||
        (s_prefix_arg_value == INT64_MIN / 10 && digit <= -(INT64_MIN % 10)))
      s_prefix_arg_value = s_prefix_arg_value * 10 - digit;
    else
      s_prefix_arg_value = INT64_MIN;
  } else if (s_prefix_arg_value < INT64_MAX / 10 ||
             (s_prefix_arg_value == INT64_MAX / 10 &&
              digit <= INT64_MAX % 10)) {
    s_prefix_arg_value = s_prefix_arg_value * 10 + digit;
  } else
    s_prefix_arg_value = INT64_MAX;
  return true;
}

typedef struct {
  bool valid;
  int key;
  int mods;
  uint64_t stack_generation;
  KeyChordMap *map;
  uint64_t map_generation;
  size_t binding_index;
} KeyChordRepeatCache;

static KeyChordRepeatCache keychord_repeat_cache;
#ifdef OBS_KEYCHORD_PROFILE
static uint64_t keychord_repeat_fast_hit_count;
#endif

void keychord_reset_lookup_profile(void) {
#ifdef OBS_KEYCHORD_PROFILE
  keychord_lookup_cache_hit_count = 0;
  keychord_repeat_fast_hit_count = 0;
#endif
  keychord_repeat_cache.valid = false;
}

uint64_t keychord_lookup_cache_hits(void) {
#ifdef OBS_KEYCHORD_PROFILE
  return keychord_lookup_cache_hit_count;
#else
  return 0;
#endif
}

uint64_t keychord_repeat_fast_hits(void) {
#ifdef OBS_KEYCHORD_PROFILE
  return keychord_repeat_fast_hit_count;
#else
  return 0;
#endif
}

// NOTE if we bind let’s say <up> on RELEASE and then we bind <up> again later
// on PRESS The RELEASE one will always take precedence. But if we bind <up> on
// PRESS and then rebind it on PRESS again the second one will take precedence

AfterKeychordHook internal_after_keychord_hook = NULL;
void register_after_keychord_hook(AfterKeychordHook hook) {
  internal_after_keychord_hook = hook;
}

BeforeKeychordHook internal_before_keychord_hook = NULL;
void register_before_keychord_hook(BeforeKeychordHook hook) {
  internal_before_keychord_hook = hook;
}

/* RawKeyInterceptor internal_raw_key_interceptor = NULL; */
/* void register_raw_key_interceptor(RawKeyInterceptor interceptor) */
/*  {internal_raw_key_interceptor = interceptor;} */

static RawKeyInterceptor raw_interceptors[MAX_RAW_INTERCEPTORS];
static int raw_interceptor_count = 0;

void register_raw_key_interceptor(RawKeyInterceptor interceptor) {
  if (raw_interceptor_count < MAX_RAW_INTERCEPTORS)
    raw_interceptors[raw_interceptor_count++] = interceptor;
}

void unregister_raw_key_interceptor(RawKeyInterceptor interceptor) {
  for (int i = 0; i < raw_interceptor_count; i++) {
    if (raw_interceptors[i] == interceptor) {
      // Shift remaining down
      for (int j = i; j < raw_interceptor_count - 1; j++)
        raw_interceptors[j] = raw_interceptors[j + 1];
      raw_interceptor_count--;
      return;
    }
  }
}

CallInteractivelyFn keychord_call_interactively = NULL;
void register_call_interactively(CallInteractivelyFn fn) {
  keychord_call_interactively = fn;
}
CallInteractivelyNamedFn keychord_call_interactively_named = NULL;
void register_call_interactively_named(CallInteractivelyNamedFn fn) {
  keychord_call_interactively_named = fn;
}

KeyChordNativeActionGuard keychord_native_action_guard = NULL;
void register_keychord_native_action_guard(KeyChordNativeActionGuard guard) {
  keychord_native_action_guard = guard;
}

void keymap_init(KeyChordMap *map) {
  map->bindings = malloc(KEYCHORD_INIT_CAP * sizeof(KeyChordBinding));
  map->count = 0;
  map->capacity = map->bindings ? KEYCHORD_INIT_CAP : 0;
  map->current_chord.length = 0;
  map->last_key_time = 0.0;
  map->chord_timeout = DEFAULT_CHORD_TIMEOUT;
  map->lookup_generation = next_keymap_generation++;
  if (!map->lookup_generation)
    map->lookup_generation = next_keymap_generation++;
}

static void keymap_touch(KeyChordMap *map) {
  if (!map)
    return;
  map->lookup_generation++;
  if (!map->lookup_generation)
    map->lookup_generation = next_keymap_generation++;
}

static void keymap_stack_touch(void) {
  keymap_stack_generation++;
  if (!keymap_stack_generation)
    keymap_stack_generation = 1;
}

static void repeat_cache_store(int key, int mods, KeyChordMap *map,
                               const KeyChordBinding *binding) {
  if (!map || !binding || !(binding->action_type_flag & GLFW_REPEAT)) {
    keychord_repeat_cache.valid = false;
    return;
  }
  size_t index = (size_t)(binding - map->bindings);
  if (index >= map->count) {
    keychord_repeat_cache.valid = false;
    return;
  }
  keychord_repeat_cache.valid = true;
  keychord_repeat_cache.key = key;
  keychord_repeat_cache.mods = mods;
  keychord_repeat_cache.stack_generation = keymap_stack_generation;
  keychord_repeat_cache.map = map;
  keychord_repeat_cache.map_generation = map->lookup_generation;
  keychord_repeat_cache.binding_index = index;
}

static KeyChordBinding *repeat_cache_lookup(int key, int mods) {
  KeyChordRepeatCache *cache = &keychord_repeat_cache;
  if (!cache->valid || cache->key != key || cache->mods != mods ||
      cache->stack_generation != keymap_stack_generation || !cache->map ||
      cache->map_generation != cache->map->lookup_generation ||
      cache->binding_index >= cache->map->count)
    return NULL;
  KeyChordBinding *binding = &cache->map->bindings[cache->binding_index];
  if (!(binding->action_type_flag & GLFW_REPEAT)) {
    cache->valid = false;
    return NULL;
  }
#ifdef OBS_KEYCHORD_PROFILE
  keychord_repeat_fast_hit_count++;
#endif
  return binding;
}

static void release_binding_action(KeyChordBinding *binding) {
  if (!binding)
    return;

  if (binding->action_type == ACTION_CALLBACK) {
    if (binding->action.callback.destroy)
      binding->action.callback.destroy(binding->action.callback.userdata);
  } else if (binding->action_type == ACTION_MONAD_PROC) {
    monad_value_release(binding->action.monad_proc);
  }
  if (binding->command_proc) {
    monad_value_release(binding->command_proc);
    binding->command_proc = NULL;
  }
  free(binding->command_name);
  binding->command_name = NULL;
}

void keymap_free(KeyChordMap *map) {
  if (map == frame_local_map)
    keychord_set_frame_local_map(NULL);
  if (map == overriding_terminal_local_map)
    keymap_clear_overriding();
  for (size_t i = 0; i < map->count; i++) {
    release_binding_action(&map->bindings[i]);
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
    {0, 0, 0} // sentinel
};

static char _notation_buf[16];

const char *key_to_notation(int key, int mods) {
  bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
  bool shift = (mods & GLFW_MOD_SHIFT) != 0;
  bool alt = (mods & GLFW_MOD_ALT) != 0;

  // Special keys
  if (key == GLFW_KEY_SPACE) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-SPC");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-SPC");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-SPC");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "SPC");
    return _notation_buf;
  }
  if (key == GLFW_KEY_ENTER) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-RET");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-RET");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-RET");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "RET");
    return _notation_buf;
  }
  if (key == GLFW_KEY_TAB) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-TAB");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-TAB");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-TAB");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "TAB");
    return _notation_buf;
  }
  if (key == GLFW_KEY_ESCAPE) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-ESC");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-ESC");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-ESC");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "ESC");
    return _notation_buf;
  }
  if (key == GLFW_KEY_BACKSPACE) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-DEL");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-DEL");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-DEL");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "DEL");
    return _notation_buf;
  }
  if (key == GLFW_KEY_DELETE) {
    if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-<delete>");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "<delete>");
    return _notation_buf;
  }
  if (key == GLFW_KEY_UP) {
    if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-<up>");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "<up>");
    return _notation_buf;
  }
  if (key == GLFW_KEY_DOWN) {
    if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-<down>");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "<down>");
    return _notation_buf;
  }
  if (key == GLFW_KEY_LEFT) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-<left>");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-<left>");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-<left>");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "<left>");
    return _notation_buf;
  }
  if (key == GLFW_KEY_RIGHT) {
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-<right>");
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-<right>");
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-<right>");
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "<right>");
    return _notation_buf;
  }

  // Letter keys
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
    char lower = 'a' + (key - GLFW_KEY_A);
    char upper = 'A' + (key - GLFW_KEY_A);
    if (ctrl && alt && shift)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-%c", upper);
    else if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-%c", lower);
    else if (ctrl && shift)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-%c", upper);
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-%c", lower);
    else if (alt && shift)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-%c", upper);
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-%c", lower);
    else if (shift)
      snprintf(_notation_buf, sizeof(_notation_buf), "%c", upper);
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "%c", lower);
    return _notation_buf;
  }

  // Digit keys
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
    char c = '0' + (key - GLFW_KEY_0);
    if (shift) {
      for (int i = 0; shift_char_map[i].shifted != 0; i++) {
        if (shift_char_map[i].glfw_key == key) {
          if (alt)
            snprintf(_notation_buf, sizeof(_notation_buf), "M-%c",
                     shift_char_map[i].shifted);
          else
            snprintf(_notation_buf, sizeof(_notation_buf), "%c",
                     shift_char_map[i].shifted);
          return _notation_buf;
        }
      }
    }
    if (ctrl && alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-M-%c", c);
    else if (ctrl)
      snprintf(_notation_buf, sizeof(_notation_buf), "C-%c", c);
    else if (alt)
      snprintf(_notation_buf, sizeof(_notation_buf), "M-%c", c);
    else
      snprintf(_notation_buf, sizeof(_notation_buf), "%c", c);
    return _notation_buf;
  }

  // Punctuation/symbol keys
  for (int i = 0; shift_char_map[i].shifted != 0; i++) {
    if (shift_char_map[i].glfw_key == key) {
      char c = shift ? shift_char_map[i].shifted : shift_char_map[i].base;
      if (ctrl && alt)
        snprintf(_notation_buf, sizeof(_notation_buf), "C-M-%c", c);
      else if (ctrl)
        snprintf(_notation_buf, sizeof(_notation_buf), "C-%c", c);
      else if (alt)
        snprintf(_notation_buf, sizeof(_notation_buf), "M-%c", c);
      else
        snprintf(_notation_buf, sizeof(_notation_buf), "%c", c);
      return _notation_buf;
    }
  }

  snprintf(_notation_buf, sizeof(_notation_buf), "%c", (char)key);
  return _notation_buf;
}

static char _literal_buf[8];

/// notation_to_literal — implementation group
const char *notation_to_literal(const char *notation) {
  if (strcmp(notation, "TAB") == 0) {
    _literal_buf[0] = '\t';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "RET") == 0) {
    _literal_buf[0] = '\n';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "SPC") == 0) {
    _literal_buf[0] = ' ';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "ESC") == 0) {
    _literal_buf[0] = '\x1b';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "DEL") == 0) {
    _literal_buf[0] = '\x7f';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "NUL") == 0) {
    _literal_buf[0] = '\x00';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "BEL") == 0) {
    _literal_buf[0] = '\x07';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "BS") == 0) {
    _literal_buf[0] = '\x08';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "LF") == 0) {
    _literal_buf[0] = '\x0a';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "VT") == 0) {
    _literal_buf[0] = '\x0b';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "FF") == 0) {
    _literal_buf[0] = '\x0c';
    _literal_buf[1] = 0;
    return _literal_buf;
  }
  if (strcmp(notation, "CR") == 0) {
    _literal_buf[0] = '\x0d';
    _literal_buf[1] = 0;
    return _literal_buf;
  }

  if (notation[0] == 'C' && notation[1] == '-' && notation[2] != '\0' &&
      notation[3] == '\0') {
    char k = notation[2];
    if (k >= '@' && k <= '_') {
      _literal_buf[0] = k - '@';
      _literal_buf[1] = 0;
      return _literal_buf;
    }
    if (k >= 'a' && k <= 'z') {
      _literal_buf[0] = k - 'a' + 1;
      _literal_buf[1] = 0;
      return _literal_buf;
    }
    if (k >= '`' && k <= '~') {
      _literal_buf[0] = k - '`';
      _literal_buf[1] = 0;
      return _literal_buf;
    }
  }

  return notation;
}

// Parse a single key with modifiers (e.g., "C-w" or "M-x")
static bool parse_single_key(const char *notation, int *key_out,
                             int *mods_out) {
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

  if (len == 0)
    return false;

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
    if (isalpha((unsigned char)c)) {
      *key_out = toupper(
          (unsigned char)c); // GLFW always uses uppercase for letter keys

      // If the character in notation is uppercase, it means Shift is required
      if (isupper((unsigned char)c)) {
        mods |= GLFW_MOD_SHIFT;
      }
    } else {
      *key_out = c;
    }
  } else {
    // Handle special key names
    if (strcmp(key_part, "SPC") == 0) {
      *key_out = GLFW_KEY_SPACE;
    } else if (strcmp(key_part, "RET") == 0 ||
               strcmp(key_part, "<return>") == 0) {
      *key_out = GLFW_KEY_ENTER;
    } else if (strcmp(key_part, "TAB") == 0 || strcmp(key_part, "<tab>") == 0) {
      *key_out = GLFW_KEY_TAB;
    } else if (strcmp(key_part, "ESC") == 0) {
      *key_out = GLFW_KEY_ESCAPE;
    } else if (strcmp(key_part, "DEL") == 0 ||
               strcmp(key_part, "<backspace>") == 0) {
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
  if (!notation || !chord)
    return false;
  *chord = (KeyChord){0};
  /* Common keybinding operations no longer allocate or use strtok's global
   * parser state. Reject overlong tokens or excess keys instead of silently
   * truncating a chord after four keys. */
  const char *cursor = notation;
  while (*cursor) {
    while (*cursor == ' ')
      ++cursor;
    if (!*cursor)
      break;
    if (chord->length >= 4)
      return false;
    char token[96];
    size_t length = 0;
    while (cursor[length] && cursor[length] != ' ') {
      if (length + 1 >= sizeof(token))
        return false;
      ++length;
    }
    if (!length)
      return false;
    memcpy(token, cursor, length);
    token[length] = 0;
    int key = 0, mods = 0;
    if (!parse_single_key(token, &key, &mods))
      return false;
    chord->keys[chord->length] = key;
    chord->mods[chord->length] = mods;
    ++chord->length;
    cursor += length;
  }
  return chord->length != 0;
}

bool keychord_bind(KeyChordMap *map, const char *notation,
                   KeyChordAction action, const char *description,
                   int action_type) {
  if (!map || !notation || !action)
    return false;

  KeyChord chord;
  if (!parse_keychord_notation(notation, &chord))
    return false;

  // Check if binding already exists
  for (size_t i = 0; i < map->count; i++) {
    if (keychord_equal(&map->bindings[i].chord, &chord)) {
      // Update binding
      release_binding_action(&map->bindings[i]);
      map->bindings[i].action_type = ACTION_C_FUNCTION;
      map->bindings[i].action.c_action = action;
      map->bindings[i].command_proc = NULL;
      map->bindings[i].command_name = NULL;
      map->bindings[i].action_type_flag |= action_type;
      keymap_touch(map);

      // Free and update description
      if (map->bindings[i].description) {
        free(map->bindings[i].description);
      }
      map->bindings[i].description = description ? strdup(description) : NULL;

      return true;
    }
  }

  // Add new binding
  if (map->count >= map->capacity) {
    map->capacity *= 2;
    map->bindings =
        realloc(map->bindings, map->capacity * sizeof(KeyChordBinding));
  }

  KeyChordBinding *binding = &map->bindings[map->count];
  binding->chord = chord;
  binding->action_type = ACTION_C_FUNCTION;
  binding->action.c_action = action;
  binding->command_proc = NULL;
  binding->command_name = NULL;
  binding->description =
      description ? strdup(description) : NULL; // Always strdup!
  binding->notation = strdup(notation);
  binding->action_type_flag = action_type;

  map->count++;
  keymap_touch(map);
  return true;
}

bool keychord_bind_monad(KeyChordMap *map, const char *notation,
                         monad_value_t *monad_proc, const char *description,
                         int action_type) {
  return keychord_bind_monad_named(map, notation, monad_proc, NULL, description,
                                   action_type);
}

bool keychord_bind_monad_named(KeyChordMap *map, const char *notation,
                               monad_value_t *monad_proc,
                               const char *command_name,
                               const char *description, int action_type) {
  if (!map || !notation || !monad_proc)
    return false;
  KeyChord chord = {0};
  if (!parse_keychord_notation(notation, &chord))
    return false;

  /* Allocate ALL replacement metadata before retiring an existing callback.
   * An allocation failure must preserve the previous binding and its retain. */
  char *new_name = command_name ? strdup(command_name) : NULL;
  char *new_description = description ? strdup(description) : NULL;
  if ((command_name && !new_name) || (description && !new_description)) {
    free(new_name);
    free(new_description);
    return false;
  }
  for (size_t i = 0; i < map->count; ++i) {
    if (!keychord_equal(&map->bindings[i].chord, &chord))
      continue;
    KeyChordBinding *entry = &map->bindings[i];
    monad_value_retain(monad_proc);
    release_binding_action(entry);
    entry->action_type = ACTION_MONAD_PROC;
    entry->action.monad_proc = monad_proc;
    entry->command_proc = NULL;
    entry->command_name = new_name;
    free(entry->description);
    entry->description = new_description;
    entry->action_type_flag |= action_type;
    entry->managed_monad =
        command_name && strcmp(command_name, "obsidian.monad") == 0;
    keymap_touch(map);
    return true;
  }

  char *new_notation = strdup(notation);
  if (!new_notation) {
    free(new_name);
    free(new_description);
    return false;
  }
  if (map->count == map->capacity) {
    size_t capacity = map->capacity ? map->capacity * 2 : KEYCHORD_INIT_CAP;
    if (capacity < map->capacity ||
        capacity > SIZE_MAX / sizeof(KeyChordBinding)) {
      free(new_name);
      free(new_description);
      free(new_notation);
      return false;
    }
    KeyChordBinding *grown =
        realloc(map->bindings, capacity * sizeof(KeyChordBinding));
    if (!grown) {
      free(new_name);
      free(new_description);
      free(new_notation);
      return false;
    }
    map->bindings = grown;
    map->capacity = capacity;
  }
  KeyChordBinding *entry = &map->bindings[map->count];
  *entry = (KeyChordBinding){0};
  entry->chord = chord;
  entry->action_type = ACTION_MONAD_PROC;
  entry->action.monad_proc = monad_proc;
  entry->command_name = new_name;
  entry->description = new_description;
  entry->notation = new_notation;
  entry->action_type_flag = action_type;
  entry->managed_monad =
      command_name && strcmp(command_name, "obsidian.monad") == 0;
  monad_value_retain(monad_proc);
  ++map->count;
  keymap_touch(map);
  return true;
}

bool keychord_bind_managed_monad_named(KeyChordMap *map, const char *notation,
                                       monad_value_t *monad_proc,
                                       const char *command_name,
                                       const char *description,
                                       int action_type) {
  if (!keychord_bind_monad_named(map, notation, monad_proc, command_name,
                                 description, action_type))
    return false;
  KeyChordBinding *binding = keychord_find_binding(map, notation);
  if (!binding || binding->action_type != ACTION_MONAD_PROC)
    return false;
  binding->managed_monad = true;
  return true;
}

bool keychord_bind_native_named(KeyChordMap *map, const char *notation,
                                KeyChordAction action,
                                monad_value_t *command_proc,
                                const char *command_name,
                                const char *description, int action_type) {
  if (!map || !notation || !action || !command_proc)
    return false;

  KeyChord chord;
  if (!parse_keychord_notation(notation, &chord))
    return false;

  for (size_t i = 0; i < map->count; i++) {
    if (!keychord_equal(&map->bindings[i].chord, &chord))
      continue;

    monad_value_retain(command_proc);
    release_binding_action(&map->bindings[i]);
    map->bindings[i].action_type = ACTION_C_FUNCTION;
    map->bindings[i].action.c_action = action;
    map->bindings[i].command_proc = command_proc;
    map->bindings[i].command_name = command_name ? strdup(command_name) : NULL;
    free(map->bindings[i].description);
    map->bindings[i].description = description ? strdup(description) : NULL;
    map->bindings[i].action_type_flag |= action_type;
    keymap_touch(map);
    return true;
  }

  if (map->count + 1 > map->capacity) {
    size_t new_cap = map->capacity * 2;
    KeyChordBinding *grown =
        realloc(map->bindings, new_cap * sizeof(KeyChordBinding));
    if (!grown)
      return false;
    map->bindings = grown;
    map->capacity = new_cap;
  }

  KeyChordBinding *binding = &map->bindings[map->count++];
  binding->chord = chord;
  binding->action_type = ACTION_C_FUNCTION;
  binding->action.c_action = action;
  binding->command_proc = command_proc;
  binding->command_name = command_name ? strdup(command_name) : NULL;
  monad_value_retain(command_proc);
  binding->description = description ? strdup(description) : NULL;
  binding->notation = strdup(notation);
  binding->action_type_flag = action_type;
  keymap_touch(map);
  return true;
}

bool keychord_bind_native(KeyChordMap *map, const char *notation,
                          KeyChordAction action, monad_value_t *command_proc,
                          const char *description, int action_type) {
  return keychord_bind_native_named(map, notation, action, command_proc, NULL,
                                    description, action_type);
}

bool keychord_bind_callback(KeyChordMap *map, const char *notation,
                            KeyChordCallback callback, void *userdata,
                            KeyChordCallbackDestroy destroy,
                            const char *description, int action_type) {
  if (!map || !notation || !callback) {
    return false;
  }

  KeyChord chord;
  if (!parse_keychord_notation(notation, &chord))
    return false;

  // Check if binding already exists
  for (size_t i = 0; i < map->count; i++) {
    if (keychord_equal(&map->bindings[i].chord, &chord)) {
      release_binding_action(&map->bindings[i]);
      map->bindings[i].action_type = ACTION_CALLBACK;
      map->bindings[i].action.callback.invoke = callback;
      map->bindings[i].action.callback.destroy = destroy;
      map->bindings[i].action.callback.userdata = userdata;
      map->bindings[i].command_proc = NULL;
      map->bindings[i].command_name = NULL;
      map->bindings[i].action_type_flag |= action_type;
      keymap_touch(map);

      free(map->bindings[i].description);
      map->bindings[i].description = description ? strdup(description) : NULL;
      return true;
    }
  }

  // Add new binding
  if (map->count >= map->capacity) {
    map->capacity *= 2;
    map->bindings =
        realloc(map->bindings, map->capacity * sizeof(KeyChordBinding));
  }

  KeyChordBinding *binding = &map->bindings[map->count];
  binding->chord = chord;
  binding->action_type = ACTION_CALLBACK;
  binding->action.callback.invoke = callback;
  binding->action.callback.destroy = destroy;
  binding->action.callback.userdata = userdata;
  binding->command_proc = NULL;
  binding->command_name = NULL;
  binding->description = description ? strdup(description) : NULL;
  binding->notation = strdup(notation);
  binding->action_type_flag = action_type;

  map->count++;
  keymap_touch(map);
  return true;
}

bool keychord_unbind(KeyChordMap *map, const char *notation) {
  if (!map || !notation)
    return false;

  KeyChord chord;
  if (!parse_keychord_notation(notation, &chord))
    return false;

  for (size_t i = 0; i < map->count; i++) {
    if (keychord_equal(&map->bindings[i].chord, &chord)) {
      release_binding_action(&map->bindings[i]);
      free(map->bindings[i].description);
      free(map->bindings[i].notation);

      // Move last binding to this position
      if (i < map->count - 1) {
        map->bindings[i] = map->bindings[map->count - 1];
      }
      map->count--;
      keymap_touch(map);
      return true;
    }
  }

  return false;
}

KeyChordBinding *keychord_lookup_binding(KeyChordMap *map,
                                         const KeyChord *chord,
                                         int action_type) {
  if (!map || !chord)
    return NULL;

  uint64_t hash = (uint64_t)(uintptr_t)map ^
                  (map->lookup_generation * UINT64_C(0x9e3779b97f4a7c15)) ^
                  (uint64_t)(unsigned)action_type;
  for (size_t i = 0; i < chord->length; i++) {
    hash ^= (uint64_t)(unsigned)(chord->keys[i] + 1) * UINT64_C(0x100000001b3);
    hash ^= (uint64_t)(unsigned)(chord->mods[i] + 1) * UINT64_C(0x9e3779b1);
  }
  size_t slot = (size_t)(hash % KEYCHORD_LOOKUP_CACHE_CAPACITY);
  KeyChordLookupCacheEntry *cached = &keychord_lookup_cache[slot];
  if (cached->map == map && cached->generation == map->lookup_generation &&
      cached->action_type == action_type &&
      keychord_equal(&cached->chord, chord)) {
#ifdef OBS_KEYCHORD_PROFILE
    keychord_lookup_cache_hit_count++;
#endif
    if (cached->binding_index == SIZE_MAX ||
        cached->binding_index >= map->count)
      return NULL;
    return &map->bindings[cached->binding_index];
  }

  for (size_t i = 0; i < map->count; i++) {
    if (keychord_equal(&map->bindings[i].chord, chord) &&
        (map->bindings[i].action_type_flag & action_type)) {
      cached->map = map;
      cached->chord = *chord;
      cached->action_type = action_type;
      cached->generation = map->lookup_generation;
      cached->binding_index = i;
      return &map->bindings[i];
    }
  }

  cached->map = map;
  cached->chord = *chord;
  cached->action_type = action_type;
  cached->generation = map->lookup_generation;
  cached->binding_index = SIZE_MAX;

  return NULL;
}

// Helper to execute a binding
/// execute_binding — implementation group
static void execute_binding(KeyChordBinding *binding) {
  if (!binding)
    return;
  if (skip_next_binding_execution) {
    skip_next_binding_execution = false;
    return;
  }
  if (binding->action_type == ACTION_C_FUNCTION) {
    if (keychord_native_action_guard)
      keychord_native_action_guard(binding->action.c_action);
    else
      binding->action.c_action();
  } else if (binding->action_type == ACTION_CALLBACK) {
    binding->action.callback.invoke(binding->action.callback.userdata);
  } else if (binding->action_type == ACTION_MONAD_PROC) {
    if (keychord_call_interactively_named)
      keychord_call_interactively_named(binding->action.monad_proc,
                                        binding->command_name);
    else if (keychord_call_interactively)
      keychord_call_interactively(binding->action.monad_proc);
  }
}

void keychord_invoke_binding(KeyChordBinding *binding) {
  execute_binding(binding);
}

monad_value_t *keychord_binding_command(const KeyChordBinding *binding) {
  if (!binding)
    return NULL;
  if (binding->command_proc)
    return binding->command_proc;
  if (binding->action_type == ACTION_MONAD_PROC)
    return binding->action.monad_proc;
  return NULL;
}

const char *keychord_binding_command_name(const KeyChordBinding *binding) {
  return binding ? binding->command_name : NULL;
}

bool keychord_process_key(KeyChordMap *map, int key, int action, int mods) {
  if (!map)
    return false;

  // Ignore standalone modifier key presses in chord building
  if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT ||
      key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
      key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT ||
      key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER) {
    return false;
  }
  // Create a chord for the current key press
  KeyChord single_key_chord;
  single_key_chord.keys[0] = key;
  single_key_chord.mods[0] = mods;
  single_key_chord.length = 1;

  // Check what action types are bound to this key
  KeyChordBinding *press_binding =
      keychord_lookup_binding(map, &single_key_chord, GLFW_PRESS);
  KeyChordBinding *release_binding =
      keychord_lookup_binding(map, &single_key_chord, GLFW_RELEASE);
  KeyChordBinding *repeat_binding =
      keychord_lookup_binding(map, &single_key_chord, GLFW_REPEAT);

  // If we're on PRESS but this key is ONLY bound to RELEASE or REPEAT, consume
  // it
  if (action == GLFW_PRESS && !press_binding &&
      (release_binding || repeat_binding)) {
    if (map->current_chord.length > 0) {
      keychord_reset_state(map);
    }
    return true;
  }

  // Handle single-key bindings (when not building a multi-key chord)
  if (map->current_chord.length == 0) {
    KeyChordBinding *binding =
        keychord_lookup_binding(map, &single_key_chord, action);
    if (binding) {
      // Call BEFORE hook
      if (internal_before_keychord_hook) {
        internal_before_keychord_hook(binding->notation, binding);
      }

      // Execute the action
      execute_binding(binding);

      // Call AFTER hook
      if (internal_after_keychord_hook) {
        internal_after_keychord_hook(binding->notation, binding);
      }

      return true;
    }
  }

  // Only build multi-key chords on PRESS
  if (action != GLFW_PRESS) {
    return false;
  }

  double current_time = glfwGetTime();

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
  KeyChordBinding *multi_binding =
      keychord_lookup_binding(map, &map->current_chord, GLFW_PRESS);

  if (multi_binding) {
    // Call BEFORE hook
    if (internal_before_keychord_hook) {
      internal_before_keychord_hook(multi_binding->notation, multi_binding);
    }

    // Execute the action
    execute_binding(multi_binding);

    // Call AFTER hook
    if (internal_after_keychord_hook) {
      internal_after_keychord_hook(multi_binding->notation, multi_binding);
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
  if (!map || !notation)
    return NULL;

  for (size_t i = 0; i < map->count; i++) {
    if (map->bindings[i].notation &&
        strcmp(map->bindings[i].notation, notation) == 0) {
      return &map->bindings[i];
    }
  }

  return NULL;
}

void keymap_print_bindings(KeyChordMap *map) {
  if (!map)
    return;

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
  if (!a || !b)
    return false;
  if (a->length != b->length)
    return false;

  for (size_t i = 0; i < a->length; i++) {
    int a_key = a->keys[i];
    int b_key = b->keys[i];
    int a_mods = a->mods[i];
    int b_mods = b->mods[i];

    // Keys must match
    if (a_key != b_key)
      return false;

    // For letter keys (A-Z), normalize shift handling
    if (a_key >= GLFW_KEY_A && a_key <= GLFW_KEY_Z) {
      // Extract non-shift modifiers
      int a_other_mods = a_mods & ~GLFW_MOD_SHIFT;
      int b_other_mods = b_mods & ~GLFW_MOD_SHIFT;

      // Non-shift modifiers must match
      if (a_other_mods != b_other_mods)
        return false;

      // Shift must match exactly
      bool a_has_shift = (a_mods & GLFW_MOD_SHIFT) != 0;
      bool b_has_shift = (b_mods & GLFW_MOD_SHIFT) != 0;

      if (a_has_shift != b_has_shift)
        return false;
    } else {
      // For non-letter keys, all modifiers must match exactly
      if (a_mods != b_mods)
        return false;
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
  if (frame_local_map)
    keychord_reset_state(frame_local_map);
}

// Keymap stack for layered keymaps (local maps have priority over global)
KeyChordMap *keymap_stack[MAX_KEYMAP_STACK] = {NULL};
size_t keymap_stack_count = 0;

void keymap_stack_push(KeyChordMap *map) {
  if (keymap_stack_count < MAX_KEYMAP_STACK) {
    keymap_stack[keymap_stack_count++] = map;
    keymap_stack_touch();
  }
}

void keymap_stack_pop(void) {
  if (keymap_stack_count > 0) {
    keymap_stack_count--;
    keymap_stack[keymap_stack_count] = NULL;
    keymap_stack_touch();
  }
}

void keymap_stack_clear(void) {
  for (size_t i = 0; i < keymap_stack_count; i++) {
    keymap_stack[i] = NULL;
  }
  keymap_stack_count = 0;
  keymap_stack_touch();
}

void keychord_set_frame_local_map(KeyChordMap *map) {
  if (frame_local_map == map) return;
  frame_local_map = map;
  if (frame_local_map)
    keychord_reset_state(frame_local_map);
  keymap_stack_touch();
}

KeyChordMap *keychord_get_frame_local_map(void) {
  return frame_local_map;
}

KeyChordMap *keymap_stack_get_local(void) {
  if (keymap_stack_count > 0) {
    return keymap_stack[keymap_stack_count - 1];
  }
  return NULL;
}

void keymap_set_overriding(KeyChordMap *map) {
  overriding_terminal_local_map = map;
  overriding_binding_seen = false;
  overriding_current_binding = NULL;
  if (map)
    keychord_reset_state(map);
  keymap_stack_touch();
}

void keymap_clear_overriding(void) {
  overriding_terminal_local_map = NULL;
  overriding_binding_seen = false;
  overriding_current_binding = NULL;
  keymap_stack_touch();
}

KeyChordMap *keymap_get_overriding(void) {
  return overriding_terminal_local_map;
}

size_t keychord_collect_active_bindings(KeyChordBinding **out,
                                        size_t capacity) {
  if (!out || !capacity)
    return 0;
  size_t count = 0;
  KeyChordMap *maps[MAX_KEYMAP_STACK + 3];
  size_t map_count = 0;
  if (overriding_terminal_local_map)
    maps[map_count++] = overriding_terminal_local_map;
  for (size_t i = keymap_stack_count; i > 0; --i)
    if (keymap_stack[i - 1])
      maps[map_count++] = keymap_stack[i - 1];
  if (frame_local_map)
    maps[map_count++] = frame_local_map;
  maps[map_count++] = &keymap;

  for (size_t m = 0; m < map_count && count < capacity; ++m) {
    KeyChordMap *map = maps[m];
    for (size_t i = 0; i < map->count && count < capacity; ++i) {
      KeyChordBinding *candidate = &map->bindings[i];
      bool shadowed = false;
      for (size_t prior = 0; prior < count; ++prior) {
        if (keychord_equal(&out[prior]->chord, &candidate->chord)) {
          shadowed = true;
          break;
        }
      }
      if (!shadowed)
        out[count++] = candidate;
    }
  }
  return count;
}

bool keymap_overriding_binding_seen(void) { return overriding_binding_seen; }

monad_value_t *keymap_overriding_binding_command(void) {
  return keychord_binding_command(overriding_current_binding);
}

// Check if a chord is a prefix of any binding in a keymap
/// is_chord_prefix — implementation group
static bool is_chord_prefix(KeyChordMap *map, const KeyChord *current_chord) {
  if (!map || !current_chord)
    return false;

  for (size_t i = 0; i < map->count; i++) {
    KeyChord *bound = &map->bindings[i].chord;

    // Check if bound chord is longer than current
    if (bound->length <= current_chord->length) {
      continue;
    }

    // Check if current chord matches the beginning of this binding
    bool matches = true;
    for (size_t j = 0; j < current_chord->length; j++) {
      if (bound->keys[j] != current_chord->keys[j] ||
          bound->mods[j] != current_chord->mods[j]) {
        matches = false;
        break;
      }
    }

    if (matches) {
      return true;
    }
  }

  return false;
}

// Process key with keymap stack (local maps first, then global)
bool keychord_process_key_with_stack(int key, int action, int mods) {
  pending_input = true;
  overriding_binding_seen = false;
  overriding_current_binding = NULL;
  if (action != GLFW_REPEAT)
    keychord_repeat_cache.valid = false;
  // Raw interceptor runs before modifier filtering and binding lookup
  /* if (internal_raw_key_interceptor */
  /*  && internal_raw_key_interceptor(key, action, mods)) */
  /*     return true; */

  for (int i = 0; i < raw_interceptor_count; i++)
    if (raw_interceptors[i](key, action, mods))
      return true;

  // Ignore standalone modifier keys
  if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT ||
      key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
      key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT ||
      key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER) {
    return false;
  }
  // Create a chord for the current key
  KeyChord single_key_chord;
  single_key_chord.keys[0] = key;
  single_key_chord.mods[0] = mods;
  single_key_chord.length = 1;

  // Check if we're building a chord in any keymap
  bool building_chord = false;
  KeyChordMap *active_chord_map = NULL;

  if (overriding_terminal_local_map &&
      overriding_terminal_local_map->current_chord.length > 0) {
    building_chord = true;
    active_chord_map = overriding_terminal_local_map;
  }

  // Check local keymaps first
  for (int i = keymap_stack_count - 1; i >= 0 && !building_chord; i--) {
    if (keymap_stack[i] && keymap_stack[i]->current_chord.length > 0) {
      building_chord = true;
      active_chord_map = keymap_stack[i];
      break;
    }
  }

  if (!building_chord && frame_local_map &&
      frame_local_map->current_chord.length > 0) {
    building_chord = true;
    active_chord_map = frame_local_map;
  }

  // Check global keymap
  if (!building_chord && keymap.current_chord.length > 0) {
    building_chord = true;
    active_chord_map = &keymap;
  }

  // Handle single-key bindings (when not building a chord)
  if (!building_chord) {
    if (s_prefix_capture_enabled && s_prefix_capture_priority &&
        !currentTextCallback &&
        keychord_consume_prefix_key(key, action, mods))
      return true;
    if (action == GLFW_REPEAT) {
      KeyChordBinding *repeat_binding = repeat_cache_lookup(key, mods);
      if (repeat_binding) {
        if (overriding_terminal_local_map) {
          KeyChordMap *map = overriding_terminal_local_map;
          KeyChord chord = {.length = 1};
          chord.keys[0] = key;
          chord.mods[0] = mods;
          if (keychord_lookup_binding(map, &chord, action) != repeat_binding)
            keymap_clear_overriding();
        }
        if (internal_before_keychord_hook)
          internal_before_keychord_hook(repeat_binding->notation,
                                        repeat_binding);
        execute_binding(repeat_binding);
        if (internal_after_keychord_hook)
          internal_after_keychord_hook(repeat_binding->notation,
                                       repeat_binding);
        return true;
      }
    }
    /* `overriding-terminal-local-map' has precedence over every normal
     * map, but an unbound key must still fall through to the ordinary
     * lookup just as it does in Emacs. */
    if (overriding_terminal_local_map) {
      KeyChordBinding *override_binding = keychord_lookup_binding(
          overriding_terminal_local_map, &single_key_chord, action);
      if (override_binding) {
        overriding_binding_seen = true;
        overriding_current_binding = override_binding;
        if (internal_before_keychord_hook)
          internal_before_keychord_hook(override_binding->notation,
                                        override_binding);
        execute_binding(override_binding);
        if (internal_after_keychord_hook)
          internal_after_keychord_hook(override_binding->notation,
                                       override_binding);
        if (action == GLFW_PRESS)
          repeat_cache_store(key, mods, overriding_terminal_local_map,
                             override_binding);
        return true;
      }

      if (action == GLFW_PRESS &&
          is_chord_prefix(overriding_terminal_local_map, &single_key_chord)) {
        overriding_terminal_local_map->current_chord = single_key_chord;
        overriding_terminal_local_map->last_key_time = glfwGetTime();
        return true;
      }
      /* A transient map persists only while its own binding is used.
       * Clear it before falling through so the same input is handled
       * normally (including prefix chords and self-insert events). */
      if (action == GLFW_PRESS)
        keymap_clear_overriding();
    }

    // An active overriding map has command priority over prefix capture.
    // Otherwise keys such as C-M-- are consumed as negative arguments before
    // a transient map (for example global text scaling) can handle them.
    if (s_prefix_capture_enabled && !currentTextCallback &&
        keychord_consume_prefix_key(key, action, mods))
      return true;

    // Check local keymaps first (top of stack has priority)
    for (int i = keymap_stack_count - 1; i >= 0; i--) {
      KeyChordMap *local = keymap_stack[i];
      if (!local)
        continue;

      KeyChordBinding *binding =
          keychord_lookup_binding(local, &single_key_chord, action);
      if (binding) {
        // Found in local keymap - execute it
        if (internal_before_keychord_hook) {
          internal_before_keychord_hook(binding->notation, binding);
        }

        execute_binding(binding);

        if (internal_after_keychord_hook) {
          internal_after_keychord_hook(binding->notation, binding);
        }
        if (action == GLFW_PRESS)
          repeat_cache_store(key, mods, local, binding);

        return true;
      }
    }

    if (frame_local_map) {
      KeyChordBinding *binding =
          keychord_lookup_binding(frame_local_map, &single_key_chord, action);
      if (binding) {
        if (internal_before_keychord_hook)
          internal_before_keychord_hook(binding->notation, binding);
        execute_binding(binding);
        if (internal_after_keychord_hook)
          internal_after_keychord_hook(binding->notation, binding);
        if (action == GLFW_PRESS)
          repeat_cache_store(key, mods, frame_local_map, binding);
        return true;
      }
    }

    // Not found in any local keymap, check global
    KeyChordBinding *global_binding =
        keychord_lookup_binding(&keymap, &single_key_chord, action);
    if (global_binding) {
      if (internal_before_keychord_hook) {
        internal_before_keychord_hook(global_binding->notation, global_binding);
      }

      execute_binding(global_binding);

      if (internal_after_keychord_hook) {
        internal_after_keychord_hook(global_binding->notation, global_binding);
      }
      if (action == GLFW_PRESS)
        repeat_cache_store(key, mods, &keymap, global_binding);

      return true;
    }

    // No direct binding found, but check if this could start a multi-key chord
    // Only do this on PRESS
    if (action != GLFW_PRESS) {
      return false;
    }

    // Check if this key is a prefix in any local keymap
    for (int i = keymap_stack_count - 1; i >= 0; i--) {
      if (keymap_stack[i] &&
          is_chord_prefix(keymap_stack[i], &single_key_chord)) {
        // Start building this chord in the local keymap
        keymap_stack[i]->current_chord = single_key_chord;
        keymap_stack[i]->last_key_time = glfwGetTime();
        return true;
      }
    }

    if (frame_local_map &&
        is_chord_prefix(frame_local_map, &single_key_chord)) {
      frame_local_map->current_chord = single_key_chord;
      frame_local_map->last_key_time = glfwGetTime();
      return true;
    }

    // Check if it's a prefix in global keymap
    if (is_chord_prefix(&keymap, &single_key_chord)) {
      // Start building this chord in the global keymap
      keymap.current_chord = single_key_chord;
      keymap.last_key_time = glfwGetTime();
      return true;
    }

    // Not a binding and not a prefix
    return false;
  }

  // We're building a multi-key chord
  // Only build multi-key chords on PRESS
  if (action != GLFW_PRESS) {
    return false;
  }

  double current_time = glfwGetTime();

  // Check for chord timeout
  if (active_chord_map && current_time - active_chord_map->last_key_time >
                              active_chord_map->chord_timeout) {
    // Reset all keymaps on timeout
    for (size_t i = 0; i < keymap_stack_count; i++) {
      if (keymap_stack[i]) {
        keychord_reset_state(keymap_stack[i]);
      }
    }
    if (frame_local_map)
      keychord_reset_state(frame_local_map);
    keychord_reset_state(&keymap);
    building_chord = false;
    active_chord_map = NULL;
    // Try processing this key again as a new chord
    return keychord_process_key_with_stack(key, action, mods);
  }

  // Add key to the active chord map's current chord
  if (active_chord_map->current_chord.length < 4) {
    active_chord_map->current_chord
        .keys[active_chord_map->current_chord.length] = key;
    active_chord_map->current_chord
        .mods[active_chord_map->current_chord.length] = mods;
    active_chord_map->current_chord.length++;
    active_chord_map->last_key_time = current_time;
  }

  // Try to find a matching multi-key binding
  KeyChordBinding *multi_binding = keychord_lookup_binding(
      active_chord_map, &active_chord_map->current_chord, GLFW_PRESS);

  if (multi_binding) {
    if (active_chord_map == overriding_terminal_local_map) {
      overriding_binding_seen = true;
      overriding_current_binding = multi_binding;
    }
    if (internal_before_keychord_hook) {
      internal_before_keychord_hook(multi_binding->notation, multi_binding);
    }

    execute_binding(multi_binding);

    if (internal_after_keychord_hook) {
      internal_after_keychord_hook(multi_binding->notation, multi_binding);
    }

    keychord_reset_state(active_chord_map);
    return true;
  }

  // Check if this could be a prefix for a longer chord
  if (is_chord_prefix(active_chord_map, &active_chord_map->current_chord)) {
    // We're in the middle of a chord - consume the key
    return true;
  }

  // Not a match and not a prefix - reset and allow normal handling
  keychord_reset_state(active_chord_map);
  return false;
}

/// Prepared Monad keymap images — zero-allocation publication
/* A snapshot borrows the non-Monad bindings from the live map. Managed Monad
 * entries are intentionally dropped, then replacements are constructed in
 * private storage. No borrowed native entry is destroyed on abort. */
static bool keychord_is_managed(const KeyChordBinding *entry) {
  return entry->action_type == ACTION_MONAD_PROC &&
         (entry->managed_monad ||
          (entry->command_name &&
           strcmp(entry->command_name, "obsidian.monad") == 0));
}

size_t keychord_remove_managed_monad(KeyChordMap *map) {
  if (!map || !map->bindings)
    return 0;
  size_t write = 0, removed = 0;
  for (size_t read = 0; read < map->count; ++read) {
    KeyChordBinding *entry = &map->bindings[read];
    if (keychord_is_managed(entry)) {
      release_binding_action(entry);
      free(entry->description);
      free(entry->notation);
      ++removed;
    } else {
      if (write != read)
        map->bindings[write] = *entry;
      ++write;
    }
  }
  map->count = write;
  if (removed)
    keymap_touch(map);
  return removed;
}

void keychord_discard_prepared_image(KeyChordPreparedImage *prepared) {
  if (!prepared)
    return;
  for (size_t i = 0; i < prepared->count; ++i) {
    KeyChordBinding *binding = &prepared->bindings[i];
    if (!keychord_is_managed(binding))
      continue;
    release_binding_action(binding);
    free(binding->description);
    free(binding->notation);
  }
  free(prepared->bindings);
  *prepared = (KeyChordPreparedImage){0};
}

bool keychord_prepare_monad_image(const KeyChordMap *live,
                                  const KeyImageOperation *ops, size_t count,
                                  KeyChordPreparedImage *out) {
  if (!live || !out || (count && !ops) || out->bindings || !live->bindings ||
      live->count > live->capacity || count > SIZE_MAX - live->count)
    return false;
  size_t capacity = live->count + count;
  if (!capacity)
    capacity = 1;
  if (capacity > SIZE_MAX / sizeof(KeyChordBinding))
    return false;
  KeyChordPreparedImage prepared = {0};
  prepared.bindings = calloc(capacity, sizeof(KeyChordBinding));
  if (!prepared.bindings)
    return false;
  prepared.capacity = capacity;
  prepared.base_generation = live->lookup_generation;
  for (size_t i = 0; i < live->count; ++i) {
    if (!keychord_is_managed(&live->bindings[i]))
      prepared.bindings[prepared.count++] = live->bindings[i];
  }
  for (size_t i = 0; i < count; ++i) {
    const KeyImageOperation *op = &ops[i];
    if (!op->notation ||
        (op->action != KEY_IMAGE_BIND && op->action != KEY_IMAGE_UNBIND) ||
        ((op->action == KEY_IMAGE_BIND) != (op->procedure != NULL)))
      goto fail;
    KeyChord chord = {0};
    if (!parse_keychord_notation(op->notation, &chord))
      goto fail;
    size_t match = prepared.count;
    for (size_t j = 0; j < prepared.count; ++j) {
      if (keychord_equal(&prepared.bindings[j].chord, &chord)) {
        match = j;
        break;
      }
    }
    if (match < prepared.count) {
      KeyChordBinding *existing = &prepared.bindings[match];
      if (!keychord_is_managed(existing))
        goto fail;
      release_binding_action(existing);
      free(existing->description);
      free(existing->notation);
      prepared.bindings[match] = prepared.bindings[--prepared.count];
    }
    if (op->action == KEY_IMAGE_UNBIND)
      continue;
    if (prepared.count == prepared.capacity)
      goto fail;
    KeyChordBinding fresh = {0};
    fresh.notation = strdup(op->notation);
    const char *command_name =
        op->command_name ? op->command_name : "obsidian.monad";
    const char *description =
        op->description ? op->description : "Monad command";
    fresh.description = strdup(description);
    fresh.command_name = strdup(command_name);
    if (!fresh.notation || !fresh.description || !fresh.command_name) {
      free(fresh.notation);
      free(fresh.description);
      free(fresh.command_name);
      goto fail;
    }
    fresh.chord = chord;
    fresh.action_type = ACTION_MONAD_PROC;
    fresh.action.monad_proc = op->procedure;
    fresh.action_type_flag =
        op->action_type_flag ? op->action_type_flag : GLFW_PRESS;
    fresh.managed_monad = op->managed_monad || !op->command_name;
    monad_value_retain(op->procedure);
    prepared.bindings[prepared.count++] = fresh;
  }
  *out = prepared;
  return true;
fail:
  keychord_discard_prepared_image(&prepared);
  return false;
}

bool keychord_commit_monad_image(KeyChordMap *live,
                                 KeyChordPreparedImage *prepared) {
  if (!live || !prepared || !prepared->bindings ||
      live->lookup_generation != prepared->base_generation)
    return false;
  /* Single owner-thread pointer swap; all key strings and callbacks were
   * allocated/retained in prepare, so the live update cannot OOM. */
  KeyChordBinding *old = live->bindings;
  const size_t old_count = live->count;
  /* The terminal-local lookup may retain a pointer into this allocation.
   * Invalidate it before old storage can be reclaimed. */
  if (overriding_current_binding && old &&
      (uintptr_t)overriding_current_binding >= (uintptr_t)old &&
      (uintptr_t)overriding_current_binding - (uintptr_t)old <
          old_count * sizeof(*old)) {
    overriding_current_binding = NULL;
    overriding_binding_seen = false;
  }
  live->bindings = prepared->bindings;
  live->count = prepared->count;
  live->capacity = prepared->capacity;
  *prepared = (KeyChordPreparedImage){0};
  keymap_touch(live);
  keymap_stack_touch();
  /* Post-swap retirement: old engine-owned bindings were transferred by
   * shallow copy, while old Monad-owned bindings must be released. */
  for (size_t i = 0; i < old_count; ++i) {
    if (!keychord_is_managed(&old[i]))
      continue;
    release_binding_action(&old[i]);
    free(old[i].description);
    free(old[i].notation);
  }
  free(old);
  return true;
}
