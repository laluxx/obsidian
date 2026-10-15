/// source_stage.c — Bounded worker, source fidelity and revision pinning
#define _POSIX_C_SOURCE 200809L
#include "source_stage.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/// Revision identity
static bool same_revision(const struct stat *a, const struct stat *b) {
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

void obs_source_snapshot_dispose(ObsSourceSnapshot *s) {
    if (!s) return;
    free(s->source);
    *s = (ObsSourceSnapshot){0};
}

bool obs_source_snapshot_current(const ObsSourceSnapshot *s) {
    if (!s || s->status != OBS_STAGE_OK || !s->source) return false;
    struct stat now;
    if (stat(s->path, &now) != 0 || !S_ISREG(now.st_mode)) return false;
    return now.st_dev == s->device && now.st_ino == s->inode &&
           now.st_size == s->size &&
           now.st_mtim.tv_sec == s->mtime.tv_sec &&
           now.st_mtim.tv_nsec == s->mtime.tv_nsec &&
           now.st_ctim.tv_sec == s->ctime.tv_sec &&
           now.st_ctim.tv_nsec == s->ctime.tv_nsec;
}

/// Bounded source read (I/O phase never calls the compiler)
ObsSourceSnapshot obs_source_snapshot_read(uint64_t ticket, const char *path) {
    ObsSourceSnapshot s = {.ticket = ticket, .status = OBS_STAGE_IO_ERROR};
    if (!ticket || !path) { s.status = OBS_STAGE_INVALID_SOURCE; return s; }
    size_t npath = strnlen(path, sizeof(s.path));
    if (!npath || npath == sizeof(s.path)) {
        s.status = OBS_STAGE_INVALID_SOURCE;
        return s;
    }
    memcpy(s.path, path, npath + 1);
    /* Check file type *before* stdio reads: opening a FIFO without O_NONBLOCK
     * could otherwise stall shutdown of the one worker indefinitely. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return s;
    struct stat before, after, named;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) { close(fd); return s; }
    FILE *f = fdopen(fd, "rb");
    if (!f) { close(fd); return s; }
    if ((uintmax_t)before.st_size > OBS_STAGE_SOURCE_MAX) {
        s.status = OBS_STAGE_TOO_LARGE;
        goto done;
    }
    s.length = (size_t)before.st_size;
    s.source = malloc(s.length + 1);
    if (!s.source) { s.status = OBS_STAGE_OUT_OF_MEMORY; goto done; }
    if (fread(s.source, 1, s.length, f) != s.length ||
        fgetc(f) != EOF || ferror(f)) goto done;
    if (fstat(fileno(f), &after) != 0 ||
        stat(path, &named) != 0 ||
        !same_revision(&before, &after) ||
        !same_revision(&before, &named)) {
        s.status = OBS_STAGE_CHANGED;
        goto done;
    }
    if (memchr(s.source, 0, s.length)) {
        s.status = OBS_STAGE_INVALID_SOURCE;
        goto done;
    }
    s.source[s.length] = 0;
    s.device = before.st_dev;
    s.inode = before.st_ino;
    s.size = before.st_size;
    s.mtime = before.st_mtim;
    s.ctime = before.st_ctim;
    s.status = OBS_STAGE_OK;
done:
    if (fclose(f) != 0 && s.status == OBS_STAGE_OK)
        s.status = OBS_STAGE_IO_ERROR;
    if (s.status != OBS_STAGE_OK) {
        free(s.source);
        s.source = NULL;
        s.length = 0;
    }
    return s;
}

/// Mailbox / worker lifecycle
static void *stage_worker(void *context) {
    ObsSourceStager *stage = context;
    /* A compiler session is thread-affine; create and destroy it on THIS
     * thread, even when the queue is stopped during a pending request. */
    void *preflight = stage->preflight_hooks.begin
                    ? stage->preflight_hooks.begin() : NULL;
    pthread_mutex_lock(&stage->mutex);
    for (;;) {
        while (!stage->queued && !stage->stopping)
            pthread_cond_wait(&stage->event, &stage->mutex);
        if (stage->stopping) break;
        const uint64_t ticket = stage->requested_ticket;
        char path[OBS_STAGE_PATH_CAP];
        memcpy(path, stage->path, sizeof(path));
        stage->queued = false;
        pthread_mutex_unlock(&stage->mutex);
        ObsSourceSnapshot next = obs_source_snapshot_read(ticket, path);
        if (next.status == OBS_STAGE_OK && stage->preflight_hooks.check) {
            if (preflight) stage->preflight_hooks.check(preflight, &next);
            else next.preflight.status = OBS_PREFLIGHT_UNAVAILABLE;
        }
        pthread_mutex_lock(&stage->mutex);
        if (!stage->stopping && !stage->queued &&
            stage->requested_ticket == ticket) {
            obs_source_snapshot_dispose(&stage->completed);
            stage->completed = next;
            stage->ready = true;
        } else {
            obs_source_snapshot_dispose(&next);
        }
    }
    pthread_mutex_unlock(&stage->mutex);
    if (preflight && stage->preflight_hooks.end)
        stage->preflight_hooks.end(preflight);
    return NULL;
}

bool obs_source_stage_open_with_preflight(ObsSourceStager *stage,
                                          ObsStagePreflightHooks hooks) {
    if (!stage) return false;
    if (hooks.check && (!hooks.begin || !hooks.end)) return false;
    memset(stage, 0, sizeof(*stage));
    stage->preflight_hooks = hooks;
    if (pthread_mutex_init(&stage->mutex, NULL) != 0) return false;
    if (pthread_cond_init(&stage->event, NULL) != 0) {
        pthread_mutex_destroy(&stage->mutex);
        return false;
    }
    if (pthread_create(&stage->thread, NULL, stage_worker, stage) != 0) {
        pthread_cond_destroy(&stage->event);
        pthread_mutex_destroy(&stage->mutex);
        return false;
    }
    stage->started = true;
    return true;
}

bool obs_source_stage_open(ObsSourceStager *stage) {
    return obs_source_stage_open_with_preflight(stage, (ObsStagePreflightHooks){0});
}

void obs_source_stage_close(ObsSourceStager *stage) {
    if (!stage || !stage->started) return;
    pthread_mutex_lock(&stage->mutex);
    stage->stopping = true;
    pthread_cond_signal(&stage->event);
    pthread_mutex_unlock(&stage->mutex);
    pthread_join(stage->thread, NULL);
    obs_source_snapshot_dispose(&stage->completed);
    pthread_cond_destroy(&stage->event);
    pthread_mutex_destroy(&stage->mutex);
    memset(stage, 0, sizeof(*stage));
}

bool obs_source_stage_request(ObsSourceStager *stage, uint64_t ticket,
                              const char *path) {
    if (!stage || !stage->started || !ticket || !path) return false;
    size_t npath = strnlen(path, OBS_STAGE_PATH_CAP);
    if (!npath || npath == OBS_STAGE_PATH_CAP) return false;
    pthread_mutex_lock(&stage->mutex);
    if (stage->stopping) { pthread_mutex_unlock(&stage->mutex); return false; }
    memcpy(stage->path, path, npath + 1);
    stage->requested_ticket = ticket;
    stage->queued = true;
    stage->ready = false;
    obs_source_snapshot_dispose(&stage->completed);
    pthread_cond_signal(&stage->event);
    pthread_mutex_unlock(&stage->mutex);
    return true;
}

bool obs_source_stage_ready(ObsSourceStager *stage, uint64_t ticket) {
    if (!stage || !stage->started || !ticket) return false;
    pthread_mutex_lock(&stage->mutex);
    bool ready = stage->ready && stage->completed.ticket == ticket;
    pthread_mutex_unlock(&stage->mutex);
    return ready;
}

bool obs_source_stage_take(ObsSourceStager *stage, uint64_t ticket,
                           ObsSourceSnapshot *out) {
    if (!stage || !stage->started || !ticket || !out) return false;
    pthread_mutex_lock(&stage->mutex);
    bool ready = stage->ready && stage->completed.ticket == ticket;
    if (ready) {
        *out = stage->completed;
        stage->completed = (ObsSourceSnapshot){0};
        stage->ready = false;
    }
    pthread_mutex_unlock(&stage->mutex);
    return ready;
}
