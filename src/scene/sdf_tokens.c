/// sdf_tokens — stable identifiers for Monad-created SDF spheres
#include "sdf_tokens.h"
#include <limits.h>
#include <string.h>
_Static_assert((OBS_SDF_TICKET_HASH_CAP &
                (OBS_SDF_TICKET_HASH_CAP - 1u)) == 0,
               "ticket hash capacity must be a power of two");
_Static_assert(OBS_SDF_TICKET_LIMIT <= UINT16_MAX,
               "ticket hash slot encoding must fit in uint16_t");
/* IDs stay positive Int64, never wrap or recycle across scene resets. */
static uint64_t s_next_token;
static void refresh(ObsSdfTickets *t, uint64_t epoch) {
    if (t->lifetime_epoch != epoch) {
        memset(t->by_slot, 0, sizeof(t->by_slot));
        memset(t->by_ticket, 0, sizeof(t->by_ticket));
        t->lifetime_epoch = epoch;
    }
}
static uint32_t ticket_bucket(uint64_t ticket) {
    /* Mix sequential process-monotone IDs before masking to the power-of-two
     * table; linear probing stays at or below a 0.5 load factor. */
    ticket ^= ticket >> 33;
    ticket *= UINT64_C(0xff51afd7ed558ccd);
    ticket ^= ticket >> 33;
    ticket *= UINT64_C(0xc4ceb9fe1a85ec53);
    ticket ^= ticket >> 33;
    return (uint32_t)ticket & (OBS_SDF_TICKET_HASH_CAP - 1u);
}
static bool index_ticket(ObsSdfTickets *t, uint64_t ticket, uint32_t slot) {
    uint32_t bucket = ticket_bucket(ticket);
    for (uint32_t probes = 0; probes < OBS_SDF_TICKET_HASH_CAP; ++probes) {
        if (!t->by_ticket[bucket]) {
            t->by_ticket[bucket] = (uint16_t)(slot + 1u);
            return true;
        }
        bucket = (bucket + 1u) & (OBS_SDF_TICKET_HASH_CAP - 1u);
    }
    return false;
}
bool obs_sdf_tickets_can_create(void) { return s_next_token < INT64_MAX; }
uint64_t obs_sdf_tickets_create(ObsSdfTickets *t, uint64_t epoch,
                                uint32_t slot) {
    if (!t || !epoch || slot >= OBS_SDF_TICKET_LIMIT ||
        !obs_sdf_tickets_can_create()) return 0;
    refresh(t, epoch);
    if (t->by_slot[slot]) return 0;
    const uint64_t next = s_next_token + 1u;
    if (!index_ticket(t, next, slot)) return 0;
    s_next_token = next;
    t->by_slot[slot] = next;
    return next;
}
bool obs_sdf_tickets_resolve(ObsSdfTickets *t, uint64_t epoch,
                              uint64_t ticket, uint32_t count, uint32_t *slot) {
    if (!t || !epoch || !ticket || ticket > INT64_MAX || !slot ||
        count > OBS_SDF_TICKET_LIMIT) return false;
    refresh(t, epoch);
    /* Avoid hashing for small scenes, where a short contiguous scan is cheaper. */
    if (count <= 16u) {
        for (uint32_t i = 0; i < count; ++i) {
            if (t->by_slot[i] == ticket) {
                *slot = i;
                return true;
            }
        }
        return false;
    }
    uint32_t bucket = ticket_bucket(ticket);
    for (uint32_t probes = 0; probes < OBS_SDF_TICKET_HASH_CAP; ++probes) {
        const uint16_t slot_plus_one = t->by_ticket[bucket];
        if (!slot_plus_one) return false;
        const uint32_t candidate = (uint32_t)slot_plus_one - 1u;
        if (candidate < count && t->by_slot[candidate] == ticket) {
            *slot = candidate;
            return true;
        }
        bucket = (bucket + 1u) & (OBS_SDF_TICKET_HASH_CAP - 1u);
    }
    return false;
}
uint64_t obs_sdf_tickets_at(ObsSdfTickets *t, uint64_t epoch,
                             uint32_t count, uint32_t slot) {
    if (!t || !epoch || count > OBS_SDF_TICKET_LIMIT || slot >= count) return 0;
    refresh(t, epoch);
    return t->by_slot[slot];
}
