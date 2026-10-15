#include "geometry_pending.h"
#include "geometry_upload_batch.h"
#include <limits.h>
#include <string.h>

/// Invariants
static bool counts_valid(const uint32_t counts[OBS_GEOM_POOL_COUNT]) {
    return counts && counts[OBS_GEOM_MESHLET] == counts[OBS_GEOM_BOUNDS] &&
           counts[OBS_GEOM_MESHLET] == counts[OBS_GEOM_SKIN];
}

static int find_ticket(const ObsPendingScenes *book, uint64_t ticket) {
    if (!book || !book->initialized || !ticket) return -1;
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i)
        if (book->slots[i].state != OBS_SCENE_EMPTY &&
            book->slots[i].ticket == ticket) return (int)i;
    return -1;
}

static int free_slot(const ObsPendingScenes *book, uint64_t scene_id) {
    int free_index = -1;
    for (unsigned i = 0; i < OBS_PENDING_SCENES_MAX; ++i) {
        const ObsPendingScene *scene = &book->slots[i];
        if (scene->state == OBS_SCENE_EMPTY && free_index < 0)
            free_index = (int)i;
        if (scene->state != OBS_SCENE_EMPTY && scene->scene_id == scene_id)
            return -1;
    }
    return free_index;
}

static void release_slot(ObsPendingScenes *book, unsigned index) {
    memset(&book->slots[index], 0, sizeof(book->slots[index]));
    --book->live;
}

/// Initialize a journal with the renderer's committed offset snapshot
bool obs_pending_scenes_init(ObsPendingScenes *book, const ObsGeometryBudget *initial) {
    /* Reinitialization would recycle ticket identities while foreign clients
     * may still retain them. Require a new, zeroed journal instead. */
    if (!book || !initial || book->initialized) return false;
    ObsGeometryBudget zero = *initial;
    ObsGeometryReservation ignored;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) zero.pools[i].requested = 0;
    if (zero.pools[OBS_GEOM_MESHLET].used != zero.pools[OBS_GEOM_BOUNDS].used ||
        zero.pools[OBS_GEOM_MESHLET].used != zero.pools[OBS_GEOM_SKIN].used ||
        !obs_geometry_reserve_preview(&zero, &ignored)) return false;
    ObsPendingScenes next = {0};
    next.pool_layout = zero;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        next.published[i] = next.highwater[i] = zero.pools[i].used;
    next.initialized = true;
    *book = next;
    return true;
}

/// Disjoint slot reservation
bool obs_pending_scene_reserve(ObsPendingScenes *book, uint64_t scene_id,
                               const uint32_t counts[OBS_GEOM_POOL_COUNT],
                               uint64_t *ticket) {
    if (!book || !book->initialized || !scene_id || !ticket ||
        !counts_valid(counts) || book->live >= OBS_PENDING_SCENES_MAX ||
        book->serial == UINT64_MAX) return false;
    int slot = free_slot(book, scene_id);
    if (slot < 0) return false;
    ObsGeometryBudget next = book->pool_layout;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i) {
        next.pools[i].used = book->highwater[i];
        next.pools[i].requested = counts[i];
    }
    ObsGeometryUploadPlan plan = {0};
    /* The triangle stream is byte-addressed and may need four-byte padding.
     * Use the SAME planner as GPU uploads so adjacent reservations cannot
     * overlap through hidden alignment gaps. */
    if (!obs_geometry_upload_plan(&next, &plan)) return false;
    ObsGeometryReservation reservation = plan.reservation;
    const uint64_t new_ticket = book->serial + 1;
    ObsPendingScene scene = {
        .ticket = new_ticket, .scene_id = scene_id,
        .state = OBS_SCENE_PREPARED, .geometry = reservation,
    };
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        scene.prior[i] = book->highwater[i];
    book->slots[slot] = scene;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        book->highwater[i] = reservation.end[i];
    book->serial = new_ticket;
    ++book->live;
    *ticket = new_ticket;
    return true;
}

bool obs_pending_scene_submitted(ObsPendingScenes *book, uint64_t ticket) {
    int slot = find_ticket(book, ticket);
    if (slot < 0 || book->slots[slot].state != OBS_SCENE_PREPARED) return false;
    book->slots[slot].state = OBS_SCENE_SUBMITTED;
    return true;
}

bool obs_pending_scene_observe(ObsPendingScenes *book, uint64_t ticket,
                               ObsPendingSceneFence observation) {
    int slot = find_ticket(book, ticket);
    if (slot < 0 || book->slots[slot].state != OBS_SCENE_SUBMITTED) return false;
    switch (observation) {
    case OBS_SCENE_FENCE_PENDING: return true;
    case OBS_SCENE_FENCE_SIGNALED:
        book->slots[slot].state = OBS_SCENE_READY;
        return true;
    case OBS_SCENE_FENCE_UNKNOWN:
        book->slots[slot].state = OBS_SCENE_UNKNOWN;
        return true;
    default: return false;
    }
}

/// Publication and revocation
bool obs_pending_scene_publish(ObsPendingScenes *book, uint64_t ticket,
                               ObsGeometryReservation *published_geometry) {
    int slot = find_ticket(book, ticket);
    if (!published_geometry || slot < 0 || book->slots[slot].state != OBS_SCENE_READY)
        return false;
    ObsGeometryReservation reservation = book->slots[slot].geometry;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        if (reservation.end[i] > book->published[i])
            book->published[i] = reservation.end[i];
    release_slot(book, (unsigned)slot);
    *published_geometry = reservation;
    return true;
}

bool obs_pending_scene_cancel(ObsPendingScenes *book, uint64_t ticket) {
    int slot = find_ticket(book, ticket);
    if (slot < 0 || book->slots[slot].state != OBS_SCENE_PREPARED) return false;
    const ObsGeometryReservation reservation = book->slots[slot].geometry;
    bool tail = true;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        if (book->highwater[i] != reservation.end[i]) tail = false;
    /* Rewind only the newest reservation: no later GPU transfer can alias it. */
    if (tail)
        for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
            book->highwater[i] = book->slots[slot].prior[i];
    release_slot(book, (unsigned)slot);
    return true;
}

bool obs_pending_scene_retire_quiesced(ObsPendingScenes *book, uint64_t ticket,
                                       bool gpu_quiesced) {
    int slot = find_ticket(book, ticket);
    if (!gpu_quiesced || slot < 0 ||
        book->slots[slot].state == OBS_SCENE_EMPTY) return false;
    /* A caller must separately retire GPU staging and fences. The journal
     * merely releases the logical reservation; it owns no Vulkan objects. */
    release_slot(book, (unsigned)slot);
    return true;
}

bool obs_pending_scenes_drain(ObsPendingScenes *book) {
    if (!book || !book->initialized || book->live) return false;
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        book->highwater[i] = book->published[i];
    return true;
}

/// Copy-only inspection
bool obs_pending_scene_snapshot(const ObsPendingScenes *book, uint64_t ticket,
                                 ObsPendingScene *out) {
    int slot = find_ticket(book, ticket);
    if (slot < 0 || !out) return false;
    *out = book->slots[slot];
    return true;
}

uint32_t obs_pending_scenes_live(const ObsPendingScenes *book) {
    return book && book->initialized ? book->live : 0;
}
