/// effect_policy.c — Foreign host effect and region validation regressions
#include "effect_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/// Explicit phase/effect/borrow matrix
static void contract_matrix(void) {
    ObsPublication state = {0};
    ObsEffectEnvironment env = {
        .publication = &state, .owner_thread = true,
        .engine_open = true, .frame_open = true
    };
    assert(obs_effect_admit(NULL, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_INVALID);
    assert(obs_effect_admit(&env, 1u << 29, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_INVALID);
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, (ObsLifetime)99) == OBS_EFFECT_DENY_INVALID);
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_PHASE);
    assert(obs_publication_begin_atomic_load(&state));
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_ADMIT);
    assert(obs_effect_admit(&env, 0, OBS_LIFETIME_VALUE) == OBS_EFFECT_ADMIT);
    for (uint32_t forbidden = OBS_EFFECT_WRITE; forbidden <= OBS_EFFECT_IO; forbidden <<= 1)
        assert(obs_effect_admit(&env, forbidden, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_PHASE);
    env.frame_open = false;
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_FRAME) == OBS_EFFECT_DENY_LIFETIME);
    env.frame_open = true;
    env.owner_thread = false;
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_THREAD);
    env.owner_thread = true;
    obs_publication_finish_atomic_load(&state, true);
    assert(obs_effect_admit(&env, OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                            OBS_LIFETIME_ENGINE) == OBS_EFFECT_ADMIT);
    env.engine_open = false;
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_LIFETIME);
    env.engine_open = true;
    state.phase = OBS_LIVE_POISONED;
    assert(obs_effect_admit(&env, OBS_EFFECT_READ, OBS_LIFETIME_ENGINE) == OBS_EFFECT_DENY_PHASE);
}

/// Exhaustive effect masks and deterministic phase transitions
static void bounded_fuzz(void) {
    ObsPublication state = {0};
    ObsEffectEnvironment env = {
        .publication = &state, .owner_thread = true, .engine_open = true,
        .frame_open = true
    };
    uint32_t rng = 0x125fd23bu;
    for (int i = 0; i < 50000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        state.phase = (ObsLivePhase)(rng % 4);
        state.atomic_loading = ((rng >> 4) & 1u) != 0;
        state.epoch = (rng & 1u) + 1u;
        uint32_t mask = (rng >> 8) & OBS_EFFECT_ALL;
        ObsEffectVerdict verdict = obs_effect_admit(&env, mask, OBS_LIFETIME_ENGINE);
        if (state.phase == OBS_LIVE_READY && !state.atomic_loading)
            assert(verdict == OBS_EFFECT_ADMIT);
        else if (state.phase == OBS_LIVE_LOADING || state.atomic_loading)
            assert(verdict == ((mask & (OBS_EFFECT_WRITE | OBS_EFFECT_GPU |
                OBS_EFFECT_AUDIO | OBS_EFFECT_IO)) ? OBS_EFFECT_DENY_PHASE :
                (state.phase == OBS_LIVE_EMPTY || state.phase == OBS_LIVE_POISONED ?
                 OBS_EFFECT_DENY_PHASE : OBS_EFFECT_ADMIT)));
        else assert(verdict == OBS_EFFECT_DENY_PHASE);
    }
}
int main(void) {
    contract_matrix(); bounded_fuzz();
    puts("host effect policy: 50,000 cases, phases/regions/effects PASS");
}
