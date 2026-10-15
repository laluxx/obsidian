/// sdf_property_bridge — editor SDF properties share Monad/agent undo history
#pragma once
#include <stdint.h>
#include "sdf_inspector.h"

/* This is the one gateway for sidebar edits to supported SDF sphere/box shapes.
 * The UI passes COPIES of its field values, never mutated live primitives.
 * Call on the engine owner thread. */
ObsSdfEditStatus obs_sdf_property_apply(uint32_t sdf_index, uint32_t mask,
                                        const ObsSdfEditValues *values);

/* An editor-owned speculative gesture is distinct from the viewport's group.
 * Preview samples replace one staged record; failed cancellation retains
 * ownership until the conflict is resolved. */
ObsSdfEditStatus obs_sdf_property_begin(uint32_t sdf_index);
ObsSdfEditStatus obs_sdf_property_preview(uint32_t sdf_index, uint32_t mask,
                                          const ObsSdfEditValues *values);
ObsSdfEditStatus obs_sdf_property_commit(void);
ObsSdfEditStatus obs_sdf_property_cancel(void);
bool obs_sdf_property_active(void);
