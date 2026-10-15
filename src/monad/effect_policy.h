/// effect_policy.h — Engine-side effect and borrow admission for foreign calls
#ifndef OBSIDIAN_MONAD_EFFECT_POLICY_H
#define OBSIDIAN_MONAD_EFFECT_POLICY_H
#include "publication.h"
#include <stdbool.h>
#include <stdint.h>

/// Host-owned effect policy (not a QTT theorem)
/* MonadC is authoritative for proof-carrying quantity/effect judgments.
 * The engine MUST still reject physical effects at inappropriate checkpoints.
 * This is a deterministic runtime gate, NOT proof that user code is safe. */
typedef struct ObsEffectEnvironment {
    const ObsPublication *publication;
    bool owner_thread;
    bool engine_open;
    bool frame_open;
} ObsEffectEnvironment;
typedef enum ObsEffectVerdict {
    OBS_EFFECT_ADMIT = 0,
    OBS_EFFECT_DENY_INVALID,
    OBS_EFFECT_DENY_THREAD,
    OBS_EFFECT_DENY_PHASE,
    OBS_EFFECT_DENY_LIFETIME
} ObsEffectVerdict;

/* effects is a subset of OBS_EFFECT_ALL; lifetime is the minimum live region
 * required by this particular operation. FRAME means the current CPU frame
 * must be active; ENGINE means an open, valid engine; VALUE is call-local.
 * No native pointers are returned from these checks. */
ObsEffectVerdict obs_effect_admit(const ObsEffectEnvironment *env,
                                 uint32_t effects, ObsLifetime lifetime);

#endif
