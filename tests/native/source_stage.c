/// source_stage.c — Worker ownership, fidelity, supersession, and stale revisions
#define _POSIX_C_SOURCE 200809L
#include "source_stage.h"
#include "publication_queue.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/// Filesystem fixtures
static void write_exact(const char *path, const char *bytes, size_t count) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(bytes, 1, count, f) == count);
    assert(fclose(f) == 0);
}

static void wait_result(ObsSourceStager *stage, uint64_t ticket,
                        ObsSourceSnapshot *out) {
    struct timespec delay = {.tv_nsec = 1000000};
    for (unsigned i = 0; i < 3000; ++i) {
        if (obs_source_stage_take(stage, ticket, out)) return;
        nanosleep(&delay, NULL);
    }
    fputs("source-stage worker timeout\n", stderr);
    abort();
}

/// Snapshot validation and persistence
int main(void) {
    char dirname[] = "/tmp/obsidian-source-stage-XXXXXX";
    assert(mkdtemp(dirname));
    char file[512], replacement[512];
    assert(snprintf(file, sizeof(file), "%s/module.mon", dirname) > 0);
    assert(snprintf(replacement, sizeof(replacement), "%s/new.mon", dirname) > 0);

    ObsSourceSnapshot bad = obs_source_snapshot_read(0, file);
    assert(bad.status == OBS_STAGE_INVALID_SOURCE);
    obs_source_snapshot_dispose(&bad);
    bad = obs_source_snapshot_read(1, file);
    assert(bad.status == OBS_STAGE_IO_ERROR);
    obs_source_snapshot_dispose(&bad);

    const char original[] = "(define (frame) 7)\n";
    write_exact(file, original, sizeof(original) - 1);
    ObsSourceSnapshot direct = obs_source_snapshot_read(1, file);
    assert(direct.status == OBS_STAGE_OK);
    assert(direct.length == sizeof(original) - 1);
    assert(strcmp(direct.source, original) == 0);
    assert(obs_source_snapshot_current(&direct));
    /* A same-size rewrite with restored mtime is NOT the staged source.
     * ctime protects against this even if size and mtime agree. */
    struct stat old_stat;
    assert(stat(file, &old_stat) == 0);
    struct timespec settle = {.tv_nsec = 20000000};
    nanosleep(&settle, NULL);
    const char invisible[] = "(define (frame) 9)\n";
    assert(sizeof(invisible) == sizeof(original));
    write_exact(file, invisible, sizeof(invisible) - 1);
    struct timespec same_times[2] = {old_stat.st_atim, old_stat.st_mtim};
    assert(utimensat(AT_FDCWD, file, same_times, 0) == 0);
    assert(!obs_source_snapshot_current(&direct));
    obs_source_snapshot_dispose(&direct);
    write_exact(file, original, sizeof(original) - 1);

    const char nul[] = {'a', 0, 'b'};
    write_exact(file, nul, sizeof(nul));
    direct = obs_source_snapshot_read(2, file);
    assert(direct.status == OBS_STAGE_INVALID_SOURCE && !direct.source);
    obs_source_snapshot_dispose(&direct);

    write_exact(file, "", 0);
    direct = obs_source_snapshot_read(3, file);
    assert(direct.status == OBS_STAGE_OK && direct.length == 0);
    obs_source_snapshot_dispose(&direct);

    ObsSourceStager stage;
    assert(obs_source_stage_open(&stage));
    assert(!obs_source_stage_request(&stage, 0, file));
    assert(!obs_source_stage_request(&stage, 1, ""));
    assert(!obs_source_stage_ready(&stage, 42));
    assert(!obs_source_stage_take(&stage, 42, &direct));

    write_exact(file, original, sizeof(original) - 1);
    assert(obs_source_stage_request(&stage, 10, file));
    wait_result(&stage, 10, &direct);
    assert(direct.ticket == 10 && direct.status == OBS_STAGE_OK);
    assert(obs_source_snapshot_current(&direct));
    assert(!obs_source_stage_take(&stage, 10, &bad));

    /* Atomic editor save: same logical path, different inode, stale snapshot. */
    write_exact(replacement, "(define (frame) 8)\n", sizeof(original)-1);
    assert(rename(replacement, file) == 0);
    assert(!obs_source_snapshot_current(&direct));
    obs_source_snapshot_dispose(&direct);

    /* Stress latest-request-wins without retaining 1000 copies of the file. */
    for (uint64_t ticket = 11; ticket < 1011; ++ticket)
        assert(obs_source_stage_request(&stage, ticket, file));
    wait_result(&stage, 1010, &direct);
    assert(direct.ticket == 1010);
    assert(direct.status == OBS_STAGE_OK && strstr(direct.source, "8") != NULL);
    assert(!obs_source_stage_ready(&stage, 1009));
    obs_source_snapshot_dispose(&direct);

    /* A stage failure must remain a typed result, not a dangling payload. */
    assert(obs_source_stage_request(&stage, 1011, replacement));
    wait_result(&stage, 1011, &direct);
    assert(direct.status == OBS_STAGE_IO_ERROR && direct.source == NULL);
    obs_source_snapshot_dispose(&direct);

    /* Oversized source must not trigger a correspondingly large allocation. */
    FILE *big = fopen(file, "wb");
    assert(big);
    assert(fseek(big, OBS_STAGE_SOURCE_MAX, SEEK_SET) == 0);
    assert(fputc('x', big) != EOF);
    assert(fclose(big) == 0);
    direct = obs_source_snapshot_read(1012, file);
    assert(direct.status == OBS_STAGE_TOO_LARGE && !direct.source);
    obs_source_snapshot_dispose(&direct);

    /* Admission must not consume a BOOT ticket before its snapshot is ready. */
    ObsPublishQueue q;
    obs_publish_queue_reset(&q);
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_BOOT));
    ObsPublishTicket t;
    assert(!obs_publish_queue_begin_ready(&q, 1u << OBS_PUBLISH_REPL, &t));
    assert(obs_publish_queue_has(&q, OBS_PUBLISH_BOOT));
    assert(obs_publish_queue_request(&q, OBS_PUBLISH_REPL));
    assert(obs_publish_queue_begin_ready(&q, 1u << OBS_PUBLISH_REPL, &t));
    assert(t.kind == OBS_PUBLISH_REPL);
    assert(obs_publish_queue_finish(&q, t, true));
    assert(obs_publish_queue_begin_ready(&q, 1u << OBS_PUBLISH_BOOT, &t));
    assert(t.kind == OBS_PUBLISH_BOOT);
    assert(obs_publish_queue_finish(&q, t, true));

    obs_source_stage_close(&stage);
    assert(unlink(file) == 0);
    assert(rmdir(dirname) == 0);
    puts("source_stage: snapshot, revision, worker, supersession, readiness OK");
    return 0;
}
