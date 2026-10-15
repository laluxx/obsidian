#include "editor_api.h"

#include "editor.h"
#include "foreign.h"
#include "scene.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

#define EDITOR_REF "obsidian.editor"
#define SELECTION_REF "obsidian.editor-selection"

static bool editor_ref(monad_value_t *value, ObsMonadRuntime *host,
                       monad_error_t *error) {
    return obs_host_require(value, host, EDITOR_REF, &editor,
                            host ? host->generation : 0, error);
}

static bool selection_ref(monad_value_t *value, ObsMonadRuntime *host,
                          monad_error_t *error) {
    return obs_host_require(value, host, SELECTION_REF, &editor.inspector,
                            host ? host->generation : 0, error);
}

static bool mutation_allowed(monad_error_t *error) {
    return obs_monad_runtime_mutation_allowed() ||
           (obs_host_invalid(error,
               "editor writes are forbidden during Monad publication"),
            false);
}

static monad_status_t current_editor(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    (void)args;
    ObsMonadRuntime *host = context;
    if (argc || !out || !obs_host_owner(host) || !editor.initialized)
        return obs_host_invalid(error,
                                "editor is unavailable outside its owner thread");
    return obs_host_borrow(space, host, EDITOR_REF, &editor,
                           host->generation, out, error)
        ? MONAD_OK : error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
}

static monad_status_t editor_selection(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    ObsMonadRuntime *host = context;
    if (argc != 1 || !args || !out ||
        !editor_ref(args[0], host, error))
        return obs_host_invalid(error, "editor-selection expects a live editor");
    return obs_host_borrow(space, host, SELECTION_REF, &editor.inspector,
                           host->generation, out, error)
        ? MONAD_OK : error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
}

static monad_status_t selection_mesh_index(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (argc != 1 || !args || !out ||
        !selection_ref(args[0], context, error))
        return obs_host_invalid(error,
                                "selection-mesh-index expects a live selection");
    return monad_value_make_i64(
        space, editor.inspector.selected_mesh_index, out, error);
}

static monad_status_t selection_set_mesh(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    int64_t index = -1;
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !selection_ref(args[0], context, error) ||
        monad_value_as_i64(args[1], &index, error) != MONAD_OK ||
        index < -1 || index > INT_MAX ||
        (index >= 0 && (uint64_t)index >= scene->meshes.count))
        return obs_host_invalid(error,
                                "selection-mesh! expects -1 or a live mesh index");
    if (index < 0) inspector_deselect();
    else inspector_select_mesh((int)index);
    *out = monad_value_bool(space, true);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t selection_node_count(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (argc != 1 || !args || !out ||
        !selection_ref(args[0], context, error) ||
        scene->tree.count < 0 || scene->tree.count > SCENE_TREE_MAX_NODES)
        return obs_host_invalid(error,
                                "selection-node-count expects a valid selection");
    int64_t count = 0;
    for (int32_t i = 0; i < scene->tree.count; ++i)
        count += scene->tree.nodes[i].selected;
    return monad_value_make_i64(space, count, out, error);
}

static monad_status_t selection_node_index(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    int64_t ordinal = -1;
    if (argc != 2 || !args || !out ||
        !selection_ref(args[0], context, error) ||
        monad_value_as_i64(args[1], &ordinal, error) != MONAD_OK ||
        ordinal < 0 || scene->tree.count < 0 ||
        scene->tree.count > SCENE_TREE_MAX_NODES)
        return obs_host_invalid(error,
                                "selection-node-index expects a nonnegative selected-node ordinal");
    for (int32_t i = 0; i < scene->tree.count; ++i) {
        if (!scene->tree.nodes[i].selected) continue;
        if (ordinal-- == 0)
            return monad_value_make_i64(space, i, out, error);
    }
    return obs_host_invalid(error, "selected-node ordinal is out of range");
}

static monad_status_t transform_begin(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    int64_t mode = -1;
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !editor_ref(args[0], context, error) ||
        monad_value_as_i64(args[1], &mode, error) != MONAD_OK ||
        mode < EDITOR_TRANSFORM_TRANSLATE ||
        mode > EDITOR_TRANSFORM_ROTATE)
        return obs_host_invalid(error,
                                "editor-transform-begin! expects a live editor and mode 0 or 1");
    *out = monad_value_bool(space,
        editor_transform_begin((EditorTransformMode)mode));
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_mode(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    int64_t mode = -1;
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !editor_ref(args[0], context, error) ||
        monad_value_as_i64(args[1], &mode, error) != MONAD_OK ||
        mode < EDITOR_TRANSFORM_TRANSLATE ||
        mode > EDITOR_TRANSFORM_ROTATE)
        return obs_host_invalid(error,
                                "editor-transform-mode! expects mode 0 or 1");
    *out = monad_value_bool(space,
        editor_transform_set_mode((EditorTransformMode)mode));
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_axis(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    int64_t axis = -1;
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !editor_ref(args[0], context, error) ||
        monad_value_as_i64(args[1], &axis, error) != MONAD_OK ||
        axis < EDITOR_TRANSFORM_AXIS_VIEW ||
        axis > EDITOR_TRANSFORM_AXIS_Z)
        return obs_host_invalid(error,
                                "editor-transform-axis! expects axis 0 through 3");
    *out = monad_value_bool(space,
        editor_transform_set_axis((EditorTransformAxis)axis));
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_snap(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    double step = 0.0;
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !editor_ref(args[0], context, error) ||
        monad_value_as_f64(args[1], &step, error) != MONAD_OK ||
        !isfinite(step) || step < 0.0 || step > 1.0e6)
        return obs_host_invalid(error,
                                "editor-transform-snap! expects a finite step in [0, 1000000]");
    *out = monad_value_bool(space,
        editor_transform_set_snap((float)step));
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_origin(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 1 || !args || !out ||
        !editor_ref(args[0], context, error))
        return obs_host_invalid(error,
                                "editor-transform-origin! expects a live editor");
    *out = monad_value_bool(space, editor_transform_reset_origin());
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_finish(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (!mutation_allowed(error))
        return error ? error->status : MONAD_ERROR_INVALID_ARGUMENT;
    if (argc != 2 || !args || !out ||
        !editor_ref(args[0], context, error) ||
        monad_value_kind(args[1]) != MONAD_VALUE_BOOL)
        return obs_host_invalid(error,
                                "editor-transform-finish! expects an editor and Boolean");
    *out = monad_value_bool(space,
        editor_transform_finish(monad_value_is_true(args[1])));
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t transform_active(monad_object_space_t *space,
    size_t argc, monad_value_t *const *args, monad_value_t **out,
    void *context, monad_error_t *error) {
    if (argc != 1 || !args || !out ||
        !editor_ref(args[0], context, error))
        return obs_host_invalid(error,
                                "editor-transform-active? expects a live editor");
    *out = monad_value_bool(space, editor_transform_active());
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

bool obs_monad_register_editor_api(ObsMonadRuntime *host,
                                   monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"editor", "Borrow the live editor capability.", 0, current_editor, NULL},
        {"obs-current-editor", "Borrow the live editor capability.", 0, current_editor, NULL},
        {"editor-selection", "Borrow the editor's live selection view.", 1, editor_selection, NULL},
        {"selection-mesh-index", "Active inspector mesh index, or -1.", 1, selection_mesh_index, NULL},
        {"selection-mesh!", "Select a mesh by index; -1 clears selection.", 2, selection_set_mesh, NULL},
        {"selection-node-count", "Number of selected hierarchy nodes.", 1, selection_node_count, NULL},
        {"selection-node-index", "Hierarchy node index at a selected-node ordinal.", 2, selection_node_index, NULL},
        {"editor-transform-begin!", "Begin translate (0) or rotate (1) on the current selection.", 2, transform_begin, NULL},
        {"editor-transform-mode!", "Set active translate (0) or rotate (1) mode.", 2, transform_mode, NULL},
        {"editor-transform-axis!", "Constrain the active transform: view=0, X=1, Y=2, Z=3.", 2, transform_axis, NULL},
        {"editor-transform-snap!", "Set translation grid spacing in meters; zero disables snapping.", 2, transform_snap, NULL},
        {"editor-transform-origin!", "Commit a translation that moves the active object's origin to world zero.", 1, transform_origin, NULL},
        {"editor-transform-finish!", "Commit (True) or cancel (False) the active transform.", 2, transform_finish, NULL},
        {"editor-transform-active?", "Whether an editor transform transaction is active.", 1, transform_active, NULL},
    };
    return obs_host_register(host, entries,
        sizeof(entries) / sizeof(entries[0]), error);
}
