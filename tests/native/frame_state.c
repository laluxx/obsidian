/// frame_state native contract — one-frame lifetimes with no SDK dependencies
#include "frame_state.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

/// Lifetime and snapshots
int main(void) {
    assert(!obs_frame_state_current());
    assert(!obs_frame_state_validate(obs_frame_state_identity(), 0));
    obs_frame_state_begin(1920, 1080, 1, 0.016);
    const ObsFrameState *first = obs_frame_state_current();
    assert(first && first->number == 1 && first->width == 1920);
    assert(first->height == 1080 && first->slot == 1);
    assert(fabs(first->delta_seconds - 0.016) < 0.00000001);
    uint64_t gen1 = first->epoch;
    assert(obs_frame_state_validate(first, gen1));
    obs_frame_state_end();
    assert(!obs_frame_state_current());
    assert(!obs_frame_state_validate(first, gen1));
    uint64_t after = obs_frame_state_epoch();
    obs_frame_state_end();
    assert(obs_frame_state_epoch() == after); // double-end does not mint epochs
    obs_frame_state_begin(0, 0, 0, NAN);
    assert(obs_frame_state_current()->number == 2);
    assert(obs_frame_state_current()->delta_seconds == 0.0);
    assert(!obs_frame_state_validate(first, gen1));
    uint64_t gen2 = obs_frame_state_epoch();
    obs_frame_state_begin(640, 480, 0, -5.0); // nested begin also expires
    assert(obs_frame_state_current()->number == 3);
    assert(obs_frame_state_current()->delta_seconds == 0.0);
    assert(!obs_frame_state_validate(first, gen2));
    assert(obs_frame_state_validate(first, obs_frame_state_epoch()));
    assert(!obs_frame_state_validate(NULL, obs_frame_state_epoch()));
    obs_frame_state_end();
    puts("PASS: frame-scoped snapshot, single owner, expiry and invalid deltas");
    return 0;
}
