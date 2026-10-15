/// source_stage.h — Immutable off-thread Monad source snapshots
#ifndef OBSIDIAN_MONAD_SOURCE_STAGE_H
#define OBSIDIAN_MONAD_SOURCE_STAGE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define OBS_STAGE_PATH_CAP 4096u
#define OBS_STAGE_SOURCE_MAX (16u * 1024u * 1024u)

/// Advisory compiler preflight
/* The isolated compiler session checks syntax/types, not the running image.
 * A rejection here MUST NEVER veto authoritative owner-thread publication:
 * the isolated session may lack live imports and state. These diagnostics are
 * advisory and can be displayed without borrowing compiler-owned strings. */
typedef enum ObsPreflightStatus {
    OBS_PREFLIGHT_DISABLED = 0,
    OBS_PREFLIGHT_ACCEPTED,
    OBS_PREFLIGHT_DIAGNOSTIC,
    OBS_PREFLIGHT_UNAVAILABLE
} ObsPreflightStatus;

typedef struct ObsPreflightReport {
    ObsPreflightStatus status;
    uint32_t diagnostic_count;
    uint64_t elapsed_us;
    char code[48];
    char message[192];
} ObsPreflightReport;

struct ObsSourceSnapshot;
typedef struct ObsStagePreflightHooks {
    /* Called once from the worker thread; returns worker-owned context. */
    void *(*begin)(void);
    void (*check)(void *context, struct ObsSourceSnapshot *snapshot);
    void (*end)(void *context);
} ObsStagePreflightHooks;

/// Snapshot contract
/* Pure disk I/O: the worker MUST NOT access MonadC, the scene, GLFW or Vulkan.
 * Memory ownership moves to the owner thread only through take(). */
typedef enum ObsStageStatus {
    OBS_STAGE_OK = 0,
    OBS_STAGE_IO_ERROR,
    OBS_STAGE_CHANGED,
    OBS_STAGE_TOO_LARGE,
    OBS_STAGE_INVALID_SOURCE,
    OBS_STAGE_OUT_OF_MEMORY
} ObsStageStatus;

typedef struct ObsSourceSnapshot {
    char *source; /* NUL-terminated; dispose with obs_source_snapshot_dispose */
    size_t length;
    uint64_t ticket;
    char path[OBS_STAGE_PATH_CAP];
    dev_t device;
    ino_t inode;
    off_t size;
    struct timespec mtime;
    struct timespec ctime;
    ObsStageStatus status;
    ObsPreflightReport preflight;
} ObsSourceSnapshot;

/// Single worker, latest requested revision wins
typedef struct ObsSourceStager {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t event;
    char path[OBS_STAGE_PATH_CAP];
    uint64_t requested_ticket;
    bool queued, ready, stopping;
    ObsSourceSnapshot completed;
    bool started;
    ObsStagePreflightHooks preflight_hooks;
} ObsSourceStager;

bool obs_source_stage_open(ObsSourceStager *stage);
/* Hook callbacks execute exclusively on the worker, never the renderer.
 * NULL means I/O-only staging. */
bool obs_source_stage_open_with_preflight(ObsSourceStager *stage,
                                           ObsStagePreflightHooks hooks);
/* Stop/join before destroying the engine host. May wait for bounded disk I/O. */
void obs_source_stage_close(ObsSourceStager *stage);
/* Thread-safe; superseding a job discards its result, never its caller memory. */
bool obs_source_stage_request(ObsSourceStager *stage, uint64_t ticket,
                              const char *path);
bool obs_source_stage_ready(ObsSourceStager *stage, uint64_t ticket);
bool obs_source_stage_take(ObsSourceStager *stage, uint64_t ticket,
                           ObsSourceSnapshot *out);
void obs_source_snapshot_dispose(ObsSourceSnapshot *snapshot);
/* Check on owner thread immediately before publication (atomic file replace safe). */
bool obs_source_snapshot_current(const ObsSourceSnapshot *snapshot);
/* Public for headless, deterministic validation of short/unstable reads. */
ObsSourceSnapshot obs_source_snapshot_read(uint64_t ticket, const char *path);

#endif
