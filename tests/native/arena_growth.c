#include "arena_growth.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t capacity = UINT64_MAX;

    assert(obs_arena_growth_capacity(0, 4, 1, &capacity));
    assert(capacity == 4);
    assert(obs_arena_growth_capacity(4, 4, 4, &capacity));
    assert(capacity == 4);
    assert(obs_arena_growth_capacity(4, 4, 5, &capacity));
    assert(capacity == 8);
    assert(obs_arena_growth_capacity(8, 4, 17, &capacity));
    assert(capacity == 32);
    assert(obs_arena_growth_capacity(5 * 1024 * 1024, 5 * 1024 * 1024,
                                     5 * 1024 * 1024 + 1, &capacity));
    assert(capacity == 10 * 1024 * 1024);
    assert(obs_arena_growth_capacity(UINT64_MAX / 2 + 1, 1, UINT64_MAX,
                                     &capacity));
    assert(capacity == UINT64_MAX);

    capacity = UINT64_C(0xfeedface);
    assert(!obs_arena_growth_capacity(8, 0, 16, &capacity));
    assert(capacity == UINT64_C(0xfeedface));
    assert(!obs_arena_growth_capacity(8, 1, 16, NULL));

    puts("arena growth: minimum, geometric expansion, overflow edge, and "
         "failure atomicity PASS");
    return 0;
}
