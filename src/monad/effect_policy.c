/// effect_policy.c — Deterministic checks at the engine foreign boundary
#include "effect_policy.h"

/// Owner, live-region and publication admission
ObsEffectVerdict obs_effect_admit(const ObsEffectEnvironment *env,
                                 uint32_t effects, ObsLifetime lifetime) {
    if (!env || !env->publication || (effects & ~OBS_EFFECT_ALL) != 0 ||
        lifetime > OBS_LIFETIME_ENGINE)
        return OBS_EFFECT_DENY_INVALID;
    if (!env->owner_thread) return OBS_EFFECT_DENY_THREAD;
    if (!env->engine_open || (lifetime == OBS_LIFETIME_FRAME && !env->frame_open))
        return OBS_EFFECT_DENY_LIFETIME;
    const ObsPublication *p = env->publication;
    if (p->phase == OBS_LIVE_EMPTY || p->phase == OBS_LIVE_POISONED)
        return OBS_EFFECT_DENY_PHASE;
    /* Compiler exploration may read engine-owned state while preparing a
     * candidate but must not write, enqueue GPU work, or perform audio/IO.
     * No host mutation here can be rolled back by native-image publication. */
    if (p->phase == OBS_LIVE_LOADING || p->atomic_loading) {
        return effects & (OBS_EFFECT_WRITE | OBS_EFFECT_GPU |
                          OBS_EFFECT_AUDIO | OBS_EFFECT_IO)
            ? OBS_EFFECT_DENY_PHASE : OBS_EFFECT_ADMIT;
    }
    if (p->phase != OBS_LIVE_READY || !p->epoch)
        return OBS_EFFECT_DENY_PHASE;
    return OBS_EFFECT_ADMIT;
}
