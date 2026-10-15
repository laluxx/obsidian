/// sdf_inspector — bounded Emacs-style self-inverting scene undo log
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define OBS_SDF_EDIT_HISTORY_CAP 64u
#define OBS_SDF_EDIT_POSITION 1u
#define OBS_SDF_EDIT_MATERIAL 2u
#define OBS_SDF_EDIT_SHAPE 4u
#define OBS_SDF_EDIT_EMISSION 8u
#define OBS_SDF_EDIT_BOX 16u
#define OBS_SDF_EDIT_TERRAIN 32u
#define OBS_SDF_EDIT_CSG 64u

typedef enum {
    OBS_SDF_EDIT_OK = 0,
    OBS_SDF_EDIT_INVALID,
    OBS_SDF_EDIT_STALE,
    OBS_SDF_EDIT_EMPTY,
    OBS_SDF_EDIT_CONFLICT,
    OBS_SDF_EDIT_FULL
} ObsSdfEditStatus;
typedef struct {
    double xyz[3];
    double rgba[4];
    double roughness, metallic;
    double radius, smoothness;
    double box_extents[3];
    double emissive[3], emissive_strength;
    int operation;
    double terrain_scale, terrain_gain, terrain_lacunarity, terrain_range;
    int terrain_octaves;
} ObsSdfEditValues;
typedef struct {
    uint64_t selection;
    uint32_t undo_count, redo_count;
} ObsSdfInspectorState;

/* The caller admits engine owner, publication and GPU effects. Neither an
 * independent renderer nor a second capability namespace is created here. */
ObsSdfEditStatus obs_sdf_inspector_select(uint64_t ticket);
ObsSdfEditStatus obs_sdf_inspector_selected(uint64_t *out);
/* Pure numeric preflight shared with editor boundary; no selection effects. */
ObsSdfEditStatus obs_sdf_inspector_validate_edit(uint32_t mask,
                                                const ObsSdfEditValues *values);
ObsSdfEditStatus obs_sdf_inspector_validate_target(uint64_t ticket,
                                                  uint32_t mask);
ObsSdfEditStatus obs_sdf_inspector_read_appearance(uint64_t ticket,
                                                  ObsSdfEditValues *out);
ObsSdfEditStatus obs_sdf_inspector_edit(uint32_t mask,
                                       const ObsSdfEditValues *values);
/* Coalesce successive property-panel samples into one staged record. */
ObsSdfEditStatus obs_sdf_inspector_preview_edit(uint32_t mask,
                                               const ObsSdfEditValues *values);
/* Chronologically invert the next applicable whole command group. Consecutive
 * calls traverse older groups; after a command boundary, undo can invert a
 * prior inverse (undo-of-undo). Returns INVALID while a preview group is open,
 * EMPTY when no candidate remains, or STALE/CONFLICT without partial publish. */
ObsSdfEditStatus obs_sdf_inspector_undo(void);
/* Like undo, but skip undo-generated inverse groups and inactive originals.
 * A successful call still records the inverse of the chosen original group;
 * use redo to explicitly consume a recent undo-generated group. */
ObsSdfEditStatus obs_sdf_inspector_undo_only(void);
/* Undo the newest active original command touching the selected object.
 * A multi-object command remains atomic; unrelated newer edits are preserved.
 * A newer active original command touching any member of that group is a
 * dependency and blocks the inversion, even if intervening edits returned the
 * primitive to the earlier numeric postimage. Already-undone newer commands
 * do not block it. Conflicts leave scene and journal unchanged. */
ObsSdfEditStatus obs_sdf_inspector_undo_selected(void);
/* End a consecutive undo sequence. In the current scene epoch this only
 * changes the undo cursor; it never commits/cancels a preview or edits scene
 * values. Epoch synchronization may discard stale history first. */
void obs_sdf_inspector_command_boundary(void);
/* Live preview transaction (similar in spirit to Emacs atomic-change-group):
 * begin opens a speculative gesture; edits update the real scene but are not
 * appended to undo history until end commits them as one atomic command.
 * Commit keeps each object's first preimage/final postimage and filters
 * net-zero changes. Every later grouped write must match that object's last
 * staged postimage; outside mutations fail closed. Abort rolls previews back
 * with no history loss, or rejects a conflict. Nesting is unsupported.
 * 64 successful raw preview edits max. */
bool obs_sdf_inspector_group_active(void);
/* Monotone, scene-scoped group identity. */
uint64_t obs_sdf_inspector_group_token(void);
ObsSdfEditStatus obs_sdf_inspector_begin_group(void);
ObsSdfEditStatus obs_sdf_inspector_end_group(void);
/* Live previews remain outside undo history until commit. Cancel undoes the
 * entire group atomically, but cannot override a conflicting outside write. */
ObsSdfEditStatus obs_sdf_inspector_abort_group(void);
/* Consume the newest undo-generated group after whole-group validation.
 * Returns INVALID during a preview, EMPTY without a recent inverse, or
 * STALE/CONFLICT without partial publish. If the bounded log evicted every
 * original root, redo reifies a fresh original group so it remains undoable. */
ObsSdfEditStatus obs_sdf_inspector_redo(void);
/* Return current selection and retained-history counts. Counts are advisory:
 * scene reset/eviction may expire records, and later out-of-band writes can
 * make replay fail. A stale selection is cleared during this query. */
ObsSdfInspectorState obs_sdf_inspector_state(void);
/* Drop retained undo/redo records and break the undo sequence. Does not edit
 * scene values; preserves a live selection. Epoch synchronization may clear a
 * stale selection. Refuses while a preview group is open. */
ObsSdfEditStatus obs_sdf_inspector_clear(void);
/* Convenience edit commands preserve the unmodified material channels. */
ObsSdfEditStatus obs_sdf_inspector_move(const double xyz[3]);
ObsSdfEditStatus obs_sdf_inspector_tint(const double rgb[3]);

/* Pointer preview is an absolute-value update with a single coalesced journal
 * record: 1000 pointer samples remain exactly one undoable change. Requires
 * an open, otherwise empty gesture or one previously coalesced drag entry. */
ObsSdfEditStatus obs_sdf_inspector_drag_move(const double xyz[3]);
