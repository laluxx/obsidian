/// publication.c — Generation leases and conservative QTT contract validation
#include "publication.h"
#include <string.h>

/// Contract validation (requires compiler-authored descriptors)
static bool valid_export(const ObsExportContract *entry) {
    return entry && entry->name && entry->name[0] &&
           entry->type_identity != 0 &&
           (entry->effects & ~OBS_EFFECT_ALL) == 0 &&
           entry->quantity <= OBS_QUANTITY_UNRESTRICTED &&
           entry->lifetime <= OBS_LIFETIME_ENGINE;
}
static bool valid_image(const ObsImageContract *image) {
    if (!image || !image->compiler_verified ||
        (image->export_count && !image->exports) ||
        image->export_count > 65536) return false;
    for (size_t i = 0; i < image->export_count; ++i) {
        if (!valid_export(&image->exports[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(image->exports[i].name, image->exports[j].name) == 0)
                return false; /* Ambiguous compiler export is never accepted. */
    }
    return true;
}
ObsContractResult obs_publication_validate(const ObsImageContract *old_image,
                                           const ObsImageContract *candidate) {
    if (!candidate || !candidate->compiler_verified ||
        (old_image && !old_image->compiler_verified))
        return OBS_CONTRACT_UNVERIFIED;
    if (!valid_image(candidate) || (old_image && !valid_image(old_image)))
        return OBS_CONTRACT_INVALID;
    if (!old_image) return OBS_CONTRACT_OK;
    for (size_t i = 0; i < old_image->export_count; ++i) {
        const ObsExportContract *prior = &old_image->exports[i];
        const ObsExportContract *next = NULL;
        for (size_t j = 0; j < candidate->export_count; ++j)
            if (strcmp(candidate->exports[j].name, prior->name) == 0) {
                next = &candidate->exports[j]; break;
            }
        if (!next) return OBS_CONTRACT_MISSING_EXPORT;
        if (prior->type_identity != next->type_identity)
            return OBS_CONTRACT_TYPE_MISMATCH;
        if (prior->quantity != next->quantity)
            return OBS_CONTRACT_QUANTITY_MISMATCH;
        if (prior->lifetime != next->lifetime)
            return OBS_CONTRACT_LIFETIME_MISMATCH;
        /* A replacement must not unexpectedly demand additional effects. */
        if ((next->effects & ~prior->effects) != 0)
            return OBS_CONTRACT_EFFECT_ESCALATION;
    }
    if (old_image->state_schema != candidate->state_schema &&
        !candidate->has_verified_migration)
        return OBS_CONTRACT_STATE_MIGRATION_REQUIRED;
    return OBS_CONTRACT_OK;
}

/// Execution generations and quiescence
void obs_publication_reset(ObsPublication *state) {
    if (!state) return;
    *state = (ObsPublication){0};
}
bool obs_publication_begin_call(ObsPublication *state, ObsCodeLease *lease) {
    if (!state || !lease || lease->active || state->phase != OBS_LIVE_READY ||
        !state->epoch || state->active_calls == UINT32_MAX) return false;
    ++state->active_calls;
    *lease = (ObsCodeLease){ .epoch = state->epoch, .active = true };
    return true;
}
bool obs_publication_end_call(ObsPublication *state, ObsCodeLease *lease) {
    if (!state || !lease || !lease->active || !state->active_calls ||
        lease->epoch != state->epoch || state->phase != OBS_LIVE_READY)
        return false;
    --state->active_calls;
    lease->active = false;
    return true;
}
bool obs_publication_begin_legacy_load(ObsPublication *state) {
    if (!state || state->active_calls || state->phase == OBS_LIVE_LOADING ||
        state->attempts == UINT64_MAX) return false;
    ++state->attempts;
    state->phase = OBS_LIVE_LOADING;
    return true;
}
bool obs_publication_begin_atomic_load(ObsPublication *state) {
    if (!state || state->active_calls || state->phase == OBS_LIVE_LOADING ||
        state->attempts == UINT64_MAX || state->epoch == UINT64_MAX)
        return false;
    state->phase_before_atomic_load = state->phase;
    state->atomic_loading = true;
    ++state->attempts;
    state->phase = OBS_LIVE_LOADING;
    return true;
}
void obs_publication_finish_atomic_load(ObsPublication *state, bool success) {
    if (!state || state->phase != OBS_LIVE_LOADING || !state->atomic_loading)
        return;
    if (success) {
        ++state->epoch;
        ++state->successes;
        state->phase = OBS_LIVE_READY;
    } else {
        ++state->failures;
        state->phase = state->phase_before_atomic_load;
    }
    state->atomic_loading = false;
}

void obs_publication_finish_legacy_load(ObsPublication *state, bool success) {
    if (!state || state->phase != OBS_LIVE_LOADING) return;
    if (success && state->epoch != UINT64_MAX) {
        ++state->epoch;
        ++state->successes;
        state->phase = OBS_LIVE_READY;
    } else {
        ++state->failures;
        state->phase = OBS_LIVE_POISONED;
    }
}
bool obs_publication_can_retire(const ObsPublication *state) {
    return state && !state->active_calls &&
           state->phase != OBS_LIVE_LOADING;
}

/// Host mutation admission
bool obs_publication_mutations_allowed(const ObsPublication *state) {
    return state && state->phase == OBS_LIVE_READY &&
           !state->atomic_loading && state->epoch != 0;
}
