/// identity_bench — measures C-side logical reference resolution only
#define _POSIX_C_SOURCE 200809L
#include "object_identity.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct { uint64_t id; int payload; } Item;
static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
int main(int argc, char **argv) {
    size_t n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 8192;
    size_t iterations = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 1000000;
    if (!n || n > 100000 || !iterations || iterations > 10000000) return 2;
    Item *items = calloc(n, sizeof(*items));
    if (!items) return 2;
    ObsIdentityRegistry r = {0};
    ObsIdentityView v = {items, sizeof(*items), n, offsetof(Item, id), 1, OBS_ID_MESH};
    const void **tokens = malloc(n * sizeof(*tokens));
    uint32_t *generations = malloc(n * sizeof(*generations));
    if (!tokens || !generations) return 2;
    for (size_t i = 0; i < n; ++i)
        if (!obs_identity_borrow(&r, v, i, &tokens[i], &generations[i])) return 2;
    uint64_t start = now_ns();
    size_t checksum = 0;
    for (size_t i = 0; i < iterations; ++i) {
        size_t pos = (i * 2654435761ULL) % n, resolved = SIZE_MAX;
        if (!obs_identity_resolve(&r, v, tokens[pos], generations[pos], &resolved)) return 2;
        checksum += resolved;
    }
    uint64_t total = now_ns() - start;
    printf("identity resolve: %zu slots, %zu calls: %.2f ns/call (checksum=%zu)\n",
           n, iterations, (double)total / (double)iterations, checksum);
    ++v.revision;  /* cold structural synchronization cost */
    start = now_ns();
    if (!obs_identity_sync(&r, v)) return 2;
    total = now_ns() - start;
    printf("structural remap: %zu slots: %.2f us\n", n, (double)total / 1000.0);
    free(items); free(tokens); free(generations);
    obs_identity_destroy(&r);
    return 0;
}
