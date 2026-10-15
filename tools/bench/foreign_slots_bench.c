#define _POSIX_C_SOURCE 200809L
/// foreign_slots_bench.c — Compare old scan to constant-time checked membership
#include "slot_index.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/// Reproducible CPU microbenchmark
struct Slot { char data[64]; };
static volatile size_t sink;
#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif
static NOINLINE bool old_scan(const struct Slot *slots, size_t n,
                               const void *candidate, size_t *out) {
    for (size_t i = 0; i < n; ++i)
        if (&slots[i] == candidate) { *out = i; return true; }
    return false;
}
static uint64_t ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint32_t step(uint32_t *rng) {
    return *rng = *rng * UINT32_C(1664525) + UINT32_C(1013904223);
}
static void bench(size_t n, unsigned runs) {
    struct Slot *slots = calloc(n, sizeof(*slots));
    if (!slots) exit(1);
    uint32_t rng = 123;
    size_t *targets = malloc(runs * sizeof(*targets));
    if (!targets) exit(1);
    for (unsigned i = 0; i < runs; ++i) targets[i] = step(&rng) % n;
    size_t found = 0;
    const uint64_t start_old = ns();
    for (unsigned i = 0; i < runs; ++i) {
        size_t idx = 0;
        if (old_scan(slots, n, &slots[targets[i]], &idx)) found += idx;
    }
    const uint64_t end_old = ns();
    sink = found;
    found = 0;
    const uint64_t start_new = ns();
    for (unsigned i = 0; i < runs; ++i) {
        size_t idx = 0;
        if (obs_slot_index(slots, sizeof(*slots), n, &slots[targets[i]], &idx)) found += idx;
    }
    const uint64_t end_new = ns();
    sink = found;
    printf("slots=%-5zu scan=%.2f ns/lookup checked-slot=%.2f ns/lookup\n",
           n, (double)(end_old - start_old) / runs,
           (double)(end_new - start_new) / runs);
    free(targets);
    free(slots);
}
int main(int argc, char **argv) {
    unsigned runs = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 100000;
    if (runs < 1000 || runs > 10000000) return 2;
    bench(16, runs);
    bench(1024, runs);
    bench(8192, runs);
    return sink == SIZE_MAX ? 0 : 0;
}
