#define _POSIX_C_SOURCE 200809L
/// omdl_digest_bench.c — CRC64-ECMA cache integrity microbenchmark
#include "omdl_cache.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/// Reference bitwise implementation (comparison only)
static uint64_t crc64_reference(const unsigned char *p, size_t n) {
    uint64_t crc=0;
    while (n--) {
        crc ^= (uint64_t)*p++ << 56;
        for (unsigned j=0;j<8;++j)
            crc=(crc << 1) ^ ((crc >> 63) ? UINT64_C(0x42F0E1EBA9EA3693) : 0);
    }
    return crc;
}

static double now_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts) != 0) exit(2);
    return (double)ts.tv_sec+(double)ts.tv_nsec*1e-9;
}

/// Controlled microbenchmark — not GPU, disk or whole engine timing
int main(void) {
    const size_t n=8u*1024u*1024u;
    unsigned char *buffer=malloc(n);
    if (!buffer) return 2;
    uint32_t random=0x9e3779b9;
    for (size_t i=0;i<n;++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        buffer[i]=(unsigned char)random;
    }
    volatile uint64_t sink=0;
    const unsigned repeats=3;
    double t=now_sec();
    for (unsigned i=0;i<repeats;++i)
        sink = sink ^ crc64_reference(buffer,n);
    double reference=now_sec()-t;
    t=now_sec();
    for (unsigned i=0;i<repeats;++i)
        sink = sink ^ omdl_cache_digest_bytes(0,buffer,n);
    double fast=now_sec()-t;
    uint64_t expected=crc64_reference(buffer,n);
    if (expected!=omdl_cache_digest_bytes(0,buffer,n)) return 1;
    printf("OMDL CRC64 checksum reference: %.1f MiB/s; table: %.1f MiB/s; ratio %.2fx (sink=%" PRIu64 ")\n",
            ((double)n*repeats/1048576.0)/reference,
            ((double)n*repeats/1048576.0)/fast,
            reference/fast, (uint64_t)sink);
    free(buffer);
    return 0;
}
