/// frame_state.h — CPU-side frame capabilities and epoch lifecycle
#ifndef OBSIDIAN_FRAME_STATE_H
#define OBSIDIAN_FRAME_STATE_H
#include <stdbool.h>
#include <stdint.h>

/// Frame snapshot
/* The engine owns this object; it is never freed by user code. A borrowed
 * handle is valid ONLY during the CPU frame whose epoch it captured. */
typedef struct ObsFrameState {
    uint64_t number;
    uint64_t epoch;
    double delta_seconds;
    uint32_t width;
    uint32_t height;
    uint32_t slot;
    bool active;
} ObsFrameState;

/// Lifecycle
void obs_frame_state_begin(uint32_t width, uint32_t height,
                           uint32_t slot, double delta_seconds);
void obs_frame_state_end(void);
const ObsFrameState *obs_frame_state_current(void);
const ObsFrameState *obs_frame_state_identity(void);
uint64_t obs_frame_state_epoch(void);
bool obs_frame_state_validate(const ObsFrameState *identity, uint64_t epoch);

#endif
