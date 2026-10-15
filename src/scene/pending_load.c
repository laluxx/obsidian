#include "pending_load.h"
#include <string.h>

/// Validation
static bool valid(const ObsPendingSceneLoad *load, uint64_t ticket) {
    return load && load->initialized && ticket &&
           load->ticket == ticket && load->state != OBS_LOAD_EMPTY;
}

static void clear_candidate(ObsPendingSceneLoad *load) {
    load->owned = (ObsSceneLoadPayload){0};
    load->ticket = 0;
    load->scene_id = 0;
    load->state = OBS_LOAD_EMPTY;
}

static void dispose_candidate(ObsPendingSceneLoad *load) {
    ObsSceneLoadPayload owned = load->owned;
    clear_candidate(load);
    owned.dispose(owned.object, owned.user);
}

bool obs_scene_load_init(ObsPendingSceneLoad *load,
                          const ObsSceneLoadBackend *backend) {
    if (!load || load->initialized || !backend || !backend->start ||
        !backend->poll || !backend->commit ||
        !backend->discard_ready || !backend->recover_quiesced) return false;
    ObsPendingSceneLoad fresh = {0};
    fresh.backend = *backend;
    fresh.initialized = true;
    *load = fresh;
    return true;
}

/// Adopt a private scene candidate only after backend admission
bool obs_scene_load_prepare(ObsPendingSceneLoad *load, uint64_t scene_id,
                            const uint32_t counts[OBS_GEOM_POOL_COUNT],
                            const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                            ObsSceneLoadPayload *candidate, uint64_t *ticket) {
    if (!load || !load->initialized || load->state != OBS_LOAD_EMPTY ||
        !scene_id || !counts || !spans || !candidate || !candidate->object ||
        !candidate->dispose || !candidate->activate || !ticket) return false;

    uint64_t issued = 0;
    ObsSceneStartResult start = load->backend.start(load->backend.user,
                                           scene_id, counts, spans, &issued);
    if (start == OBS_SCENE_START_REJECTED) return false;
    if (!issued || (start != OBS_SCENE_START_ACCEPTED &&
                    start != OBS_SCENE_START_UNKNOWN)) return false;
    load->state = start == OBS_SCENE_START_ACCEPTED ? OBS_LOAD_IN_FLIGHT
                                                    : OBS_LOAD_QUARANTINED;
    if (start == OBS_SCENE_START_UNKNOWN) ++load->quarantined;
    load->owned = *candidate;
    *candidate = (ObsSceneLoadPayload){0};
    load->scene_id = scene_id;
    load->ticket = issued;
    ++load->accepted;
    *ticket = issued;
    return true;
}

/// Nonblocking GPU completion observation
ObsSceneLoadState obs_scene_load_poll(ObsPendingSceneLoad *load, uint64_t ticket) {
    if (!valid(load, ticket)) return OBS_LOAD_EMPTY;
    if (load->state != OBS_LOAD_IN_FLIGHT) return load->state;
    switch (load->backend.poll(load->backend.user, ticket)) {
    case OBS_ASYNC_IN_FLIGHT: return load->state;
    case OBS_ASYNC_READY: return load->state = OBS_LOAD_READY;
    case OBS_ASYNC_QUARANTINED:
        ++load->quarantined;
        return load->state = OBS_LOAD_QUARANTINED;
    default:
        /* A backend suddenly forgetting an accepted ticket cannot justify
         * freeing the payload. Retain it until quiescence is proven. */
        ++load->quarantined;
        return load->state = OBS_LOAD_QUARANTINED;
    }
}

/// Safe-point publication and retirement
bool obs_scene_load_publish(ObsPendingSceneLoad *load, uint64_t ticket,
                             ObsSceneGpuBases *published) {
    if (!valid(load, ticket) || load->state != OBS_LOAD_READY || !published)
        return false;
    const ObsSceneLoadPayload candidate = load->owned;
    if (candidate.ready && !candidate.ready(candidate.object, candidate.user))
        return false;
    ObsSceneGpuBases bases;
    /* The Vulkan adapter must commit actual allocator cursors here. Any
     * failure leaves this candidate invisible and held for recovery. */
    if (!load->backend.commit(load->backend.user, ticket, &bases)) {
        ++load->quarantined;
        load->state = OBS_LOAD_QUARANTINED;
        return false;
    }
    /* From here activation MUST NOT FAIL. All allocations, host registration
     * and physics publication must have been prepared beforehand. */
    clear_candidate(load);
    candidate.activate(candidate.object, candidate.user, &bases);
    ++load->activated;
    *published = bases;
    return true;
}

bool obs_scene_load_cancel(ObsPendingSceneLoad *load, uint64_t ticket) {
    if (!valid(load, ticket)) return false;
    if (load->state != OBS_LOAD_READY) return false;
    bool success = load->backend.discard_ready(load->backend.user, ticket);
    if (!success) {
        ++load->quarantined;
        load->state = OBS_LOAD_QUARANTINED;
        return false;
    }
    dispose_candidate(load);
    ++load->discarded;
    return true;
}

bool obs_scene_load_recover_quiesced(ObsPendingSceneLoad *load, uint64_t ticket,
                                      bool gpu_quiesced) {
    if (!valid(load, ticket) || load->state != OBS_LOAD_QUARANTINED ||
        !gpu_quiesced || !load->backend.recover_quiesced(load->backend.user,
                                                          ticket, true)) return false;
    dispose_candidate(load);
    ++load->discarded;
    return true;
}

bool obs_scene_load_close(ObsPendingSceneLoad *load) {
    if (!load || !load->initialized || load->state != OBS_LOAD_EMPTY)
        return false;
    /* Prevent use of an obsolete backend pointer after renderer teardown. */
    load->initialized = false;
    return true;
}
