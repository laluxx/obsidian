/// sculpt_api — compiled Monad commands into production SDF scene
#ifdef OBS_MONAD_SCULPT_TEST
#include "sculpt_api_contract.h"
#else
#include "sculpt_api.h"
#include "foreign.h"
#include "effect_policy.h"
#endif
#include "sdf_sculpt.h"
#include "sdf_inspector.h"
#include "sdf_tokens.h"
#include "sdf_appearance_clipboard.h"
#include <stdint.h>
#include <math.h>
#include <float.h>
static bool admitted(void *ctx) {
    return obs_host_owner(ctx) &&
        obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                 OBS_LIFETIME_ENGINE, false);
}
static monad_status_t sphere(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    double v[7];
    if (n != 7 || !a || !out || !admitted(ctx))
        return obs_host_invalid(e, "sdf-sphere!: requires admitted engine mutation");
    for (unsigned i = 0; i < 7; ++i)
        if (monad_value_as_f64(a[i], &v[i], e) != MONAD_OK)
            return obs_host_invalid(e, "sdf-sphere!: expected seven real numbers");
    uint64_t id = 0;
    if (!obs_sculpt_sphere(v, v[3], v + 4, &id))
        return obs_host_invalid(e, "sdf-sphere!: nonfinite values, resource limit or scene failure");
    return monad_value_make_i64(sp, (int64_t)id, out, e);
}
static monad_status_t ticket_at(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t index;
    if (n != 1 || !a || !out || !obs_host_owner(ctx) ||
        monad_value_as_i64(a[0], &index, e) != MONAD_OK ||
        index < 0 || index >= OBS_SDF_TICKET_LIMIT)
        return obs_host_invalid(e, "sdf-ticket: expected live Monad sphere index");
    uint64_t id = obs_sculpt_ticket_at((uint32_t)index);
    if (!id) return obs_host_invalid(e, "sdf-ticket: not a live Monad sphere");
    return monad_value_make_i64(sp, (int64_t)id, out, e);
}
static monad_status_t move(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t id;
    double v[3];
    if (n != 4 || !a || !out || !admitted(ctx) ||
        monad_value_as_i64(a[0], &id, e) != MONAD_OK || id <= 0)
        return obs_host_invalid(e, "sdf-move!: expected live sphere ticket");
    for (unsigned i = 0; i < 3; ++i)
        if (monad_value_as_f64(a[i + 1], &v[i], e) != MONAD_OK)
            return obs_host_invalid(e, "sdf-move!: expected three real coordinates");
    if (!obs_sculpt_move((uint64_t)id, v))
        return obs_host_invalid(e, "sdf-move!: invalid coordinates or stale ticket");
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t tint(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t id;
    double rgb[3];
    if (n != 4 || !a || !out || !admitted(ctx) ||
        monad_value_as_i64(a[0], &id, e) != MONAD_OK || id <= 0)
        return obs_host_invalid(e, "sdf-tint!: expected live sphere ticket");
    for (unsigned i = 0; i < 3; ++i)
        if (monad_value_as_f64(a[i + 1], &rgb[i], e) != MONAD_OK)
            return obs_host_invalid(e, "sdf-tint!: expected three real color factors");
    if (!obs_sculpt_tint((uint64_t)id, rgb))
        return obs_host_invalid(e, "sdf-tint!: invalid color or stale ticket");
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t x_pos(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t id;
    double x;
    if (n != 1 || !a || !out || !obs_host_owner(ctx) ||
        monad_value_as_i64(a[0], &id, e) != MONAD_OK || id <= 0 ||
        !obs_sculpt_x((uint64_t)id, &x))
        return obs_host_invalid(e, "sdf-x: expired or invalid sphere ticket");
    return monad_value_make_f64(sp, x, out, e);
}
/* Read-only picking. Return a shared Int64 sphere ticket or 0 for a miss.
 * The ray is expressed in engine world units, independent of the camera. */
static monad_status_t raycast(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    double v[7];
    if (n != 7 || !a || !out || !obs_host_owner(ctx))
        return obs_host_invalid(e, "sdf-raycast: requires seven real numbers");
    for (unsigned i = 0; i < 7; ++i)
        if (monad_value_as_f64(a[i], &v[i], e) != MONAD_OK || !isfinite(v[i]))
            return obs_host_invalid(e, "sdf-raycast: expected finite real numbers");
    const double magnitude = hypot(hypot(v[3], v[4]), v[5]);
    if (magnitude <= 0 || !isfinite(magnitude) || v[6] <= 0 ||
        v[6] > (double)FLT_MAX)
        return obs_host_invalid(e, "sdf-raycast: invalid direction or range");
    for (unsigned i = 0; i < 6; ++i)
        if (fabs(v[i]) >= (double)FLT_MAX)
            return obs_host_invalid(e, "sdf-raycast: world coordinate overflow");
    ObsSculptRayHit hit;
    const bool found = obs_sculpt_ray_cast(v, v + 3, v[6], &hit);
    return monad_value_make_i64(sp, found ? (int64_t)hit.ticket : 0, out, e);
}
/* Persistent editor selection is a checked engine-owned ticket; numeric
 * commands cannot bypass the host owner/effect admission already in place. */
static monad_status_t inspect_select(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t id;
    if (n != 1 || !a || !out || !admitted(ctx) ||
        monad_value_as_i64(a[0], &id, e) != MONAD_OK || id < 0 ||
        obs_sdf_inspector_select((uint64_t)id) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-select!: invalid or expired ticket");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_selected(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    uint64_t id = 0;
    if (n != 0 || !out || !obs_host_owner(ctx) ||
        obs_sdf_inspector_selected(&id) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-selected: selection expired");
    return monad_value_make_i64(sp, (int64_t)id, out, e);
}
static monad_status_t inspect_move(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    double xyz[3];
    if (n != 3 || !a || !out || !admitted(ctx))
        return obs_host_invalid(e, "sdf-inspect-move!: denied or invalid arguments");
    for (unsigned i = 0; i < 3; ++i)
        if (monad_value_as_f64(a[i], &xyz[i], e) != MONAD_OK)
            return obs_host_invalid(e, "sdf-inspect-move!: requires XYZ");
    if (obs_sdf_inspector_move(xyz) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-move!: invalid or stale selection");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_tint(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    double rgb[3];
    if (n != 3 || !a || !out || !admitted(ctx))
        return obs_host_invalid(e, "sdf-inspect-tint!: denied or invalid arguments");
    for (unsigned i = 0; i < 3; ++i)
        if (monad_value_as_f64(a[i], &rgb[i], e) != MONAD_OK)
            return obs_host_invalid(e, "sdf-inspect-tint!: requires RGB");
    if (obs_sdf_inspector_tint(rgb) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-tint!: invalid or stale selection");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_undo(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_undo() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-undo!: empty, stale or conflicting edit");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_undo_selected(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_undo_selected() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-undo-selected!: no safe selected edit");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_redo(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_redo() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-redo!: empty, stale or conflicting edit");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_undo_only(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_undo_only() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-undo-only!: empty, stale or conflicting edit");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_boundary(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !obs_host_owner(ctx))
        return obs_host_invalid(e, "sdf-inspect-undo-boundary!: owner or arity error");
    obs_sdf_inspector_command_boundary();
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_begin_group(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_begin_group() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-begin-group!: nested/denied");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_end_group(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_end_group() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-end-group!: no active group/denied");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_abort_group(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !admitted(ctx) ||
        obs_sdf_inspector_abort_group() != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-inspect-abort-group!: denied, inactive or conflicting preview");
    *out = monad_value_nil(sp); return MONAD_OK;
}
static monad_status_t inspect_undo_count(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !obs_host_owner(ctx))
        return obs_host_invalid(e, "sdf-inspect-undo-count: wrong owner or arity");
    const ObsSdfInspectorState state = obs_sdf_inspector_state();
    return monad_value_make_i64(sp, state.undo_count, out, e);
}
static monad_status_t inspect_redo_count(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    (void)a;
    if (n != 0 || !out || !obs_host_owner(ctx))
        return obs_host_invalid(e, "sdf-inspect-redo-count: wrong owner or arity");
    const ObsSdfInspectorState state = obs_sdf_inspector_state();
    return monad_value_make_i64(sp, state.redo_count, out, e);
}
static monad_status_t appearance_copy(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t index = -1;
    if (n != 1 || !a || !out || !admitted(ctx) ||
        monad_value_as_i64(a[0], &index, e) != MONAD_OK || index < 0 ||
        index >= OBS_SDF_TICKET_LIMIT ||
        obs_sdf_appearance_copy((uint32_t)index) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-appearance-copy!: invalid or speculative source");
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
static monad_status_t appearance_paste(monad_object_space_t *sp, size_t n,
    monad_value_t *const *a, monad_value_t **out, void *ctx, monad_error_t *e) {
    int64_t index = -1;
    if (n != 1 || !a || !out || !admitted(ctx) ||
        monad_value_as_i64(a[0], &index, e) != MONAD_OK || index < 0 ||
        index >= OBS_SDF_TICKET_LIMIT ||
        obs_sdf_appearance_paste((uint32_t)index) != OBS_SDF_EDIT_OK)
        return obs_host_invalid(e, "sdf-appearance-paste!: missing sample, stale object or active edit");
    *out = monad_value_nil(sp);
    return MONAD_OK;
}
bool obs_monad_register_sculpt_api(ObsMonadRuntime *host, monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"sdf-sphere!", "Create a renderer-backed sphere; return nonreused scene ticket.", 7, sphere, NULL},
        {"sdf-ticket", "Return a shared scene-epoch sphere ticket by index.", 1, ticket_at, NULL},
        {"sdf-move!", "Move a sphere by ticket, checking finite coordinates.", 4, move, NULL},
        {"sdf-tint!", "Change RGB of a sphere by ticket.", 4, tint, NULL},
        {"sdf-x", "Read sphere world X by ticket.", 1, x_pos, NULL},
        {"sdf-raycast", "Pick nearest translation-only primitive sphere; 0 on miss.", 7, raycast, NULL},
        {"sdf-inspect-select!", "Select a live shared sphere ticket (0 clears).", 1, inspect_select, NULL},
        {"sdf-inspect-selected", "Return selected shared sphere ticket (0 if none).", 0, inspect_selected, NULL},
        {"sdf-inspect-move!", "Edit selected sphere XYZ with bounded undo history.", 3, inspect_move, NULL},
        {"sdf-inspect-tint!", "Edit selected sphere RGB, preserve alpha/PBR.", 3, inspect_tint, NULL},
        {"sdf-inspect-undo!", "Emacs undo: consecutive older edits; after boundary undo an undo.", 0, inspect_undo, NULL},
        {"sdf-inspect-undo-selected!", "Undo selected object's latest original command, atomically.", 0, inspect_undo_selected, NULL},
        {"sdf-inspect-redo!", "Emacs undo-redo: consume a recent undo-generated record.", 0, inspect_redo, NULL},
        {"sdf-inspect-undo-only!", "Emacs undo-only: skip redo records.", 0, inspect_undo_only, NULL},
        {"sdf-inspect-undo-boundary!", "Interrupt an undo sequence without editing.", 0, inspect_boundary, NULL},
        {"sdf-inspect-begin-group!", "Amalgamate inspector edits as one undo command.", 0, inspect_begin_group, NULL},
        {"sdf-inspect-end-group!", "Commit live preview as one undoable command.", 0, inspect_end_group, NULL},
        {"sdf-inspect-abort-group!", "Cancel preview and restore previous scene state.", 0, inspect_abort_group, NULL},
        {"sdf-inspect-undo-count", "Number of reversible edits.", 0, inspect_undo_count, NULL},
        {"sdf-inspect-redo-count", "Number of redo operations available.", 0, inspect_redo_count, NULL},
        {"sdf-appearance-copy!", "Sample material and emission from any SDF index into the editor clipboard.", 1, appearance_copy, NULL},
        {"sdf-appearance-paste!", "Paste sampled SDF appearance as one undoable scene edit.", 1, appearance_paste, NULL}
    };
    return obs_host_register(host, entries, sizeof(entries)/sizeof(entries[0]), error);
}
