#include "sdf_property_bridge.h"
#include "sdf_sculpt.h"

ObsSdfEditStatus obs_sdf_property_apply(uint32_t index, uint32_t mask,
                                        const ObsSdfEditValues *values) {
    const ObsSdfEditStatus valid = obs_sdf_inspector_validate_edit(mask, values);
    if (valid != OBS_SDF_EDIT_OK) return valid;
    /* A visible editor panel must never join another owner's uncommitted
     * viewport gesture, steal its selection or merge its undo command. */
    if (obs_sdf_inspector_group_active()) return OBS_SDF_EDIT_CONFLICT;
    const uint64_t ticket = obs_sculpt_ticket_any_at(index);
    if (!ticket) return OBS_SDF_EDIT_STALE;
    uint64_t current = 0;
    if (obs_sdf_inspector_selected(&current) != OBS_SDF_EDIT_OK)
        current = 0; /* stale old selection must not prevent a new selection */
    if (current != ticket) {
        ObsSdfEditStatus st = obs_sdf_inspector_select(ticket);
        if (st != OBS_SDF_EDIT_OK) return st;
    }
    return obs_sdf_inspector_edit(mask, values);
}

/* The editor has one owner-thread active property gesture. Do not infer
 * ownership from journal.group_active alone (the viewport uses it too). */
static uint64_t panel_owner, panel_group;
static uint32_t panel_index;

bool obs_sdf_property_active(void) {
    if (!panel_owner) return false;
    if (obs_sdf_inspector_group_token() != panel_group ||
        obs_sculpt_ticket_any_at(panel_index) != panel_owner) {
        panel_owner = panel_group = 0;
        return false;
    }
    return true;
}

ObsSdfEditStatus obs_sdf_property_begin(uint32_t index) {
    if (obs_sdf_property_active() || obs_sdf_inspector_group_active())
        return OBS_SDF_EDIT_CONFLICT;
    const uint64_t ticket = obs_sculpt_ticket_any_at(index);
    if (!ticket) return OBS_SDF_EDIT_STALE;
    ObsSdfEditStatus st = obs_sdf_inspector_select(ticket);
    if (st != OBS_SDF_EDIT_OK) return st;
    st = obs_sdf_inspector_begin_group();
    if (st == OBS_SDF_EDIT_OK) {
        panel_owner = ticket;
        panel_index = index;
        panel_group = obs_sdf_inspector_group_token();
    }
    return st;
}

ObsSdfEditStatus obs_sdf_property_preview(uint32_t index, uint32_t mask,
                                          const ObsSdfEditValues *values) {
    ObsSdfEditStatus st = obs_sdf_inspector_validate_edit(mask, values);
    if (st != OBS_SDF_EDIT_OK) return st;
    if (!obs_sdf_property_active()) return OBS_SDF_EDIT_INVALID;
    const uint64_t ticket = obs_sculpt_ticket_any_at(index);
    if (!ticket || ticket != panel_owner) return OBS_SDF_EDIT_STALE;
    uint64_t selected = 0;
    if (obs_sdf_inspector_selected(&selected) != OBS_SDF_EDIT_OK ||
        selected != panel_owner) return OBS_SDF_EDIT_CONFLICT;
    return obs_sdf_inspector_preview_edit(mask, values);
}

ObsSdfEditStatus obs_sdf_property_commit(void) {
    if (!obs_sdf_property_active()) return OBS_SDF_EDIT_INVALID;
    ObsSdfEditStatus st = obs_sdf_inspector_end_group();
    if (st == OBS_SDF_EDIT_OK) panel_owner = panel_group = 0;
    return st;
}

ObsSdfEditStatus obs_sdf_property_cancel(void) {
    if (!obs_sdf_property_active()) return OBS_SDF_EDIT_INVALID;
    ObsSdfEditStatus st = obs_sdf_inspector_abort_group();
    if (st == OBS_SDF_EDIT_OK) panel_owner = panel_group = 0;
    return st;
}
