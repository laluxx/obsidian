/// frame_state.c — One-frame typed borrowing without exposing GPU pointers
#include "frame_state.h"
#include <math.h>
#include <limits.h>
#include <stddef.h>

/// Private frame clock
static ObsFrameState s_frame;
static void advance_epoch(void) {
    /* Do not let zero become a publicly issued epoch. */
    ++s_frame.epoch;
    if (!s_frame.epoch) ++s_frame.epoch;
}

/// Lifecycle
void obs_frame_state_begin(uint32_t width, uint32_t height,
                           uint32_t slot, double delta_seconds) {
    /* Even an accidental nested begin invalidates all earlier frame borrows. */
    advance_epoch();
    if (s_frame.number != UINT64_MAX) ++s_frame.number;
    s_frame.width = width;
    s_frame.height = height;
    s_frame.slot = slot;
    s_frame.delta_seconds = isfinite(delta_seconds) && delta_seconds >= 0.0
                          ? delta_seconds : 0.0;
    s_frame.active = true;
}
void obs_frame_state_end(void) {
    if (s_frame.active) {
        s_frame.active = false;
        advance_epoch();
    }
}
const ObsFrameState *obs_frame_state_current(void) {
    return s_frame.active ? &s_frame : NULL;
}
const ObsFrameState *obs_frame_state_identity(void) { return &s_frame; }
uint64_t obs_frame_state_epoch(void) { return s_frame.epoch; }
bool obs_frame_state_validate(const ObsFrameState *identity, uint64_t epoch) {
    return identity == &s_frame && s_frame.active && epoch &&
           epoch == s_frame.epoch;
}
