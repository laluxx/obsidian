/// slot_index.c — Adversarial CPU-side foreign slot membership regression
#include "slot_index.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/// Deterministic tests
struct Slot { uint64_t x, y; char padding[48]; };
static uint32_t step(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}
int main(void) {
    const size_t n = 4096;
    struct Slot *slots = calloc(n, sizeof(*slots));
    assert(slots);
    size_t index = SIZE_MAX;
    assert(!obs_slot_index(NULL, sizeof(*slots), n, slots, &index));
    assert(index == 0);
    assert(!obs_slot_index(slots, 0, n, slots, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), 0, slots, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), n, NULL, &index));
    assert(obs_slot_index(slots, sizeof(*slots), n, &slots[0], &index) && index == 0);
    assert(obs_slot_index(slots, sizeof(*slots), n, &slots[n-1], &index) && index == n-1);
    assert(!obs_slot_index(slots, sizeof(*slots), n, slots + n, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), n, (char *)slots + 1, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), n, (char *)&slots[7] + sizeof(*slots)-1, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), 1, &slots[1], &index));
    assert(!obs_slot_index(slots, sizeof(*slots), n, (void *)(uintptr_t)UINTPTR_MAX, &index));
    assert(!obs_slot_index(slots, sizeof(*slots), n, (void *)(uintptr_t)1, &index));
    /* Simulate stale pointers after a compaction: generation checks must
     * invalidate them in the caller; this resolver ONLY checks membership. */
    for (size_t i=0; i<n; ++i) {
        assert(obs_slot_index(slots, sizeof(*slots), n, &slots[i], &index));
        assert(index == i);
    }
    uint32_t rng = 0x195FDD21u;
    for (size_t iteration = 0; iteration < 100000; ++iteration) {
        size_t idx = (size_t)(step(&rng) % (n + 1));
        size_t byte = (size_t)(step(&rng) % sizeof(*slots));
        uintptr_t candidate_address = (uintptr_t)slots +
            (uintptr_t)idx * (uintptr_t)sizeof(*slots) + (uintptr_t)byte;
        void *candidate = (void *)candidate_address;
        bool expected = idx < n && byte == 0;
        index = SIZE_MAX;
        bool accepted = obs_slot_index(slots, sizeof(*slots), n, candidate, &index);
        assert(accepted == expected);
        assert(!accepted || index == idx);
        assert(accepted || index == 0);
    }
    free(slots);
    puts("PASS: 100000 adversarial foreign-slot queries, 4096 live slots");
    return 0;
}
