/// bindings.c — Typed host capabilities for a live Monad world
/// Implementation
#include "bindings.h"
#include "scene_api.h"
#include "input_api.h"
#include "camera_api.h"
#include "mesh_api.h"
#include "mesh_views_api.h"
#include "sdf_api.h"
#include "sculpt_api.h"
#include "audio_api.h"
#include "render_api.h"
#include "asset_api.h"
#include "gltf_node_api.h"
#include "material_asset_api.h"
#include "foreign.h"
#include "hotreload_api.h"
#include "frame_api.h"
#include "window_api.h"
#include "live_binding_txn.h"
#include "effect_api.h"
#include "editor_api.h"
#include "command_catalog.h"
#include "vertico.h"

#include "camera.h"
#include "scene.h"
#include "keychords.h"
#include "text_editor.h"

#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

/* No C engine struct is copied into the Monad heap.  The host reference is
 * opaque, typed, generational and validated on every foreign call.  The host
 * retains ownership; scripting never frees a camera, a Scene, or a GPU asset. */
#define OBS_SCENE_TYPE "obsidian.scene"
#define OBS_CAMERA_TYPE "obsidian.camera"

typedef monad_status_t (*HostFn)(monad_object_space_t *, size_t,
                                 monad_value_t *const *, monad_value_t **,
                                 void *, monad_error_t *);

typedef struct { ObsMonadRuntime *host; int axis; } ObsAxisContext;

typedef struct {
    const char *name;
    const char *doc;
    int arity;
    HostFn function;
} HostBinding;

static monad_status_t reject(monad_error_t *error, const char *message) {
    if (error) {
        error->status = MONAD_ERROR_INVALID_ARGUMENT;
        error->message = message;
    }
    return MONAD_ERROR_INVALID_ARGUMENT;
}

/// Compiler-image publication journal
/* During monad_load_source, host registrations are staged rather than mutating
 * the live keymap/frame callback. A rejection preserves the prior callbacks. */
static ObsLiveBindingTxn s_live_txn;
static ObsCommandCatalog s_command_catalog_a;
static ObsCommandCatalog s_command_catalog_b;
static ObsCommandCatalog *s_live_commands = &s_command_catalog_a;
static ObsCommandCatalog *s_staged_commands = &s_command_catalog_b;
static bool s_live_command_catalog_staging;
static void dispatch_monad_command(monad_value_t *procedure,
                                   const char *command_name);
static void retain_script_value(void *value) {
    monad_value_retain((monad_value_t *)value);
}
static void release_script_value(void *value) {
    monad_value_release((monad_value_t *)value);
}
bool obs_monad_binding_staging(void) { return s_live_txn.active; }
bool obs_monad_binding_stage_frame(monad_value_t *proc) {
    return obs_live_txn_append(&s_live_txn, OBS_LIVE_FRAME_BIND, NULL, proc);
}
/// Prepared keymap and frame callback publication
/* The compiler publishes before this host transaction. The engine prepares
 * every new keymap allocation and procedure retention in private storage,
 * then swaps the active keymap with no allocations. If preparation fails,
 * the old host callbacks survive; full compiler+host atomicity still needs
 * candidate-level MonadC commit support. */
typedef struct ObsPreparedBindings {
    KeyChordPreparedImage image;
    monad_value_t *frame;
    bool has_keymap_image;
    bool has_frame;
} ObsPreparedBindings;

static bool prepare_live_bindings(const ObsLiveBindingTxn *tx, void *context) {
    ObsPreparedBindings *prepared = context;
    KeyImageOperation operations[OBS_LIVE_TXN_CAP];
    size_t count = 0;
    for (size_t i = 0; i < tx->count; ++i) {
        const ObsLiveOp *op = &tx->operations[i];
        if (op->action == OBS_LIVE_FRAME_BIND) {
            if (!op->value) return false;
            prepared->frame = op->value; /* journal retains value */
            prepared->has_frame = true;
        } else if (op->action == OBS_LIVE_KEY_BIND ||
                   op->action == OBS_LIVE_KEY_UNBIND) {
            operations[count++] = (KeyImageOperation){
                .action = op->action == OBS_LIVE_KEY_BIND
                           ? KEY_IMAGE_BIND : KEY_IMAGE_UNBIND,
                .notation = op->key,
                .procedure = (monad_value_t *)op->value
            };
        } else return false;
    }
    if (count == 0) return true;
    if (!keychord_prepare_monad_image(&keymap, operations, count,
                                      &prepared->image))
        return false;
    prepared->has_keymap_image = true;
    return true;
}

static void commit_live_bindings(const ObsLiveBindingTxn *tx, void *context) {
    (void)tx;
    ObsPreparedBindings *prepared = context;
    /* Exclusive owner-thread checkpoint: no other keymap mutation may run
     * between successful preparation and this swap. */
    if (prepared->has_keymap_image &&
        !keychord_commit_monad_image(&keymap, &prepared->image)) {
        /* The only legal cause here would be a programming error violating
         * owner-thread exclusivity. Never silently publish partial bindings. */
        abort();
    }
    obs_monad_window_discard_callbacks();
    if (prepared->has_frame)
        obs_monad_frame_set_callback(prepared->frame);
    else
        obs_monad_frame_discard_callback();
}

bool obs_monad_binding_begin_publication(void) {
    if (s_live_command_catalog_staging) return false;
    return obs_live_txn_begin(&s_live_txn, retain_script_value,
                              release_script_value);
}
void obs_monad_binding_finish_publication(bool published) {
    if (!s_live_txn.active) return;
    ObsPreparedBindings prepared = {0};
    const bool ok = obs_live_txn_finish_prepared(&s_live_txn, published,
        prepare_live_bindings, commit_live_bindings, &prepared);
    /* On unsuccessful preparation, only new prepared bindings are released;
     * the old live map and frame callback remain unchanged. */
    keychord_discard_prepared_image(&prepared.image);
    if (ok && published) {
        ObsCommandCatalog *old_live = s_live_commands;
        s_live_commands = s_staged_commands;
        s_staged_commands = old_live;
        obs_command_catalog_clear(s_staged_commands);
    } else {
        obs_command_catalog_clear(s_staged_commands);
    }
    if (!ok)
        fprintf(stderr, "Obsidian Monad: host callback image preparation failed\n");
}
void obs_monad_binding_abort_publication(void) {
    obs_live_txn_abort(&s_live_txn);
    obs_command_catalog_clear(s_staged_commands);
}

bool obs_monad_live_command_catalog_begin(void) {
    if (s_live_txn.active || s_live_command_catalog_staging) return false;
    obs_command_catalog_clear(s_staged_commands);
    s_live_command_catalog_staging = true;
    return true;
}

bool obs_monad_live_command_register(const char *name,
                                     monad_value_t *procedure) {
    if (!name || !*name || !procedure) return false;
    ObsCommandCatalog *catalog = s_live_command_catalog_staging
                                     ? s_staged_commands : s_live_commands;
    return obs_command_catalog_register(catalog, name, "", procedure);
}

void obs_monad_live_command_catalog_finish(bool commit) {
    if (!s_live_command_catalog_staging) return;
    if (commit) {
        ObsCommandCatalog *old_live = s_live_commands;
        s_live_commands = s_staged_commands;
        s_staged_commands = old_live;
    }
    obs_command_catalog_clear(s_staged_commands);
    s_live_command_catalog_staging = false;
}

void obs_monad_live_command_catalog_clear(void) {
    obs_command_catalog_clear(s_live_commands);
    obs_command_catalog_clear(s_staged_commands);
    s_live_command_catalog_staging = false;
}

size_t obs_monad_interactive_command_count(void) {
    return s_live_commands->count;
}
const ObsInteractiveCommand *obs_monad_interactive_command_at(size_t index) {
    return obs_command_catalog_at(s_live_commands, index);
}
bool obs_monad_interactive_command_is(const void *entry) {
    return obs_command_catalog_contains(s_live_commands, entry);
}
void obs_monad_invoke_interactive_command(const ObsInteractiveCommand *entry) {
    if (!entry || !entry->procedure) return;
    dispatch_monad_command(entry->procedure, entry->name);
}

static bool active_host(ObsMonadRuntime *host) {
    return host && host->active && host->space &&
           obs_monad_on_owner_thread();
}

static bool require_ref(monad_value_t *value, ObsMonadRuntime *host,
                        const char *type, const void *expected,
                        monad_error_t *error) {
    return obs_host_require(value, host, type, expected,
                            host ? host->generation : 0, error);
}

static monad_status_t current_scene(monad_object_space_t *space, size_t argc,
                                    monad_value_t *const *argv,
                                    monad_value_t **result, void *context,
                                    monad_error_t *error) {
    (void)argv;
    ObsMonadRuntime *host = context;
    if (argc != 0 || !result || !active_host(host))
        return reject(error, "scene reference requires the engine thread");
    return obs_host_borrow(space, host, OBS_SCENE_TYPE, scene,
                           host->generation, result, error)
           ? MONAD_OK : error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
}

static monad_status_t current_camera(monad_object_space_t *space, size_t argc,
                                     monad_value_t *const *argv,
                                     monad_value_t **result, void *context,
                                     monad_error_t *error) {
    (void)argv;
    ObsMonadRuntime *host = context;
    if (argc != 0 || !result || !active_host(host))
        return reject(error, "camera reference requires the engine thread");
    return obs_host_borrow(space, host, OBS_CAMERA_TYPE, &camera,
                           host->generation, result, error)
           ? MONAD_OK : error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
}

static monad_status_t scene_node_count(monad_object_space_t *space, size_t argc,
                                       monad_value_t *const *argv,
                                       monad_value_t **result, void *context,
                                       monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 1 || !argv || !result ||
        !require_ref(argv[0], host, OBS_SCENE_TYPE, scene, error))
        return reject(error, "expected a live obsidian.scene");
    if (scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES)
        return reject(error, "invalid scene tree bounds");
    return monad_value_make_i64(space, scene->tree.count, result, error);
}

static monad_status_t scene_node_name(monad_object_space_t *space, size_t argc,
                                      monad_value_t *const *argv,
                                      monad_value_t **result, void *context,
                                      monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result ||
        !require_ref(argv[0], host, OBS_SCENE_TYPE, scene, error))
        return reject(error, "expected a live obsidian.scene");
    int64_t index = -1;
    if (monad_value_as_i64(argv[1], &index, error) != MONAD_OK ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES ||
        index < 0 || index >= scene->tree.count)
        return reject(error, "scene node index out of bounds");
    const char *name = scene->tree.nodes[index].name;
    size_t len = 0;
    while (len < sizeof(scene->tree.nodes[index].name) && name[len]) ++len;
    return monad_value_make_string(space, name, len, result, error);
}


static monad_status_t scene_node_parent(monad_object_space_t *space, size_t argc,
                                        monad_value_t *const *argv,
                                        monad_value_t **result, void *context,
                                        monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result ||
        !require_ref(argv[0], host, OBS_SCENE_TYPE, scene, error))
        return reject(error, "expected a live obsidian.scene");
    int64_t index = -1;
    if (monad_value_as_i64(argv[1], &index, error) != MONAD_OK ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES ||
        index < 0 || index >= scene->tree.count)
        return reject(error, "scene node index out of bounds");
    return monad_value_make_i64(space, scene->tree.nodes[index].parent, result, error);
}

static monad_status_t scene_node_visible(monad_object_space_t *space, size_t argc,
                                         monad_value_t *const *argv,
                                         monad_value_t **result, void *context,
                                         monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result ||
        !require_ref(argv[0], host, OBS_SCENE_TYPE, scene, error))
        return reject(error, "expected a live obsidian.scene");
    int64_t index = -1;
    if (monad_value_as_i64(argv[1], &index, error) != MONAD_OK ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES ||
        index < 0 || index >= scene->tree.count)
        return reject(error, "scene node index out of bounds");
    /* UI-facing scene-tree flag; not a promise of GPU culling behavior. */
    *result = monad_value_bool(space, scene->tree.nodes[index].visible);
    return MONAD_OK;
}

static monad_status_t camera_axis(monad_object_space_t *space, size_t argc,
                                  monad_value_t *const *argv,
                                  monad_value_t **result, void *context,
                                  monad_error_t *error) {
    /* Axis is selected with the descriptor's stable string name.  The
     * operation itself is pure read, with no hidden scene updates. */
    ObsAxisContext *axis = context;
    if (argc != 1 || !argv || !result || !axis ||
        !require_ref(argv[0], axis->host, OBS_CAMERA_TYPE, &camera, error))
        return reject(error, "expected a live obsidian.camera");
    return monad_value_make_f64(space, camera.position[axis->axis], result, error);
}

static monad_status_t camera_set_position(monad_object_space_t *space,
                                          size_t argc, monad_value_t *const *argv,
                                          monad_value_t **result, void *context,
                                          monad_error_t *error) {
    if (!obs_monad_runtime_mutation_allowed())
        return reject(error, "engine mutations are forbidden during Monad publication");
    ObsMonadRuntime *host = context;
    if (argc != 4 || !argv || !result ||
        !require_ref(argv[0], host, OBS_CAMERA_TYPE, &camera, error))
        return reject(error, "expected a live obsidian.camera");
    double xyz[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (monad_value_as_f64(argv[i + 1], &xyz[i], error) != MONAD_OK ||
            !isfinite(xyz[i]) || fabs(xyz[i]) > 1.0e8)
            return reject(error, "camera coordinates must be finite Float values");
    }
    for (unsigned i = 0; i < 3; ++i) camera.position[i] = (float)xyz[i];
    camera_update(&camera);
    *result = monad_value_nil(space);
    return MONAD_OK;
}

static monad_status_t camera_speed(monad_object_space_t *space, size_t argc,
                                   monad_value_t *const *argv,
                                   monad_value_t **result, void *context,
                                   monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 1 || !argv || !result ||
        !require_ref(argv[0], host, OBS_CAMERA_TYPE, &camera, error))
        return reject(error, "expected a live obsidian.camera");
    return monad_value_make_f64(space, camera.movement_speed, result, error);
}

/* An actual C-owned engine behavior is now callable from compiled Monad,
 * with argument validation and foreign-reference lifetime checks. */
static monad_status_t camera_set_speed(monad_object_space_t *space, size_t argc,
                                       monad_value_t *const *argv,
                                       monad_value_t **result, void *context,
                                       monad_error_t *error) {
    if (!obs_monad_runtime_mutation_allowed())
        return reject(error, "engine mutations are forbidden during Monad publication");
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result ||
        !require_ref(argv[0], host, OBS_CAMERA_TYPE, &camera, error))
        return reject(error, "expected a live obsidian.camera");
    double speed;
    if (monad_value_as_f64(argv[1], &speed, error) != MONAD_OK ||
        !isfinite(speed) || speed < 0 || speed > 1.0e6)
        return reject(error, "camera speed must be a finite non-negative Float");
    camera.movement_speed = (float)speed;
    *result = monad_value_nil(space);
    return MONAD_OK;
}

static monad_status_t engine_version(monad_object_space_t *space, size_t argc,
                                     monad_value_t *const *argv,
                                     monad_value_t **result, void *context,
                                     monad_error_t *error) {
    (void)argv;
    if (argc != 0 || !result || !active_host(context))
        return reject(error, "host is not active");
    return monad_value_make_i64(space, 1, result, error);
}


/* Live-script control is deliberately cooperative: a script can ask for a
 * reload, but cannot compile source inside the currently executing callback. */
static monad_status_t request_reload(monad_object_space_t *space, size_t argc,
                                    monad_value_t *const *argv,
                                    monad_value_t **result, void *context,
                                    monad_error_t *error) {
    (void)argv;
    if (argc != 0 || !result || !active_host(context))
        return reject(error, "reload requests require the engine thread");
    obs_monad_engine_request_reload();
    *result = monad_value_bool(space, true);
    return MONAD_OK;
}

static monad_status_t reload_state(monad_object_space_t *space, size_t argc,
                                   monad_value_t *const *argv,
                                   monad_value_t **result, void *context,
                                   monad_error_t *error) {
    (void)argv;
    if (argc != 0 || !result || !active_host(context))
        return reject(error, "reload status requires the engine thread");
    ObsMonadReloadReport report;
    obs_monad_engine_reload_report(&report);
    return monad_value_make_i64(space, (int64_t)report.state, result, error);
}

/* Monad procedures are dispatched by the native keymap without evaluating
 * source on every keypress.  The callback enters the live image only after
 * the engine's owning-thread and recursion guards have passed. */
static _Thread_local unsigned s_command_depth;

static void dispatch_monad_command_with_text(
    monad_value_t *procedure, const char *command_name,
    const char *const *text_args, size_t text_count) {
    ObsMonadRuntime *host = obs_monad_engine();
    if (!active_host(host) || !procedure || s_command_depth >= 16) {
        fprintf(stderr, "Obsidian Monad: command is unavailable or recursive\n");
        keychord_clear_prefix_arg();
        return;
    }
    ObsCodeLease lease = {0};
    if (!obs_monad_code_enter(&lease)) {
        keychord_clear_prefix_arg();
        return;
    }
    ++s_command_depth;
    /* The running command may rebind its own key.  Pin the closure before
     * entering the language so keymap replacement cannot free it mid-call. */
    monad_value_retain(procedure);
    monad_value_t *result = NULL;
    monad_value_t *arguments[OBS_COMMAND_SPEC_MAX] = {0};
    size_t argument_count = 0, text_index = 0;
    monad_error_t error = {0};
    const ObsInteractiveCommand *command =
        obs_command_catalog_find(s_live_commands, command_name);
    const char *spec = command && command->procedure == procedure
        ? command->interactive_spec : "";
    monad_status_t status = MONAD_OK;
    for (size_t i = 0; spec[i] && status == MONAD_OK; ++i) {
        monad_value_t *value = NULL;
        if (spec[i] == 'p') {
            int64_t prefix = keychord_prefix_arg_is_set()
                ? keychord_prefix_arg_value() : 1;
            status = monad_value_make_i64(host->space, prefix, &value, &error);
        } else if (spec[i] == 'P') {
            if (keychord_prefix_arg_is_set()) {
                status = monad_value_make_i64(host->space,
                    keychord_prefix_arg_value(), &value, &error);
            } else {
                value = monad_value_nil(host->space);
                if (!value) status = MONAD_ERROR_OUT_OF_MEMORY;
            }
        } else if (spec[i] == 's' || spec[i] == 'b' || spec[i] == 'B' ||
                   spec[i] == 'f' || spec[i] == 'n') {
            if (!text_args || text_index >= text_count || !text_args[text_index]) {
                status = MONAD_ERROR_INVALID_ARGUMENT;
            } else if (spec[i] == 'n') {
                const char *text = text_args[text_index];
                char *end = NULL;
                errno = 0;
                long long number = strtoll(text, &end, 10);
                if (errno || end == text || !end || *end)
                    status = MONAD_ERROR_INVALID_ARGUMENT;
                else
                    status = monad_value_make_i64(host->space, (int64_t)number,
                                                  &value, &error);
            } else {
                const char *text = text_args[text_index];
                status = monad_value_make_string(host->space, text,
                                                 strlen(text), &value, &error);
            }
            ++text_index;
        } else if (spec[i] == 'r') {
            if (!g_text_editor.buf.mark_active) {
                status = MONAD_ERROR_INVALID_ARGUMENT;
            } else {
                size_t point = g_text_editor.buf.pt;
                size_t mark = g_text_editor.buf.mark;
                size_t start = point < mark ? point : mark;
                size_t end = point < mark ? mark : point;
                status = monad_value_make_i64(host->space, (int64_t)start,
                                              &value, &error);
                if (status == MONAD_OK &&
                    argument_count + 1 < OBS_COMMAND_SPEC_MAX) {
                    arguments[argument_count++] = value;
                    value = NULL;
                    status = monad_value_make_i64(host->space, (int64_t)end,
                                                  &value, &error);
                } else if (status == MONAD_OK) {
                    status = MONAD_ERROR_INVALID_ARGUMENT;
                }
            }
        }
        if (status == MONAD_OK) {
            if (argument_count == OBS_COMMAND_SPEC_MAX) {
                monad_value_release(value);
                status = MONAD_ERROR_INVALID_ARGUMENT;
            } else {
                arguments[argument_count++] = value;
            }
        } else {
            monad_value_release(value);
        }
    }
    if (status == MONAD_OK)
        status = monad_value_call(host->space, procedure, argument_count,
                                  argument_count ? arguments : NULL,
                                  &result, &error);
    monad_value_release(result);
    for (size_t i = 0; i < argument_count; ++i)
        monad_value_release(arguments[i]);
    monad_value_release(procedure);
    keychord_clear_prefix_arg();
    --s_command_depth;
    (void)obs_monad_code_leave(&lease);
    if (status != MONAD_OK)
        fprintf(stderr, "Obsidian Monad command: %s\n",
                error.message ? error.message : "invocation failed");
}

static void dispatch_monad_command(monad_value_t *procedure,
                                   const char *command_name) {
    const ObsInteractiveCommand *command =
        obs_command_catalog_find(s_live_commands, command_name);
    if (command && strpbrk(command->interactive_spec, "snbBfr")) {
        vertico_read_interactive_command(command);
        return;
    }
    dispatch_monad_command_with_text(procedure, command_name, NULL, 0);
}

void obs_monad_invoke_interactive_command_text(
    const ObsInteractiveCommand *entry, const char *const *text_args,
    size_t text_count) {
    if (!entry || !entry->procedure ||
        !obs_command_catalog_contains(s_live_commands, entry))
        return;
    dispatch_monad_command_with_text(entry->procedure, entry->name, text_args,
                                     text_count);
}

static monad_status_t bind_key(monad_object_space_t *space, size_t argc,
                               monad_value_t *const *argv,
                               monad_value_t **result, void *context,
                               monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result || !active_host(host) ||
        monad_value_kind(argv[1]) != MONAD_VALUE_PROCEDURE)
        return reject(error, "expected notation and a Monad procedure");
    const char *bytes = NULL;
    size_t length = 0;
    if (monad_value_bytes(argv[0], &bytes, &length, error) != MONAD_OK ||
        length == 0 || length > 95 || !bytes || memchr(bytes, 0, length))
        return reject(error, "key notation must be a short, NUL-free String");
    char notation[96];
    memcpy(notation, bytes, length);
    notation[length] = 0;
    const char *command_name = NULL;
    monad_error_t info_error = {0};
    if (monad_value_procedure_info(argv[1], &command_name, NULL, NULL,
                                   &info_error) != MONAD_OK ||
        !command_name || !*command_name)
        command_name = "Monad command";
    KeyChordBinding *existing = keychord_find_binding(&keymap, notation);
    if (existing && !(existing->action_type == ACTION_MONAD_PROC &&
                      existing->managed_monad))
        return reject(error, "cannot override an engine-owned key chord");
    KeyChord parsed = {0};
    if (s_live_txn.active && !parse_keychord_notation(notation, &parsed))
        return reject(error, "invalid live key chord notation");
    bool ok = s_live_txn.active
        ? obs_live_txn_append(&s_live_txn, OBS_LIVE_KEY_BIND, notation, argv[1])
        : keychord_bind_monad_named(&keymap, notation, argv[1],
                                    "obsidian.monad", command_name, GLFW_PRESS);
    if (!ok) return reject(error, "invalid key notation or callback journal full");
    *result = monad_value_bool(space, true);
    return MONAD_OK;
}

static monad_status_t unbind_key(monad_object_space_t *space, size_t argc,
                                 monad_value_t *const *argv,
                                 monad_value_t **result, void *context,
                                 monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 1 || !argv || !result || !active_host(host))
        return reject(error, "expected key notation");
    const char *bytes = NULL;
    size_t length = 0;
    if (monad_value_bytes(argv[0], &bytes, &length, error) != MONAD_OK ||
        length == 0 || length > 95 || !bytes || memchr(bytes, 0, length))
        return reject(error, "invalid key notation");
    char notation[96];
    memcpy(notation, bytes, length);
    notation[length] = 0;
    KeyChordBinding *binding = keychord_find_binding(&keymap, notation);
    /* Scripts cannot erase arbitrary native engine/editor bindings. */
    bool owned = binding && binding->action_type == ACTION_MONAD_PROC &&
                 binding->managed_monad;
    bool ok = s_live_txn.active
        ? (!binding || owned) && obs_live_txn_append(
            &s_live_txn, OBS_LIVE_KEY_UNBIND, notation, NULL)
        : owned && keychord_unbind(&keymap, notation);
    if (s_live_txn.active && !ok && (!binding || owned))
        return reject(error, "callback journal full");
    *result = monad_value_bool(space, ok);
    return MONAD_OK;
}

void obs_monad_discard_live_bindings(void) {
    obs_monad_frame_discard_callback();
    /* Single-pass compaction: O(bindings) instead of repeated parse/unbind
     * and keymap compaction (previously quadratic for many Monad chords). */
    (void)keychord_remove_managed_monad(&keymap);
}

void obs_monad_unregister_key_dispatch(void) {
    obs_monad_discard_live_bindings();
    obs_command_catalog_clear(s_live_commands);
    obs_command_catalog_clear(s_staged_commands);
    register_call_interactively_named(NULL);
}

/// Allocation-safe host image publication diagnostics
static monad_status_t key_image_commits(monad_object_space_t *space,
    size_t argc, monad_value_t *const *argv, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)argv;
    if (argc || !out || !active_host(context))
        return reject(error, "binding commit count requires owner thread");
    return monad_value_make_i64(space, (int64_t)s_live_txn.committed, out, error);
}
static monad_status_t key_image_rejections(monad_object_space_t *space,
    size_t argc, monad_value_t *const *argv, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)argv;
    if (argc || !out || !active_host(context))
        return reject(error, "binding rejection count requires owner thread");
    return monad_value_make_i64(space,
        (int64_t)s_live_txn.preflight_failures, out, error);
}

static monad_status_t register_interactive_command(
    monad_object_space_t *space, size_t argc, monad_value_t *const *argv,
    monad_value_t **result, void *context, monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 2 || !argv || !result || !active_host(host) ||
        monad_value_kind(argv[1]) != MONAD_VALUE_PROCEDURE)
        return reject(error, "expected interactive spec and a Monad procedure");
    const char *spec = NULL;
    size_t spec_length = 0;
    if (monad_value_bytes(argv[0], &spec, &spec_length, error) != MONAD_OK ||
        !spec || spec_length >= OBS_COMMAND_SPEC_MAX ||
        memchr(spec, 0, spec_length))
        return reject(error, "interactive spec must be a bounded NUL-free String");
    const char *name = NULL;
    monad_error_t info_error = {0};
    if (monad_value_procedure_info(argv[1], &name, NULL, NULL, &info_error) !=
            MONAD_OK ||
        !name || !*name)
        return reject(error, "interactive procedure must have a stable name");
    char owned_spec[OBS_COMMAND_SPEC_MAX];
    memcpy(owned_spec, spec, spec_length);
    owned_spec[spec_length] = 0;
    ObsCommandCatalog *catalog =
        s_live_txn.active ? s_staged_commands : s_live_commands;
    if (!obs_command_catalog_register(catalog, name, owned_spec, argv[1]))
        return reject(error, "interactive command catalog is full or invalid");
    *result = monad_value_bool(space, true);
    return MONAD_OK;
}

bool obs_monad_register_bindings(ObsMonadRuntime *host, monad_error_t *error) {
    if (!active_host(host)) {
        reject(error, "cannot register bindings without an active Monad host");
        return false;
    }
    /* Context objects remain stable for the entire owned object space. */
    static ObsAxisContext axes[3];
    for (int i = 0; i < 3; ++i) {
        axes[i].host = host;
        axes[i].axis = i;
    }
    static const HostBinding bindings[] = {
        {"obs-engine-api-version", "Version of Obsidian's Monad host API.", 0, engine_version},
        {"binding-commits", "Number of prepared host callback image commits.", 0, key_image_commits},
        {"binding-rejections", "Host callback image preparations rejected.", 0, key_image_rejections},
        {"obs-command-register!", "Register a named Monad procedure and its interactive argument spec.", 2, register_interactive_command},
        {"command-register!", "Register a named Monad procedure and its interactive argument spec.", 2, register_interactive_command},
        {"obs-current-scene", "Borrow the live scene as an opaque typed host reference.", 0, current_scene},
        {"scene", "Borrow the current scene. Root of the engine DSL.", 0, current_scene},
        {"obs-scene-node-count", "Number of nodes in the live scene hierarchy.", 1, scene_node_count},
        {"node-count", "Number of nodes in a scene.", 1, scene_node_count},
        {"obs-scene-node-name", "Name of a live scene node by zero-based index.", 2, scene_node_name},
        {"obs-scene-node-parent", "Parent node index (-1 for root).", 2, scene_node_parent},
        {"obs-scene-node-visible?", "Read the scene tree UI visibility flag.", 2, scene_node_visible},
        {"obs-current-camera", "Borrow the live camera as an opaque typed host reference.", 0, current_camera},
        {"camera", "Borrow the current camera.", 0, current_camera},
        {"obs-camera-x", "Read camera position x.", 1, camera_axis},
        {"camera-x", "Read camera position x.", 1, camera_axis},
        {"obs-camera-y", "Read camera position y.", 1, camera_axis},
        {"camera-y", "Read camera position y.", 1, camera_axis},
        {"obs-camera-z", "Read camera position z.", 1, camera_axis},
        {"camera-z", "Read camera position z.", 1, camera_axis},
        {"obs-camera-speed", "Read camera movement speed.", 1, camera_speed},
        {"camera-speed", "Read camera movement speed.", 1, camera_speed},
        {"obs-camera-set-position", "Set camera position at an engine safe point.", 4, camera_set_position},
        {"camera-position!", "Set camera position at an engine safe point.", 4, camera_set_position},
        {"obs-camera-set-speed", "Set camera movement speed.", 2, camera_set_speed},
        {"camera-speed!", "Set camera movement speed.", 2, camera_set_speed},
        {"obs-bind-key", "Bind a compiled Monad command to a key chord.", 2, bind_key},
        {"bind-key!", "Bind a compiled Monad command to a key chord.", 2, bind_key},
        {"obs-unbind-key", "Remove a key chord owned by Monad.", 1, unbind_key},
        {"unbind-key!", "Remove a key chord owned by Monad.", 1, unbind_key},
        {"obs-monad-request-reload", "Queue boot module reload after event dispatch.", 0, request_reload},
        {"reload!", "Queue boot module reload after event dispatch.", 0, request_reload},
        {"obs-monad-reload-state", "0=never, 1=published, 2=failed.", 0, reload_state},
        {"reload-state", "0=never, 1=published, 2=failed.", 0, reload_state},
    };
    for (size_t i = 0; i < sizeof(bindings)/sizeof(bindings[0]); ++i) {
        monad_dynamic_procedure_descriptor_t desc;
        monad_dynamic_procedure_descriptor_init(&desc);
        desc.name = bindings[i].name;
        desc.docstring = bindings[i].doc;
        desc.minimum_arity = (size_t)bindings[i].arity;
        desc.maximum_arity = (size_t)bindings[i].arity;
        desc.function = bindings[i].function;
        desc.context = host;
        if (bindings[i].function == camera_axis) {
            /* Both old and short spellings must use the same coordinate.
             * Never infer axes from registration order. */
            const char *name = bindings[i].name;
            int axis = strstr(name, "camera-y") ? 1 :
                       strstr(name, "camera-z") ? 2 : 0;
            desc.context = &axes[axis];
        }
        monad_dynamic_binding_t *binding = NULL;
        if (monad_dynamic_define_procedure(host->space, &desc,
                                           &binding, error) != MONAD_OK)
            return false;
    }
    if (!obs_monad_register_scene_api(host, error) ||
        !obs_monad_register_input_api(host, error) ||
        !obs_monad_register_camera_api(host, error) ||
        !obs_monad_register_mesh_api(host, error) ||
        !obs_monad_register_mesh_views_api(host, error) ||
        !obs_monad_register_sdf_api(host, error) ||
        !obs_monad_register_sculpt_api(host, error) ||
        !obs_monad_register_audio_api(host, error) ||
        !obs_monad_register_render_api(host, error) ||
        !obs_monad_register_asset_api(host, error) ||
        !obs_monad_register_gltf_node_api(host, error) ||
        !obs_monad_register_material_asset_api(host, error) ||
        !obs_monad_register_hotreload_api(host, error) ||
        !obs_monad_register_frame_api(host, error) ||
        !obs_monad_register_window_api(host, error) ||
        !obs_monad_register_effect_api(host, error) ||
        !obs_monad_register_editor_api(host, error)) return false;
    register_call_interactively_named(dispatch_monad_command);
    return true;
}
