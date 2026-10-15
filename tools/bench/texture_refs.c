#define _POSIX_C_SOURCE 200809L
#include "texture_refs.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// Benchmark fixture
static uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

int main(void) {
    ObsTextureRefs *refs = calloc(1, sizeof(*refs));
    assert(refs);
    char keys[4096][32];
    for (unsigned i = 0; i < 4096; ++i) {
        snprintf(keys[i], sizeof(keys[i]), "texture_%04u.png", i);
        assert(obs_texture_refs_enroll(refs, i, keys[i]));
    }
    uint32_t mask = 0;
    uint64_t linear_time, indexed_time;
    const unsigned rounds = 200000;
    uint64_t begin = ns_now();
    for (unsigned i = 0; i < rounds; ++i) {
        unsigned target = (i * 2654435761u) & 4095u;
        unsigned found = 4096;
        for (unsigned j = 0; j < 4096; ++j)
            if (!strcmp(keys[j], keys[target])) { found = j; break; }
        mask ^= found;
    }
    linear_time = ns_now() - begin;
    begin = ns_now();
    for (unsigned i = 0; i < rounds; ++i) {
        unsigned target = (i * 2654435761u) & 4095u;
        uint32_t found = UINT32_MAX;
        assert(obs_texture_refs_find(refs, keys[target], &found));
        mask ^= found;
    }
    indexed_time = ns_now() - begin;
    printf("texture-refs 4096 keys / %u lookups (checksum %u): linear %.2f ns, indexed %.2f ns\n",
           rounds, mask, (double)linear_time / rounds, (double)indexed_time / rounds);
    free(refs);
    return 0;
}
