/// publication_queue.c — One checkpoint, one publication; no shadow runtime
#include "publication_queue.h"
#include <string.h>

/// Validation and initialization
static bool valid_kind(ObsPublishKind kind) {
    return kind == OBS_PUBLISH_BOOT || kind == OBS_PUBLISH_REPL;
}

void obs_publish_queue_reset(ObsPublishQueue *queue) {
    if (!queue) return;
    memset(queue, 0, sizeof(*queue));
    /* First contested checkpoint serves interactive work. */
    queue->last_dispatched = OBS_PUBLISH_BOOT;
}

/// Bounded request admission
bool obs_publish_queue_request_at(ObsPublishQueue *queue, ObsPublishKind kind,
                                  uint64_t now_ns) {
    if (!queue || !valid_kind(kind)) return false;
    uint64_t *pending = &queue->pending_id[kind];
    if (*pending && kind == OBS_PUBLISH_REPL) return true;
    /* Never wrap a ticket: the host must be reopened on exhaustion. */
    if (queue->next_id == UINT64_MAX) return false;
    const uint64_t id = ++queue->next_id;
    if (*pending) ++queue->coalesced;
    else ++queue->enqueued;
    *pending = id;
    queue->pending_since_ns[kind] = now_ns;
    return true;
}

bool obs_publish_queue_request(ObsPublishQueue *queue, ObsPublishKind kind) {
    return obs_publish_queue_request_at(queue, kind, 0);
}

bool obs_publish_queue_cancel(ObsPublishQueue *queue, ObsPublishKind kind) {
    if (!queue || !valid_kind(kind) || !queue->pending_id[kind]) return false;
    queue->pending_id[kind] = 0;
    queue->pending_since_ns[kind] = 0;
    ++queue->cancelled;
    return true;
}

bool obs_publish_queue_has(const ObsPublishQueue *queue, ObsPublishKind kind) {
    return queue && valid_kind(kind) &&
        (queue->pending_id[kind] != 0 ||
         (queue->busy && queue->active.kind == kind));
}

size_t obs_publish_queue_pending(const ObsPublishQueue *queue) {
    if (!queue) return 0;
    return (queue->pending_id[OBS_PUBLISH_BOOT] != 0) +
           (queue->pending_id[OBS_PUBLISH_REPL] != 0);
}

/// Fair dispatch, token-checked completion
bool obs_publish_queue_begin_ready(ObsPublishQueue *queue,
                                   unsigned ready_mask, ObsPublishTicket *out) {
    if (!queue || !out || queue->busy) return false;
    const bool boot = queue->pending_id[OBS_PUBLISH_BOOT] != 0 &&
                      (ready_mask & (1u << OBS_PUBLISH_BOOT)) != 0;
    const bool repl = queue->pending_id[OBS_PUBLISH_REPL] != 0 &&
                      (ready_mask & (1u << OBS_PUBLISH_REPL)) != 0;
    if (!boot && !repl) return false;
    ObsPublishKind kind;
    if (boot && repl)
        kind = queue->last_dispatched == OBS_PUBLISH_BOOT
             ? OBS_PUBLISH_REPL : OBS_PUBLISH_BOOT;
    else
        kind = repl ? OBS_PUBLISH_REPL : OBS_PUBLISH_BOOT;
    const ObsPublishTicket ticket = { queue->pending_id[kind], kind };
    queue->pending_id[kind] = 0;
    queue->active_since_ns = queue->pending_since_ns[kind];
    queue->pending_since_ns[kind] = 0;
    queue->last_dispatched = kind;
    queue->active = ticket;
    queue->busy = true;
    ++queue->started;
    *out = ticket;
    return true;
}

bool obs_publish_queue_finish(ObsPublishQueue *queue,
                              ObsPublishTicket ticket, bool success) {
    if (!queue || !queue->busy || !valid_kind(ticket.kind) || !ticket.id ||
        queue->active.id != ticket.id || queue->active.kind != ticket.kind)
        return false;
    queue->active = (ObsPublishTicket){0};
    queue->busy = false;
    queue->active_since_ns = 0;
    ++queue->completed;
    if (success) ++queue->succeeded;
    else ++queue->failed;
    return true;
}

/// Compatibility entry point: all queued lanes are runnable
bool obs_publish_queue_begin(ObsPublishQueue *queue, ObsPublishTicket *out) {
    return obs_publish_queue_begin_ready(queue,
        (1u << OBS_PUBLISH_KIND_COUNT) - 1u, out);
}
