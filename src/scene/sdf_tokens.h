/// sdf_tokens — process-monotone, scene-epoch-checked Monad SDF tickets
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define OBS_SDF_TICKET_LIMIT 1024u
#define OBS_SDF_TICKET_HASH_CAP (OBS_SDF_TICKET_LIMIT * 2u)
/* Zero-initialize before first use. Tickets are retained by slot until the
 * lifetime epoch changes; there is no per-slot removal or ID recycling. */
typedef struct {
    uint64_t lifetime_epoch;
    uint64_t by_slot[OBS_SDF_TICKET_LIMIT];
    /* slot+1, so zero marks an empty bucket; tickets are recovered from
     * by_slot. At most half the fixed table is occupied per scene epoch. */
    uint16_t by_ticket[OBS_SDF_TICKET_HASH_CAP];
} ObsSdfTickets;
bool obs_sdf_tickets_can_create(void);
/* Create a process-monotone positive Int64 ticket for an unused slot in this
 * epoch. Returns zero on invalid input, duplicate slot, or exhausted IDs. */
uint64_t obs_sdf_tickets_create(ObsSdfTickets *tickets, uint64_t lifetime,
                                uint32_t slot);
/* Resolve only slots below count. On failure, *slot is left unchanged. */
bool obs_sdf_tickets_resolve(ObsSdfTickets *tickets, uint64_t lifetime,
                              uint64_t ticket, uint32_t count, uint32_t *slot);
/* Return the ticket at slot, or zero if input is invalid / no ticket exists. */
uint64_t obs_sdf_tickets_at(ObsSdfTickets *tickets, uint64_t lifetime,
                             uint32_t count, uint32_t slot);
