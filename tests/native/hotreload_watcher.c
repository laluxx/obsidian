/// HotReloadWatcherTests — no re-entrant compilation from a file event
#define _POSIX_C_SOURCE 200809L
#include "hotreload.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/// Filesystem fixture
static void replace_text(const char *file, const char *value) {
    FILE *out = fopen(file, "wb");
    assert(out);
    assert(fwrite(value, 1, strlen(value), out) == strlen(value));
    assert(fclose(out) == 0);
}
int main(void) {
    char file[] = "/tmp/obs-reload-test-XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0); close(fd);
    replace_text(file, "version-1");
    ObsReloadWatcher w = {0};
    const uint64_t ms = 1000000u;
    assert(!obs_reload_poll(&w, file, 1*ms, 0, 350*ms));
    assert(w.observed_valid);
    assert(!obs_reload_poll(&w, file, 2*ms, 0, 350*ms));
    replace_text(file, "version-two");
    assert(!obs_reload_poll(&w, file, 10*ms, 0, 350*ms));
    assert(!obs_reload_poll(&w, file, 359*ms, 0, 350*ms));
    assert(obs_reload_poll(&w, file, 360*ms, 0, 350*ms));
    obs_reload_acknowledge(&w);
    assert(!obs_reload_poll(&w, file, 800*ms, 0, 350*ms));
    /* Preserve source length and forge the original mtime: ctime must still
     * generate a change candidate, not allow an invisible edit. */
    struct stat older;
    assert(stat(file, &older) == 0);
    struct timespec settle = {.tv_nsec = 20000000};
    nanosleep(&settle, NULL);
    replace_text(file, "VERSION-two");
    struct timespec same_times[2] = {older.st_atim, older.st_mtim};
    assert(utimensat(AT_FDCWD, file, same_times, 0) == 0);
    assert(!obs_reload_poll(&w, file, 900*ms, 0, 350*ms));
    assert(obs_reload_poll(&w, file, 1260*ms, 0, 350*ms));
    obs_reload_acknowledge(&w);
    replace_text(file, "version-three");
    assert(!obs_reload_poll(&w, file, 1600*ms, 250*ms, 350*ms));
    assert(!obs_reload_poll(&w, file, 1700*ms, 250*ms, 350*ms));
    assert(obs_reload_poll(&w, file, 2100*ms, 250*ms, 350*ms));
    obs_reload_reset(&w);
    assert(!w.observed_valid);
    unlink(file);
    puts("ok: first observation, debounce, acknowledge, poll interval, reset");
    return 0;
}
