/// publication_queue.h — Bounded owner-thread publication request scheduler
#ifndef OBSIDIAN_PUBLICATION_QUEUE_H
#define OBSIDIAN_PUBLICATION_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Request identities
/* Work items identify the source, not a compiled native image. The compiler
 * still owns compilation/QTT checks/native publication; no jobs run off-thread.
 * The scheduler is driven strictly by the engine owner thread. */
typedef enum ObsPublishKind {
    OBS_PUBLISH_BOOT = 0,
    OBS_PUBLISH_REPL = 1,
    OBS_PUBLISH_KIND_COUNT = 2
} ObsPublishKind;

typedef struct ObsPublishTicket {
    uint64_t id;
    ObsPublishKind kind;
} ObsPublishTicket;

typedef struct ObsPublishQueue {
    uint64_t next_id;
    uint64_t pending_id[OBS_PUBLISH_KIND_COUNT];
    uint64_t pending_since_ns[OBS_PUBLISH_KIND_COUNT];
    uint64_t active_since_ns;
    ObsPublishTicket active;
    bool busy;
    ObsPublishKind last_dispatched;
    uint64_t enqueued, coalesced, cancelled;
    uint64_t started, completed, succeeded, failed;
} ObsPublishQueue;

/// Scheduling contract
void obs_publish_queue_reset(ObsPublishQueue *queue);
/* BOOT: the newest change supersedes a queued reload, no partial state kept.
 * REPL: duplicate queueing is idempotent (its source lives in the REPL model).
 * Return false on invalid arguments or exhausted monotonically assigned IDs. */
bool obs_publish_queue_request(ObsPublishQueue *queue, ObsPublishKind kind);
/* Caller-provided monotonic admission timestamp; 0 disables wait telemetry.
 * Existing queue_request callers retain identical scheduling semantics. */
bool obs_publish_queue_request_at(ObsPublishQueue *queue,
                                  ObsPublishKind kind, uint64_t now_ns);
/* Cancel *queued* work only. An active compiler call is never interrupted. */
bool obs_publish_queue_cancel(ObsPublishQueue *queue, ObsPublishKind kind);
/* Begin at most one request at a checkpoint, alternating under contention.
 * If callbacks are still executing, the caller must not call begin(). */
bool obs_publish_queue_begin(ObsPublishQueue *queue, ObsPublishTicket *out);
/* Mask out unprepared lanes without consuming their pending ticket. */
bool obs_publish_queue_begin_ready(ObsPublishQueue *queue,
                                   unsigned ready_mask, ObsPublishTicket *out);
/* Tickets must match exactly; duplicate/stale completion is rejected. */
bool obs_publish_queue_finish(ObsPublishQueue *queue,
                              ObsPublishTicket ticket, bool success);
bool obs_publish_queue_has(const ObsPublishQueue *queue, ObsPublishKind kind);
size_t obs_publish_queue_pending(const ObsPublishQueue *queue);

#endif
