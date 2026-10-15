#include "sdf_tokens.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static ObsSdfTickets tickets;
static uint64_t ids[OBS_SDF_TICKET_LIMIT];

int main(void) {
    const uint64_t epoch = 1;
    uint64_t previous = 0;
    assert(obs_sdf_tickets_can_create());

    /* Scatter sequential IDs over the slot array to exercise hash collisions
     * rather than depending on IDs and slots having the same order. */
    for (uint32_t i = 0; i < OBS_SDF_TICKET_LIMIT; ++i) {
        const uint32_t slot = (i * 257u) & (OBS_SDF_TICKET_LIMIT - 1u);
        ids[slot] = obs_sdf_tickets_create(&tickets, epoch, slot);
        assert(ids[slot] && ids[slot] > previous);
        previous = ids[slot];
    }
    assert(obs_sdf_tickets_create(&tickets, epoch, 0) == 0);

    for (uint32_t slot = 0; slot < OBS_SDF_TICKET_LIMIT; ++slot) {
        uint32_t resolved = UINT32_MAX;
        assert(obs_sdf_tickets_at(&tickets, epoch, OBS_SDF_TICKET_LIMIT,
                                  slot) == ids[slot]);
        assert(obs_sdf_tickets_resolve(&tickets, epoch, ids[slot],
                                       OBS_SDF_TICKET_LIMIT, &resolved));
        assert(resolved == slot);
    }

    uint32_t unchanged = UINT32_C(0x12345678);
    assert(!obs_sdf_tickets_resolve(&tickets, epoch, ids[1023], 1023,
                                    &unchanged));
    assert(unchanged == UINT32_C(0x12345678));
    assert(!obs_sdf_tickets_resolve(&tickets, epoch, 0,
                                    OBS_SDF_TICKET_LIMIT, &unchanged));
    assert(!obs_sdf_tickets_resolve(&tickets, epoch, (uint64_t)INT64_MAX + 1,
                                    OBS_SDF_TICKET_LIMIT, &unchanged));
    assert(!obs_sdf_tickets_resolve(&tickets, epoch, ids[0],
                                    OBS_SDF_TICKET_LIMIT + 1u, &unchanged));
    assert(unchanged == UINT32_C(0x12345678));

    /* Epoch refresh invalidates every old hash entry before a new slot is
     * assigned. Ticket identities remain process-monotone across resets. */
    assert(!obs_sdf_tickets_resolve(&tickets, epoch + 1, ids[0],
                                    OBS_SDF_TICKET_LIMIT, &unchanged));
    assert(unchanged == UINT32_C(0x12345678));
    const uint64_t next = obs_sdf_tickets_create(&tickets, epoch + 1, 0);
    assert(next > previous);
    uint32_t resolved = UINT32_MAX;
    assert(obs_sdf_tickets_resolve(&tickets, epoch + 1, next, 1, &resolved));
    assert(resolved == 0);
    assert(obs_sdf_tickets_at(&tickets, epoch + 1, 1, 0) == next);

    puts("SDF ticket hash: 1024 scattered slots, collision resolution, "
         "epoch reset and failure atomicity PASS");
    return 0;
}
