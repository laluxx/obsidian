/// FontScanBenchmark — reproducible headless text scanning profile
#define _POSIX_C_SOURCE 200809L
#include "font_utf8.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// Timing
static uint64_t ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000ULL + t.tv_nsec;
}
static volatile size_t observed;

static void profile(const char *name, const char *s, unsigned loops) {
    uint64_t timings[15];
    size_t len = strlen(s);
    for (unsigned k=0;k<15;k++) {
        uint64_t before = ns();
        size_t n=0;
        for (unsigned i=0;i<loops;i++) n += obs_font_utf8_count(s);
        uint64_t elapsed = ns() - before;
        observed = n;
        timings[k] = elapsed;
    }
    for (unsigned i=1;i<15;i++) {
        uint64_t value=timings[i]; unsigned j=i;
        while (j && timings[j-1]>value) { timings[j]=timings[j-1]; --j; }
        timings[j]=value;
    }
    printf("%s\t%zu\t%u\t%.3f\t%.3f\t%.1f\n", name, len, loops,
           timings[7]/1.0e6, timings[14]/1.0e6,
           (double)len*loops/(1024.0*1024.0)/(timings[7]/1.0e9));
}

int main(int argc, char **argv) {
    unsigned loops = 100000;
    if (argc>1) { loops = (unsigned)strtoul(argv[1], NULL, 10); if (!loops) return 2; }
    char ascii[4096+1];
    memset(ascii, 'x', 4096); ascii[4096]=0;
    char multilingual[4096+1];
    const char sample[]="Monad \xe2\x82\xac \xf0\x9f\x98\x80 ";
    size_t count=0;
    while (count+sizeof(sample)-1<=4096) {
        memcpy(multilingual+count,sample,sizeof(sample)-1); count+=sizeof(sample)-1;
    }
    multilingual[count]=0;
    puts("workload\tbytes\titerations\tmedian_ms\tworst_ms\tmedian_MiB_per_s");
    profile("ascii",ascii,loops);
    profile("mixed",multilingual,loops);
    return 0;
}
