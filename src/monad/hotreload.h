#ifndef OBSIDIAN_MONAD_HOTRELOAD_H
#define OBSIDIAN_MONAD_HOTRELOAD_H

/// File revision tracking
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct ObsReloadFingerprint {
    dev_t device;
    ino_t inode;
    int64_t size;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t changed_seconds;
    int64_t changed_nanoseconds;
} ObsReloadFingerprint;

typedef struct ObsReloadWatcher {
    ObsReloadFingerprint observed;
    ObsReloadFingerprint candidate;
    uint64_t candidate_since_ns;
    uint64_t last_check_ns;
    bool observed_valid;
    bool candidate_valid;
} ObsReloadWatcher;

/// Polling and debounce
/* Detect complete, stable on-disk edits without triggering compilation in a
 * filesystem callback. A failed compilation is acknowledged too, avoiding a
 * busy retry loop until the file changes again or the user explicitly reloads.
 * The caller owns the watcher and must only call it from the engine thread. */
bool obs_reload_poll(ObsReloadWatcher *watch, const char *path,
                     uint64_t now_ns, uint64_t poll_ns,
                     uint64_t settle_ns);
void obs_reload_acknowledge(ObsReloadWatcher *watch);
void obs_reload_reset(ObsReloadWatcher *watch);
uint64_t obs_reload_clock_ns(void);

#endif
