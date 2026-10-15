/// keychords — platform subsystem
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <monad/embed.h>
#include "input.h"

/// Public interface

// Key sequence for chord support (e.g., "C-x C-s")
typedef struct {
    int keys[4];   // Support up to 4 keys in a chord
    int mods[4];   // Modifiers for each key
    size_t length; // Number of keys in the chord
} KeyChord;

typedef void (*KeyChordAction)(void);
/* Optional host guard for native actions.  Embedders use this to establish
 * their language/runtime exception boundary around the zero-allocation
 * editor operation without putting that policy into Obsidian. */
typedef void (*KeyChordNativeActionGuard)(KeyChordAction action);
typedef void (*KeyChordCallback)(void *userdata);
typedef void (*KeyChordCallbackDestroy)(void *userdata);

typedef enum {
    ACTION_C_FUNCTION,
    ACTION_MONAD_PROC,
    ACTION_CALLBACK
} ActionType;

typedef struct {
    KeyChord chord;
    ActionType action_type;
    union {
        KeyChordAction c_action;
        struct {
            KeyChordCallback invoke;
            KeyChordCallbackDestroy destroy;
            void *userdata;
        } callback;
        monad_value_t *monad_proc; // ACTION_MONAD_PROC: dispatched through the
                           // registered CallInteractivelyFn (the editor's
                           // interactive-command path, with prefix args)
    } action;
    /* Optional canonical editor command identity for a native C action.  The
     * dispatcher uses action.c_action directly; this retained value is only
     * for command-loop hooks and introspection. */
    monad_value_t *command_proc;
    char *command_name;
    char *description;
    char *notation;       // Original notation
    int action_type_flag; // PRESS, RELEASE, or REPEAT (or bitwise combination)
    bool managed_monad;   // Replaced atomically by the live Monad keymap image
} KeyChordBinding;

typedef struct {
    KeyChordBinding *bindings;
    size_t count;
    size_t capacity;
    KeyChord current_chord;
    double last_key_time;
    double chord_timeout;
    uint64_t lookup_generation;
} KeyChordMap;

/// Prepared Monad image — allocation-free owner-thread publication
/* Preparation owns new script bindings, but borrows native bindings until
 * commit. Never call the prepared-image API across concurrent keymap writes.
 * The compiler image must be pinned by MonadC while callbacks are retained. */
typedef enum { KEY_IMAGE_BIND = 1, KEY_IMAGE_UNBIND = 2 } KeyImageAction;
typedef struct {
    KeyImageAction action;
    const char *notation;
    monad_value_t *procedure;
    const char *command_name;
    const char *description;
    bool managed_monad;
    int action_type_flag;
} KeyImageOperation;
typedef struct {
    KeyChordBinding *bindings;
    size_t count;
    size_t capacity;
    uint64_t base_generation;
} KeyChordPreparedImage;
bool keychord_prepare_monad_image(const KeyChordMap *live,
    const KeyImageOperation *operations, size_t count,
    KeyChordPreparedImage *prepared);
void keychord_discard_prepared_image(KeyChordPreparedImage *prepared);
/* Commits with NO allocation, updating generation caches once. The caller
 * must ensure the owner-thread/keymap safe point and a matching generation.
 * Releases old Monad callbacks during post-swap retirement. */
bool keychord_commit_monad_image(KeyChordMap *live,
                                 KeyChordPreparedImage *prepared);
/* Single-pass cleanup, replacing the former repeated parse/unbind loop. */
size_t keychord_remove_managed_monad(KeyChordMap *map);

// Global keymap
extern KeyChordMap keymap;
extern bool pending_input; // Maybe it should part of the keymap..


// Keymap stack for layered keymaps
#define MAX_KEYMAP_STACK 8
extern KeyChordMap *keymap_stack[MAX_KEYMAP_STACK];
extern size_t keymap_stack_count;
void keychord_set_frame_local_map(KeyChordMap *map);
KeyChordMap *keychord_get_frame_local_map(void);



// Keychord Hook
typedef void (*AfterKeychordHook)(const char *notation, KeyChordBinding *binding);
extern AfterKeychordHook internal_after_keychord_hook;
void register_after_keychord_hook(AfterKeychordHook hook);

typedef void (*BeforeKeychordHook)(const char *notation, KeyChordBinding *binding);
extern BeforeKeychordHook internal_before_keychord_hook;
void register_before_keychord_hook(BeforeKeychordHook hook);


typedef bool (*RawKeyInterceptor)(int key, int action, int mods);
extern RawKeyInterceptor internal_raw_key_interceptor;
void register_raw_key_interceptor(RawKeyInterceptor interceptor);

// Replace single interceptor with a stack
#define MAX_RAW_INTERCEPTORS 8
void register_raw_key_interceptor(RawKeyInterceptor interceptor);
void unregister_raw_key_interceptor(RawKeyInterceptor interceptor);


typedef void (*CallInteractivelyFn)(monad_value_t *proc);
extern CallInteractivelyFn keychord_call_interactively;
void register_call_interactively(CallInteractivelyFn fn);
/* Optional metadata-aware variant.  Existing embedders may keep registering
 * the one-argument callback; the dispatcher uses this only when installed. */
typedef void (*CallInteractivelyNamedFn)(monad_value_t *proc,
                                         const char *command_name);
extern CallInteractivelyNamedFn keychord_call_interactively_named;
void register_call_interactively_named(CallInteractivelyNamedFn fn);

extern KeyChordNativeActionGuard keychord_native_action_guard;
void register_keychord_native_action_guard(KeyChordNativeActionGuard guard);


const char *key_to_notation(int key, int mods);
const char *notation_to_literal(const char *notation);

extern bool skip_next_binding_execution;


void keymap_init(KeyChordMap *map);
void keymap_free(KeyChordMap *map);
bool parse_keychord_notation(const char *notation, KeyChord *chord);
bool keychord_bind(KeyChordMap *map, const char *notation, KeyChordAction action,
                   const char *description, int action_type);
// Bind a Monad procedure under a notation like "C-x 5 2".  Execution goes
// through the registered CallInteractivelyFn so commands honor interactive
// specs and prefix arguments.  The binding retains the procedure value and
// releases it when rebound or when the map is freed.
bool keychord_bind_monad(KeyChordMap *map, const char *notation,
                         monad_value_t *monad_proc, const char *description,
                         int action_type);
/* Named variant lets an embedder resolve stable command metadata once while
 * installing a binding.  The dispatcher never asks the language runtime for
 * the command name again. */
bool keychord_bind_monad_named(KeyChordMap *map, const char *notation,
                               monad_value_t *monad_proc,
                               const char *command_name,
                               const char *description, int action_type);
bool keychord_bind_managed_monad_named(
    KeyChordMap *map, const char *notation, monad_value_t *monad_proc,
    const char *command_name, const char *description, int action_type);
/* Bind a native editor action while retaining its canonical Monad procedure
 * for hooks, command identity, and describe-key.  Keypress dispatch does not
 * cross the dynamic Monad value-call adapter. */
bool keychord_bind_native(KeyChordMap *map, const char *notation,
                          KeyChordAction action,
                          monad_value_t *command_proc,
                          const char *description, int action_type);
/* Named variant used by embedders that want allocation-free command
 * bookkeeping in their command loop. */
bool keychord_bind_native_named(KeyChordMap *map, const char *notation,
                                KeyChordAction action,
                                monad_value_t *command_proc,
                                const char *command_name,
                                const char *description, int action_type);

bool keychord_bind_callback(
    KeyChordMap *map, const char *notation, KeyChordCallback callback,
    void *userdata, KeyChordCallbackDestroy destroy,
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
/* Invoke the binding through the same guarded action path as a keypress. */
void keychord_invoke_binding(KeyChordBinding *binding);
void keychord_reset_lookup_profile(void);
uint64_t keychord_lookup_cache_hits(void);
uint64_t keychord_repeat_fast_hits(void);
/* Return the command identity attached to a binding, if any. */
monad_value_t *keychord_binding_command(const KeyChordBinding *binding);
const char *keychord_binding_command_name(const KeyChordBinding *binding);

// Keymap stack operations
void keymap_stack_push(KeyChordMap *map);
void keymap_stack_pop(void);
void keymap_stack_clear(void);
KeyChordMap* keymap_stack_get_local(void); // Get the topmost local keymap (or NULL)
/* Collect effective bindings in dispatch priority order: overriding map,
 * stacked local maps (top first), then global. Shadowed chords are omitted. */
size_t keychord_collect_active_bindings(KeyChordBinding **out,
                                        size_t capacity);
bool keychord_prefix_arg_is_set(void);
bool keychord_prefix_arg_has_digits(void);
int64_t keychord_prefix_arg_value(void);
void keychord_prefix_begin_negative(void);
void keychord_clear_prefix_arg(void);
void keychord_set_prefix_capture_enabled(bool enabled);
/* Let numeric arguments precede transient overriding-map bindings. */
void keychord_set_prefix_capture_priority(bool enabled);

/* Emacs-style overriding-terminal-local-map.  Unlike the ordinary keymap
 * stack this map survives buffer/window changes and is consulted before local
 * and global maps for each input event. */
void keymap_set_overriding(KeyChordMap *map);
void keymap_clear_overriding(void);
KeyChordMap* keymap_get_overriding(void);
bool keymap_overriding_binding_seen(void);
monad_value_t *keymap_overriding_binding_command(void);

// Process key with keymap stack (checks local maps first, then global)
bool keychord_process_key_with_stack(int key, int action, int mods);
