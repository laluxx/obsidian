/// sdf_inspector — Emacs-style bounded undo-log of reversible scene edits
#ifdef OBS_SDF_TEST
#include "agent_engine_contract.h"
#else
#include "scene.h"
#include "sdf.h"
#endif
#include "sdf_inspector.h"
#include "sdf_sculpt.h"
#include <float.h>
#include <math.h>
#include <string.h>

typedef struct {
  float pos[3], rgba[4], roughness, metallic, radius;
  float smoothness, emissive[3], emissive_strength;
  float dimensions[4];
  int operation, octaves, type;
} SdfState;
typedef struct {
  uint64_t ticket, group, serial, root_serial;
  SdfState before, after;
  bool undo_generated, active, preview_drag, preview_panel;
} SdfCommand;
typedef struct {
  uint64_t epoch, selected, next_group, open_group, next_serial;
  uint32_t count, staged_count;
  SdfCommand staged[OBS_SDF_EDIT_HISTORY_CAP]; /* uncommitted, live previews */
  int32_t pending;    /* next historical entry in an undo sequence */
  bool undo_sequence; /* consecutive undo commands, not a redo cursor */
  SdfCommand commands[OBS_SDF_EDIT_HISTORY_CAP];
} SdfJournal;
static SdfJournal journal;

static void sync_epoch(void) {
  if (journal.epoch != scene->sdf_lifetime_epoch) {
    memset(&journal, 0, sizeof(journal));
    journal.epoch = scene->sdf_lifetime_epoch;
    journal.pending = -1;
  }
}
static void break_sequence(void) {
  journal.undo_sequence = false;
  journal.pending = -1;
}
static bool factor(double v) { return isfinite(v) && v >= 0 && v <= 1; }
static bool coordinate(double v) {
  return isfinite(v) && v > -(double)FLT_MAX && v < (double)FLT_MAX;
}
static bool snapshot(uint64_t ticket, SdfPrimitive **prim, SdfState *out) {
  uint32_t index;
  if (!obs_sculpt_index_any(ticket, &index))
    return false;
  SdfPrimitive *s = &scene->sdfs[index];
  if ((s->type != SDF_TYPE_SPHERE && s->type != SDF_TYPE_BOX &&
       s->type != SDF_TYPE_TERRAIN) || !(s->size[0] > 0) ||
      !isfinite(s->size[0]))
    return false;
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 4; ++row)
      if (!(col == 3 && row < 3) &&
          s->inverseTransform[col][row] != (float)(col == row))
        return false;
  SdfState v = {.roughness = s->roughness, .metallic = s->metallic,
                .radius = s->type == SDF_TYPE_SPHERE ? s->size[0] : 0,
                .smoothness = s->smoothness,
                .emissive_strength = s->emissiveStrength,
                .operation = s->operation, .type = s->type,
                .octaves = s->_pad1};
  for (unsigned i = 0; i < 4; ++i) v.dimensions[i] = s->size[i];
  if (s->type == SDF_TYPE_SPHERE &&
      (v.radius > 1000 || v.radius <= 0)) return false;
  if (s->type == SDF_TYPE_BOX) {
    for (unsigned i = 0; i < 3; ++i) {
      if (!(s->size[i] > 0) || !isfinite(s->size[i]) || s->size[i] > 1000)
        return false;
    }
  }
  if (s->type == SDF_TYPE_TERRAIN &&
      (!isfinite(v.dimensions[0]) || v.dimensions[0] <= 0 ||
       v.dimensions[0] > 100 ||
       !isfinite(v.dimensions[1]) || v.dimensions[1] < -2 ||
       v.dimensions[1] > 2 ||
       !isfinite(v.dimensions[2]) || v.dimensions[2] < .1 ||
       v.dimensions[2] > 10 || !coordinate(v.dimensions[3]) ||
       v.octaves < 1 || v.octaves > 16)) return false;
  if (!factor(v.roughness) || !factor(v.metallic) ||
      !isfinite(v.smoothness) || v.smoothness < 0 || v.smoothness > 10 ||
      !isfinite(v.emissive_strength) || v.emissive_strength < 0 ||
      v.emissive_strength > 1e6f || v.operation < 0 || v.operation > 2)
    return false;
  for (unsigned i = 0; i < 3; ++i) {
    v.pos[i] = -s->inverseTransform[3][i];
    v.emissive[i] = s->emissive[i];
    if (!coordinate(v.pos[i]) || !factor(s->color[i]) ||
        !factor(v.emissive[i]))
      return false;
  }
  for (unsigned i = 0; i < 4; ++i) {
    v.rgba[i] = s->color[i];
    if (!factor(v.rgba[i]))
      return false;
  }
  *prim = s;
  *out = v;
  return true;
}
static bool equal(const SdfState *a, const SdfState *b) {
  for (unsigned i = 0; i < 3; ++i)
    if (a->pos[i] != b->pos[i])
      return false;
  for (unsigned i = 0; i < 4; ++i)
    if (a->rgba[i] != b->rgba[i])
      return false;
  for (unsigned i = 0; i < 3; ++i)
    if (a->emissive[i] != b->emissive[i])
      return false;
  return a->roughness == b->roughness && a->metallic == b->metallic &&
         a->radius == b->radius && a->smoothness == b->smoothness &&
         a->operation == b->operation && a->type == b->type &&
         a->octaves == b->octaves &&
         memcmp(a->dimensions, b->dimensions, sizeof(a->dimensions)) == 0 &&
         a->emissive_strength == b->emissive_strength;
}
static void publish(SdfPrimitive *s, const SdfState *v) {
  for (unsigned i = 0; i < 3; ++i)
    s->inverseTransform[3][i] = -v->pos[i];
  for (unsigned i = 0; i < 4; ++i)
    s->color[i] = v->rgba[i];
  s->roughness = v->roughness;
  s->metallic = v->metallic;
  for (unsigned i = 0; i < 4; ++i) s->size[i] = v->dimensions[i];
  s->_pad1 = v->octaves;
  s->smoothness = v->smoothness;
  s->operation = v->operation;
  for (unsigned i = 0; i < 3; ++i)
    s->emissive[i] = v->emissive[i];
  s->emissiveStrength = v->emissive_strength;
  flushSdfSSBO(); /* deferred until owning GPU flight-slot fence */
}
/* Oldest log entries may expire, like Emacs' bounded undo memory. Preserve
 * the saved undo-chain pointer when inverse commands extend the log. */
static void toggle_root(uint64_t root) {
  for (uint32_t i = 0; i < journal.count; ++i)
    if (!journal.commands[i].undo_generated &&
        journal.commands[i].serial == root) {
      journal.commands[i].active = !journal.commands[i].active;
      return;
    }
}
static void append(SdfCommand cmd) {
  /* Monotone identities are never recycled in a retained undo history. */
  if (journal.next_serial == UINT64_MAX) {
    journal.count = 0;
    journal.next_serial = 0;
    break_sequence();
  }
  cmd.serial = ++journal.next_serial;
  if (!cmd.undo_generated)
    cmd.root_serial = cmd.serial;
  if (journal.count == OBS_SDF_EDIT_HISTORY_CAP) {
    /* Never retain a truncated command group: doing so would make a
     * multi-object edit impossible to undo atomically. */
    uint32_t remove = 1;
    while (remove < journal.count &&
           journal.commands[remove].group == journal.commands[0].group)
      ++remove;
    const uint32_t left = journal.count - remove;
    memmove(journal.commands, journal.commands + remove,
            left * sizeof(SdfCommand));
    journal.count = left;
    if (journal.pending >= 0)
      journal.pending = journal.pending < (int32_t)remove
                            ? -1
                            : journal.pending - (int32_t)remove;
  }
  journal.commands[journal.count++] = cmd;
}
static uint64_t new_group(void) {
  if (journal.next_group == UINT64_MAX) {
    /* IDs must never alias groups retained in the log. */
    journal.count = 0;
    journal.next_group = 0;
    break_sequence();
  }
  return ++journal.next_group;
}

void obs_sdf_inspector_command_boundary(void) {
  sync_epoch();
  break_sequence();
}
/* A live command group is a transaction: preview edits are applied to the
 * renderer but staged outside the committed undo log. A failed/cancelled
 * gesture cannot evict old history or expose half a grouped command. */
bool obs_sdf_inspector_group_active(void) {
  sync_epoch();
  return journal.open_group != 0;
}
uint64_t obs_sdf_inspector_group_token(void) {
  sync_epoch();
  return journal.open_group;
}
ObsSdfEditStatus obs_sdf_inspector_begin_group(void) {
  sync_epoch();
  if (journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  break_sequence();
  journal.open_group = new_group();
  journal.staged_count = 0;
  return OBS_SDF_EDIT_OK;
}

/* Used by both commit and cancellation: check that the final scene still
 * contains every staged object in the precise previewed state. */
static ObsSdfEditStatus validate_preview(void) {
  for (uint32_t i = 0; i < journal.staged_count; ++i) {
    const SdfCommand *cmd = &journal.staged[i];
    bool later = false;
    for (uint32_t j = i + 1; j < journal.staged_count; ++j)
      if (journal.staged[j].ticket == cmd->ticket) {
        later = true;
        break;
      }
    if (later)
      continue;
    SdfPrimitive *prim;
    SdfState live;
    if (!snapshot(cmd->ticket, &prim, &live))
      return OBS_SDF_EDIT_STALE;
    if (!equal(&live, &cmd->after))
      return OBS_SDF_EDIT_CONFLICT;
  }
  return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_inspector_end_group(void) {
  sync_epoch();
  if (!journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  ObsSdfEditStatus result = validate_preview();
  if (result != OBS_SDF_EDIT_OK)
    return result;
  /* Commit the net change per object, not every intermediate preview sample.
   * Keep each ticket's first preimage and last postimage; unchanged objects
   * contribute no undo record. All staged steps remain available for live
   * rollback until this entire transaction has passed validate_preview(). */
  SdfCommand compact[OBS_SDF_EDIT_HISTORY_CAP];
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < journal.staged_count; ++i) {
    const SdfCommand *step = &journal.staged[i];
    uint32_t j = 0;
    while (j < distinct && compact[j].ticket != step->ticket)
      ++j;
    if (j == distinct)
      compact[distinct++] = *step;
    else
      compact[j].after = step->after;
  }
  for (uint32_t i = 0; i < distinct; ++i)
    if (!equal(&compact[i].before, &compact[i].after))
      append(compact[i]);
  journal.staged_count = 0;
  journal.open_group = 0;
  return OBS_SDF_EDIT_OK;
}

/* Escape/cancel gesture: validate every reverse transition without writing,
 * then roll back all previews newest-first. On a conflicting external edit,
 * leave the session open and the live scene untouched. */
ObsSdfEditStatus obs_sdf_inspector_abort_group(void) {
  sync_epoch();
  if (!journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  ObsSdfEditStatus result = validate_preview();
  if (result != OBS_SDF_EDIT_OK)
    return result;
  for (uint32_t i = journal.staged_count; i > 0; --i) {
    const SdfCommand *cmd = &journal.staged[i - 1];
    SdfPrimitive *prim;
    SdfState state;
    /* The entire preview was preflighted and this owner-thread loop
     * cannot be interrupted by another scene writer. */
    if (!snapshot(cmd->ticket, &prim, &state))
      return OBS_SDF_EDIT_STALE;
    publish(prim, &cmd->before);
  }
  journal.staged_count = 0;
  journal.open_group = 0;
  return OBS_SDF_EDIT_OK;
}

ObsSdfEditStatus obs_sdf_inspector_select(uint64_t ticket) {
  sync_epoch();
  if (ticket) {
    SdfPrimitive *prim;
    SdfState v;
    if (!snapshot(ticket, &prim, &v))
      return OBS_SDF_EDIT_STALE;
  }
  journal.selected = ticket;
  break_sequence(); /* selection is a non-undo editor command */
  return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_inspector_selected(uint64_t *out) {
  sync_epoch();
  if (!out)
    return OBS_SDF_EDIT_INVALID;
  if (journal.selected) {
    SdfPrimitive *prim;
    SdfState v;
    if (!snapshot(journal.selected, &prim, &v)) {
      journal.selected = 0;
      return OBS_SDF_EDIT_STALE;
    }
  }
  *out = journal.selected;
  return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_inspector_validate_edit(
    uint32_t mask, const ObsSdfEditValues *values) {
  if (!values || !mask ||
      (mask & ~(OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL |
                OBS_SDF_EDIT_SHAPE | OBS_SDF_EDIT_EMISSION |
                OBS_SDF_EDIT_BOX | OBS_SDF_EDIT_TERRAIN |
                OBS_SDF_EDIT_CSG)))
    return OBS_SDF_EDIT_INVALID;
  if ((mask & OBS_SDF_EDIT_POSITION) &&
      (!coordinate(values->xyz[0]) || !coordinate(values->xyz[1]) ||
       !coordinate(values->xyz[2])))
    return OBS_SDF_EDIT_INVALID;
  if (mask & OBS_SDF_EDIT_MATERIAL) {
    if (!factor(values->roughness) || !factor(values->metallic))
      return OBS_SDF_EDIT_INVALID;
    for (unsigned i = 0; i < 4; ++i)
      if (!factor(values->rgba[i]))
        return OBS_SDF_EDIT_INVALID;
  }
  if ((mask & OBS_SDF_EDIT_SHAPE) &&
      (!isfinite(values->radius) || values->radius <= 0 ||
       values->radius > 1000 || !isfinite(values->smoothness) ||
       values->smoothness < 0 || values->smoothness > 10 ||
       values->operation < 0 || values->operation > 2))
    return OBS_SDF_EDIT_INVALID;
  if (mask & OBS_SDF_EDIT_BOX)
    for (unsigned i = 0; i < 3; ++i)
      if (!isfinite(values->box_extents[i]) || values->box_extents[i] <= 0 ||
          values->box_extents[i] > 1000)
        return OBS_SDF_EDIT_INVALID;
  if ((mask & OBS_SDF_EDIT_TERRAIN) &&
      (!isfinite(values->terrain_scale) || values->terrain_scale <= 0 ||
       values->terrain_scale > 100 ||
       !isfinite(values->terrain_gain) || values->terrain_gain < -2 ||
       values->terrain_gain > 2 ||
       !isfinite(values->terrain_lacunarity) ||
       values->terrain_lacunarity < .1 ||
       values->terrain_lacunarity > 10 ||
       !coordinate(values->terrain_range) ||
       values->terrain_octaves < 1 || values->terrain_octaves > 16))
    return OBS_SDF_EDIT_INVALID;
  if ((mask & OBS_SDF_EDIT_CSG) &&
      (values->operation < 0 || values->operation > 2 ||
       !isfinite(values->smoothness) || values->smoothness < 0 ||
       values->smoothness > 10))
    return OBS_SDF_EDIT_INVALID;
  if (mask & OBS_SDF_EDIT_EMISSION) {
    if (!isfinite(values->emissive_strength) ||
        values->emissive_strength < 0 || values->emissive_strength > 1e6)
      return OBS_SDF_EDIT_INVALID;
    for (unsigned i = 0; i < 3; ++i)
      if (!factor(values->emissive[i]))
        return OBS_SDF_EDIT_INVALID;
  }
  return OBS_SDF_EDIT_OK;
}
static bool mask_is_type_compatible(uint32_t mask, int type) {
  return !(((mask & OBS_SDF_EDIT_SHAPE) && type != SDF_TYPE_SPHERE) ||
           ((mask & OBS_SDF_EDIT_BOX) && type != SDF_TYPE_BOX) ||
           ((mask & OBS_SDF_EDIT_TERRAIN) && type != SDF_TYPE_TERRAIN) ||
           ((mask & OBS_SDF_EDIT_BOX) && (mask & OBS_SDF_EDIT_TERRAIN)));
}
ObsSdfEditStatus obs_sdf_inspector_validate_target(uint64_t ticket,
                                                  uint32_t mask) {
  sync_epoch();
  const uint32_t known = OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL |
      OBS_SDF_EDIT_SHAPE | OBS_SDF_EDIT_EMISSION | OBS_SDF_EDIT_BOX |
      OBS_SDF_EDIT_TERRAIN | OBS_SDF_EDIT_CSG;
  if (!mask || (mask & ~known)) return OBS_SDF_EDIT_INVALID;
  SdfPrimitive *prim;
  SdfState value;
  if (!snapshot(ticket, &prim, &value)) return OBS_SDF_EDIT_STALE;
  return mask_is_type_compatible(mask, value.type) ? OBS_SDF_EDIT_OK :
                                                     OBS_SDF_EDIT_INVALID;
}
ObsSdfEditStatus obs_sdf_inspector_read_appearance(uint64_t ticket,
                                                    ObsSdfEditValues *out) {
  sync_epoch();
  if (!out) return OBS_SDF_EDIT_INVALID;
  SdfPrimitive *prim;
  SdfState value;
  if (!snapshot(ticket, &prim, &value)) return OBS_SDF_EDIT_STALE;
  ObsSdfEditValues result = {0};
  for (unsigned i = 0; i < 4; ++i) result.rgba[i] = value.rgba[i];
  result.metallic = value.metallic;
  result.roughness = value.roughness;
  for (unsigned i = 0; i < 3; ++i) result.emissive[i] = value.emissive[i];
  result.emissive_strength = value.emissive_strength;
  *out = result;
  return OBS_SDF_EDIT_OK;
}
static ObsSdfEditStatus apply_edit(uint32_t mask,
                                   const ObsSdfEditValues *values,
                                   bool panel_preview) {
  sync_epoch();
  ObsSdfEditStatus valid = obs_sdf_inspector_validate_edit(mask, values);
  if (valid != OBS_SDF_EDIT_OK)
    return valid;
  if (!journal.selected)
    return OBS_SDF_EDIT_STALE;
  SdfPrimitive *prim;
  SdfState before;
  if (!snapshot(journal.selected, &prim, &before))
    return OBS_SDF_EDIT_STALE;
  if (!mask_is_type_compatible(mask, before.type))
    return OBS_SDF_EDIT_INVALID;
  /* A grouped edit must chain from the latest value this transaction owns.
   * Otherwise an out-of-band write can be hidden by a later edit and create
   * an inverse chain that fails only after the transaction was committed. */
  if (journal.open_group && !panel_preview) {
    for (uint32_t i = journal.staged_count; i > 0; --i) {
      const SdfCommand *previous = &journal.staged[i - 1];
      if (previous->ticket == journal.selected) {
        if (!equal(&before, &previous->after))
          return OBS_SDF_EDIT_CONFLICT;
        break;
      }
    }
  }
  if (panel_preview &&
      (!journal.open_group || journal.staged_count > 1 ||
       (journal.staged_count == 1 &&
        (!journal.staged[0].preview_panel ||
         journal.staged[0].ticket != journal.selected ||
         !equal(&before, &journal.staged[0].after)))))
    return OBS_SDF_EDIT_CONFLICT;
  SdfState after = before;
  if (mask & OBS_SDF_EDIT_POSITION)
    for (unsigned i = 0; i < 3; ++i)
      after.pos[i] = (float)values->xyz[i];
  if (mask & OBS_SDF_EDIT_MATERIAL) {
    for (unsigned i = 0; i < 4; ++i)
      after.rgba[i] = (float)values->rgba[i];
    after.roughness = (float)values->roughness;
    after.metallic = (float)values->metallic;
  }
  if (mask & OBS_SDF_EDIT_SHAPE) {
    after.radius = (float)values->radius;
    after.dimensions[0] = (float)values->radius;
    after.smoothness = (float)values->smoothness;
    after.operation = values->operation;
  }
  if (mask & OBS_SDF_EDIT_BOX)
    for (unsigned i = 0; i < 3; ++i)
      after.dimensions[i] = (float)values->box_extents[i];
  if (mask & OBS_SDF_EDIT_TERRAIN) {
    after.dimensions[0] = (float)values->terrain_scale;
    after.dimensions[1] = (float)values->terrain_gain;
    after.dimensions[2] = (float)values->terrain_lacunarity;
    after.dimensions[3] = (float)values->terrain_range;
    after.octaves = values->terrain_octaves;
  }
  if (mask & OBS_SDF_EDIT_CSG) {
    after.operation = values->operation;
    after.smoothness = (float)values->smoothness;
  }
  after.radius = before.type == SDF_TYPE_SPHERE ? after.dimensions[0] : 0;
  if (mask & OBS_SDF_EDIT_EMISSION) {
    for (unsigned i = 0; i < 3; ++i)
      after.emissive[i] = (float)values->emissive[i];
    after.emissive_strength = (float)values->emissive_strength;
  }
  /* A no-op is still a command, but never generates history. */
  break_sequence();
  if (equal(&before, &after))
    return OBS_SDF_EDIT_OK;
  if (panel_preview) {
    if (!journal.staged_count) {
      journal.staged[0] = (SdfCommand){
          .ticket = journal.selected, .group = journal.open_group,
          .before = before, .after = after, .active = true,
          .preview_panel = true};
      journal.staged_count = 1;
    } else {
      journal.staged[0].after = after;
      if (equal(&journal.staged[0].before, &after))
        journal.staged_count = 0;
    }
  } else {
    if (journal.open_group && journal.staged_count == OBS_SDF_EDIT_HISTORY_CAP)
      return OBS_SDF_EDIT_FULL; /* refuse before modifying any pixels */
    SdfCommand change = {.ticket = journal.selected,
                         .group = journal.open_group ? journal.open_group
                                                     : new_group(),
                         .before = before,
                         .after = after,
                         .active = true};
    if (journal.open_group)
      journal.staged[journal.staged_count++] = change;
    else
      append(change);
  }
  publish(prim, &after);
  return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_inspector_edit(uint32_t mask,
                                        const ObsSdfEditValues *values) {
  return apply_edit(mask, values, false);
}
ObsSdfEditStatus obs_sdf_inspector_preview_edit(
    uint32_t mask, const ObsSdfEditValues *values) {
  return apply_edit(mask, values, true);
}

/* Check a whole command group before writing any scene state. Committed groups
 * are normalized to one entry per ticket: end_group folds repeated previews,
 * ordinary edits are singleton groups, and inverse/redo preserve membership.
 * That invariant lets preflight validate each member directly, without a
 * per-ticket search/simulation or partial GPU-visible publication. */
static ObsSdfEditStatus validate_inverse(const SdfCommand *steps, uint32_t n,
                                         SdfPrimitive **targets) {
  for (uint32_t i = 0; i < n; ++i) {
    const SdfCommand *c = &steps[i];
    SdfState live;
    if (!snapshot(c->ticket, &targets[i], &live))
      return OBS_SDF_EDIT_STALE;
    if (!equal(&live, &c->after))
      return OBS_SDF_EDIT_CONFLICT;
  }
  return OBS_SDF_EDIT_OK;
}
/* A command group is one indivisible history unit. Preflight and replay the
 * complete group even when object-scoped undo found it through one ticket. */
static ObsSdfEditStatus invert_at(int32_t idx, bool selected_only) {
  if (idx < 0 || idx >= (int32_t)journal.count)
    return OBS_SDF_EDIT_EMPTY;
  const uint64_t group = journal.commands[idx].group;
  int32_t begin = idx;
  while (begin > 0 && journal.commands[begin - 1].group == group)
    --begin;
  int32_t end = idx;
  while (end + 1 < (int32_t)journal.count &&
         journal.commands[end + 1].group == group)
    ++end;
  uint32_t n = (uint32_t)(end - begin + 1);
  /* Selected-object undo can reach past newer unrelated commands, but
   * must not rewrite an ancestor of any active later command on one of
   * the group's objects. Checking live values alone is insufficient:
   * A:1->2->1 looks identical to A:1, yet reverting the earlier A+B
   * group would leave both later commands with invalid preimages.
   * An already undone later original is inactive; its inverse history
   * remains available to restore it after this group is restored. */
  if (selected_only) {
    for (int32_t newer = end + 1; newer < (int32_t)journal.count; ++newer) {
      const SdfCommand *later = &journal.commands[newer];
      if (later->undo_generated || !later->active)
        continue;
      for (int32_t ancestor = begin; ancestor <= end; ++ancestor)
        if (journal.commands[ancestor].ticket == later->ticket)
          return OBS_SDF_EDIT_CONFLICT;
    }
  }
  SdfCommand inverse[OBS_SDF_EDIT_HISTORY_CAP] = {0};
  SdfPrimitive *targets[OBS_SDF_EDIT_HISTORY_CAP];
  for (uint32_t i = 0; i < n; ++i)
    inverse[i] = journal.commands[end - (int32_t)i];
  const bool undoing_inverse = journal.commands[idx].undo_generated;
  ObsSdfEditStatus checked = validate_inverse(inverse, n, targets);
  if (checked != OBS_SDF_EDIT_OK)
    return checked;
  journal.undo_sequence = true;
  /* A selected-object undo skips unrelated newer commands. Leave the
   * chronological cursor there so Ctrl+Z still traverses those commands. */
  journal.pending =
      selected_only ? (int32_t)journal.count - 1 : begin - 1;
  uint64_t generated_group = new_group();
  for (uint32_t i = 0; i < n; ++i) {
    SdfCommand cmd = inverse[i];
    /* Preflight resolved each live object. This owner-thread command invokes
     * no scene mutator between validation and publication, so reuse the
     * validated pointer instead of resolving its ticket and snapshot again. */
    SdfPrimitive *prim = targets[i];
    publish(prim, &cmd.before);
    toggle_root(cmd.root_serial);
    append((SdfCommand){.ticket = cmd.ticket,
                        .group = generated_group,
                        .before = cmd.after,
                        .after = cmd.before,
                        .root_serial = cmd.root_serial,
                        .undo_generated = true});
  }
  /* Preserve Emacs' command-sequence rule: after a command boundary, undoing
   * an undo records the inverse at the chronological tail for the next step. */
  if (!selected_only && undoing_inverse)
    journal.pending = (int32_t)journal.count - 1;
  return OBS_SDF_EDIT_OK;
}
static ObsSdfEditStatus inverse_group(bool only) {
  sync_epoch();
  if (journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  int32_t idx =
      journal.undo_sequence ? journal.pending : (int32_t)journal.count - 1;
  /* Selective undo can leave inactive original records in the chronological
   * log. Never try to replay them a second time. */
  while (idx >= 0 &&
         ((!journal.commands[idx].undo_generated &&
           !journal.commands[idx].active) ||
          (only && journal.commands[idx].undo_generated)))
    --idx;
  return invert_at(idx, false);
}
ObsSdfEditStatus obs_sdf_inspector_undo_selected(void) {
  sync_epoch();
  if (journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  if (!journal.selected)
    return OBS_SDF_EDIT_EMPTY;
  SdfPrimitive *prim;
  SdfState state;
  if (!snapshot(journal.selected, &prim, &state))
    return OBS_SDF_EDIT_STALE;
  for (int32_t i = (int32_t)journal.count - 1; i >= 0;) {
    const uint64_t group = journal.commands[i].group;
    int32_t begin = i;
    while (begin > 0 && journal.commands[begin - 1].group == group)
      --begin;
    bool touches_selection = false;
    for (int32_t j = begin; j <= i; ++j)
      if (!journal.commands[j].undo_generated &&
          journal.commands[j].active &&
          journal.commands[j].ticket == journal.selected)
        touches_selection = true;
    if (touches_selection)
      return invert_at(i, true);
    i = begin - 1;
  }
  return OBS_SDF_EDIT_EMPTY;
}
ObsSdfEditStatus obs_sdf_inspector_undo(void) { return inverse_group(false); }
ObsSdfEditStatus obs_sdf_inspector_undo_only(void) {
  return inverse_group(true);
}

/* Emacs undo-redo consumes the latest undo-generated command without adding
 * a new undo command to the log. Unlike linear cursor redo, the original
 * records remain available after intervening ordinary commands. */
ObsSdfEditStatus obs_sdf_inspector_redo(void) {
  sync_epoch();
  if (journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  if (!journal.count || !journal.commands[journal.count - 1].undo_generated)
    return OBS_SDF_EDIT_EMPTY;
  int32_t tail = (int32_t)journal.count - 1, begin = tail;
  uint64_t group = journal.commands[tail].group;
  while (begin > 0 && journal.commands[begin - 1].group == group)
    --begin;
  uint32_t n = (uint32_t)(tail - begin + 1);
  SdfCommand inverse[OBS_SDF_EDIT_HISTORY_CAP] = {0};
  SdfPrimitive *targets[OBS_SDF_EDIT_HISTORY_CAP];
  for (uint32_t i = 0; i < n; ++i) {
    SdfCommand cmd = journal.commands[tail - (int32_t)i];
    inverse[i] = (SdfCommand){.ticket = cmd.ticket,
                              .before = cmd.before,
                              .after = cmd.after,
                              .root_serial = cmd.root_serial};
  }
  ObsSdfEditStatus checked = validate_inverse(inverse, n, targets);
  if (checked != OBS_SDF_EDIT_OK)
    return checked;
  /* Undo-generated records may have evicted every original root in their
   * complete group. Partial root retention is impossible under group-aware
   * eviction; reject it before scene mutation rather than toggle half a
   * historical transaction. */
  uint32_t roots_found = 0;
  for (uint32_t i = 0; i < n; ++i) {
    bool present = false;
    for (int32_t j = 0; j < begin; ++j)
      if (!journal.commands[j].undo_generated &&
          journal.commands[j].serial == inverse[i].root_serial) {
        present = true;
        break;
      }
    roots_found += present;
  }
  if (roots_found && roots_found != n)
    return OBS_SDF_EDIT_CONFLICT;
  const bool roots_evicted = roots_found == 0;
  /* Reserve the replacement identity before publication. On the extremely
   * rare group-ID wrap, new_group() clears history; keep its truncate point
   * consistent rather than restoring the pre-reset tail below. */
  if (roots_evicted && journal.next_group == UINT64_MAX) {
    journal.count = 0;
    begin = 0;
    break_sequence();
  }
  const uint64_t replacement_group =
      roots_evicted ? new_group() : 0;
  for (uint32_t i = 0; i < n; ++i) {
    SdfPrimitive *prim = targets[i];
    publish(prim, &inverse[i].before);
    if (!roots_evicted)
      toggle_root(inverse[i].root_serial);
  }
  journal.count = (uint32_t)begin; /* no undo-of-redo records */
  if (roots_evicted) {
    /* The inverse retains the original before/after value copies. Reify them
     * as a fresh, atomic original command so another undo remains possible. */
    for (uint32_t i = 0; i < n; ++i)
      append((SdfCommand){.ticket = inverse[i].ticket,
                          .group = replacement_group,
                          .before = inverse[i].after,
                          .after = inverse[i].before,
                          .active = true});
  }
  /* An explicit redo finishes a command, like Emacs' undo-redo. */
  break_sequence();
  return OBS_SDF_EDIT_OK;
}
ObsSdfInspectorState obs_sdf_inspector_state(void) {
  sync_epoch();
  if (journal.selected) {
    SdfPrimitive *prim;
    SdfState v;
    if (!snapshot(journal.selected, &prim, &v))
      journal.selected = 0;
  }
  int32_t end =
      journal.undo_sequence ? journal.pending : (int32_t)journal.count - 1;
  uint32_t undo = 0, redo = 0;
  for (int32_t i = 0; i <= end;) {
    const uint64_t group = journal.commands[i].group;
    bool applicable = false;
    do {
      const SdfCommand *cmd = &journal.commands[i];
      if (cmd->undo_generated || cmd->active)
        applicable = true;
      ++i;
    } while (i <= end && journal.commands[i].group == group);
    if (applicable)
      ++undo;
  }
  for (int32_t i = (int32_t)journal.count - 1;
       i >= 0 && journal.commands[i].undo_generated; --i)
    if (i == (int32_t)journal.count - 1 ||
        journal.commands[i].group != journal.commands[i + 1].group)
      ++redo;
  return (ObsSdfInspectorState){
      .selection = journal.selected, .undo_count = undo, .redo_count = redo};
}
ObsSdfEditStatus obs_sdf_inspector_clear(void) {
  sync_epoch();
  /* Discarding an active group would orphan live, uncommitted edits. */
  if (journal.open_group)
    return OBS_SDF_EDIT_INVALID;
  journal.count = 0;
  break_sequence();
  return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_inspector_move(const double xyz[3]) {
  if (!xyz)
    return OBS_SDF_EDIT_INVALID;
  ObsSdfEditValues v = {0};
  for (unsigned i = 0; i < 3; ++i)
    v.xyz[i] = xyz[i];
  return obs_sdf_inspector_edit(OBS_SDF_EDIT_POSITION, &v);
}
ObsSdfEditStatus obs_sdf_inspector_tint(const double rgb[3]) {
  if (!rgb)
    return OBS_SDF_EDIT_INVALID;
  sync_epoch();
  SdfPrimitive *prim;
  SdfState before;
  if (!journal.selected || !snapshot(journal.selected, &prim, &before))
    return OBS_SDF_EDIT_STALE;
  ObsSdfEditValues v = {.roughness = before.roughness,
                        .metallic = before.metallic};
  for (unsigned i = 0; i < 3; ++i)
    v.rgba[i] = rgb[i];
  v.rgba[3] = before.rgba[3];
  return obs_sdf_inspector_edit(OBS_SDF_EDIT_MATERIAL, &v);
}

/* A pointer gesture must NOT exhaust the 64-change staging limit on its
 * 65th mouse sample. It owns one staging record, retaining its original
 * value while replacing only the latest position. Reject any other staged
 * command, stale identity, or unexpected outside writes without publication. */
ObsSdfEditStatus obs_sdf_inspector_drag_move(const double xyz[3]) {
  sync_epoch();
  if (!xyz || !coordinate(xyz[0]) || !coordinate(xyz[1]) ||
      !coordinate(xyz[2]) || !journal.open_group || !journal.selected)
    return OBS_SDF_EDIT_INVALID;
  if (journal.staged_count > 1 ||
      (journal.staged_count == 1 &&
       (!journal.staged[0].preview_drag ||
        journal.staged[0].ticket != journal.selected)))
    return OBS_SDF_EDIT_CONFLICT;
  SdfPrimitive *prim;
  SdfState before;
  if (!snapshot(journal.selected, &prim, &before))
    return OBS_SDF_EDIT_STALE;
  if (journal.staged_count && !equal(&before, &journal.staged[0].after))
    return OBS_SDF_EDIT_CONFLICT;
  SdfState after = before;
  for (unsigned i = 0; i < 3; ++i)
    after.pos[i] = (float)xyz[i];
  if (equal(&before, &after))
    return OBS_SDF_EDIT_OK;
  if (journal.staged_count == 0) {
    journal.staged[0] = (SdfCommand){.ticket = journal.selected,
                                     .group = journal.open_group,
                                     .before = before,
                                     .after = after,
                                     .active = true,
                                     .preview_drag = true};
    journal.staged_count = 1;
  } else {
    journal.staged[0].after = after;
    if (equal(&journal.staged[0].before, &after))
      journal.staged_count = 0; /* returned to origin: no undo command */
  }
  publish(prim, &after);
  return OBS_SDF_EDIT_OK;
}
