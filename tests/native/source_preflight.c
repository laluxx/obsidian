/// source_preflight.c — Thread confinement and bounded worker preflight tests
#define _POSIX_C_SOURCE 200809L
#include "source_stage.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/// Test hooks, intentionally independent of MonadC
static pthread_t owner;
static atomic_uint begins, checks, ends;
static void *hook_begin(void) {
    assert(!pthread_equal(owner, pthread_self()));
    atomic_fetch_add(&begins, 1);
    return malloc(1);
}
static void hook_check(void *context, ObsSourceSnapshot *s) {
    assert(context && s && s->source);
    assert(!pthread_equal(owner, pthread_self()));
    atomic_fetch_add(&checks, 1);
    s->preflight.status = s->source[0] == '!' ?
        OBS_PREFLIGHT_DIAGNOSTIC : OBS_PREFLIGHT_ACCEPTED;
    s->preflight.diagnostic_count = s->source[0] == '!' ? 1 : 0;
    snprintf(s->preflight.message, sizeof(s->preflight.message),
             "copy-of-%s", s->source);
}
static void hook_end(void *context) {
    assert(!pthread_equal(owner, pthread_self()));
    atomic_fetch_add(&ends, 1);
    free(context);
}
static void *failing_begin(void) { return NULL; }

/// Deterministic latest-wins and worker shutdown
static ObsSourceSnapshot take_or_abort(ObsSourceStager *stage, uint64_t ticket) {
    ObsSourceSnapshot result = {0};
    for (unsigned i = 0; i < 4000; ++i) {
        if (obs_source_stage_take(stage, ticket, &result)) return result;
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    abort();
}
int main(void) {
    owner = pthread_self();
    char path[] = "/tmp/obsidian-preflight-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "good", 4) == 4);
    close(fd);
    ObsSourceStager stage;
    const ObsStagePreflightHooks hooks = {hook_begin, hook_check, hook_end};
    assert(!obs_source_stage_open_with_preflight(&stage,
        (ObsStagePreflightHooks){.check = hook_check}));
    assert(obs_source_stage_open_with_preflight(&stage, hooks));
    for (uint64_t i = 1; i <= 1000; ++i)
        assert(obs_source_stage_request(&stage, i, path));
    ObsSourceSnapshot result = take_or_abort(&stage, 1000);
    assert(result.status == OBS_STAGE_OK);
    assert(result.preflight.status == OBS_PREFLIGHT_ACCEPTED);
    assert(strcmp(result.preflight.message, "copy-of-good") == 0);
    assert(!obs_source_stage_take(&stage, 1, &result));
    obs_source_snapshot_dispose(&result);
    /* An advisory diagnostic must still carry the immutable source bytes. */
    fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, "!bad", 4) == 4);
    close(fd);
    assert(obs_source_stage_request(&stage, 1001, path));
    result = take_or_abort(&stage, 1001);
    assert(result.status == OBS_STAGE_OK && result.source);
    assert(result.preflight.status == OBS_PREFLIGHT_DIAGNOSTIC);
    assert(result.preflight.diagnostic_count == 1);
    obs_source_snapshot_dispose(&result);
    obs_source_stage_close(&stage);
    assert(atomic_load(&begins) == 1 && atomic_load(&ends) == 1);
    assert(atomic_load(&checks) >= 2);
    assert(obs_source_stage_open_with_preflight(&stage,
        (ObsStagePreflightHooks){failing_begin, hook_check, hook_end}));
    assert(obs_source_stage_request(&stage, 1002, path));
    result = take_or_abort(&stage, 1002);
    assert(result.status == OBS_STAGE_OK);
    assert(result.preflight.status == OBS_PREFLIGHT_UNAVAILABLE);
    obs_source_snapshot_dispose(&result);
    obs_source_stage_close(&stage);
    unlink(path);
    puts("source preflight thread confinement/diagnostics/supersession: PASS");
    return 0;
}
