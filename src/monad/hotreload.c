/// hotreload.c — Stable file revision and debounce tracking
#define _POSIX_C_SOURCE 200809L
/// Dependencies
#include "hotreload.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

/// Implementation

/// Revision utilities
static bool get_revision(const char *path, ObsReloadFingerprint *out) {
    if (!path || !*path || !out) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
        return false;
    *out = (ObsReloadFingerprint){
        .device = st.st_dev, .inode = st.st_ino, .size = st.st_size,
        .modified_seconds = st.st_mtim.tv_sec,
        .modified_nanoseconds = st.st_mtim.tv_nsec,
        .changed_seconds = st.st_ctim.tv_sec,
        .changed_nanoseconds = st.st_ctim.tv_nsec
    };
    return true;
}

static bool identical(const ObsReloadFingerprint *a,
                      const ObsReloadFingerprint *b) {
    return a->device == b->device && a->inode == b->inode &&
           a->size == b->size &&
           a->modified_seconds == b->modified_seconds &&
           a->modified_nanoseconds == b->modified_nanoseconds &&
           a->changed_seconds == b->changed_seconds &&
           a->changed_nanoseconds == b->changed_nanoseconds;
}

uint64_t obs_reload_clock_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

/// Watcher lifecycle
void obs_reload_reset(ObsReloadWatcher *watch) {
    if (watch) memset(watch, 0, sizeof(*watch));
}

void obs_reload_acknowledge(ObsReloadWatcher *watch) {
    if (!watch || !watch->candidate_valid) return;
    watch->observed = watch->candidate;
    watch->observed_valid = true;
    watch->candidate_valid = false;
}

bool obs_reload_poll(ObsReloadWatcher *watch, const char *path,
                     uint64_t now_ns, uint64_t poll_ns,
                     uint64_t settle_ns) {
    if (!watch || !path || !now_ns) return false;
    if (watch->last_check_ns && now_ns >= watch->last_check_ns &&
        now_ns - watch->last_check_ns < poll_ns) return false;
    watch->last_check_ns = now_ns;
    ObsReloadFingerprint current;
    if (!get_revision(path, &current)) {
        watch->candidate_valid = false;
        return false;
    }
    if (!watch->observed_valid) {
        watch->observed = current;
        watch->observed_valid = true;
        return false; /* First observation is never a reload request. */
    }
    if (identical(&current, &watch->observed)) {
        watch->candidate_valid = false;
        return false;
    }
    if (!watch->candidate_valid || !identical(&current, &watch->candidate) ||
        now_ns < watch->candidate_since_ns) {
        watch->candidate = current;
        watch->candidate_since_ns = now_ns;
        watch->candidate_valid = true;
        return false;
    }
    return now_ns - watch->candidate_since_ns >= settle_ns;
}
