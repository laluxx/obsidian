/// semantic_preflight.c — SDK-header-compatible compiler session regression
#define _POSIX_C_SOURCE 200809L
#include "semantic_preflight.h"
#include <monad/compiler.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// SDK-shaped compiler doubles, no fabricated native candidate APIs
struct monad_compiler_session { pthread_t owner; };
struct monad_compiler_source { char *text; size_t count; };
struct monad_compiler_diagnostic_set { monad_compiler_diagnostic_t diagnostic; bool bad; };
static unsigned created, destroyed, checked;
uint32_t monad_compiler_abi_version(void) { return MONAD_COMPILER_ABI_VERSION; }
monad_status_t monad_compiler_session_create(monad_compiler_session_t **out,
                                           monad_error_t *error) {
    (void)error;
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->owner = pthread_self();
    ++created;
    return MONAD_OK;
}
void monad_compiler_session_destroy(monad_compiler_session_t *s) {
    assert(pthread_equal(s->owner, pthread_self()));
    free(s); ++destroyed;
}
void monad_compiler_source_descriptor_init(monad_compiler_source_descriptor_t *d) {
    memset(d, 0, sizeof(*d));
    d->struct_size = sizeof(*d);
    d->abi_version = MONAD_COMPILER_SOURCE_ABI_VERSION;
}
monad_status_t monad_compiler_source_create(const monad_compiler_source_descriptor_t *d,
    monad_compiler_source_t **out, monad_error_t *error) {
    (void)error;
    assert(d->struct_size == sizeof(*d));
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->text = malloc(d->byte_count + 1);
    assert((*out)->text);
    memcpy((*out)->text, d->bytes, d->byte_count);
    (*out)->text[d->byte_count] = '\0';
    (*out)->count = d->byte_count;
    return MONAD_OK;
}
void monad_compiler_source_destroy(monad_compiler_source_t *s) {
    free(s->text); free(s);
}
monad_status_t monad_compiler_check_source(monad_compiler_session_t *session,
    const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **out, monad_error_t *error) {
    (void)error;
    assert(pthread_equal(session->owner, pthread_self()));
    ++checked;
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->bad = source->count && source->text[0] == '!';
    (*out)->diagnostic.struct_size = sizeof((*out)->diagnostic);
    (*out)->diagnostic.severity = MONAD_COMPILER_DIAGNOSTIC_ERROR;
    (*out)->diagnostic.code = "MONAD-C0002";
    (*out)->diagnostic.message = "rejected by isolated compiler";
    return MONAD_OK;
}
size_t monad_compiler_diagnostic_count(const monad_compiler_diagnostic_set_t *d) {
    return d->bad ? 1u : 0u;
}
const monad_compiler_diagnostic_t *monad_compiler_diagnostic_at(
    const monad_compiler_diagnostic_set_t *d, size_t i) {
    return d->bad && i == 0 ? &d->diagnostic : NULL;
}
void monad_compiler_diagnostic_set_destroy(monad_compiler_diagnostic_set_t *d) { free(d); }

/// Worker-thread test of owned compiler diagnostics
static void *thread_body(void *unused) {
    (void)unused;
    ObsStagePreflightHooks hooks = obs_semantic_preflight_hooks();
    void *session = hooks.begin();
    assert(session);
    ObsSourceSnapshot good = {.source = "valid", .length = 5, .ticket = 7,
        .status = OBS_STAGE_OK};
    strcpy(good.path, "good.mon");
    hooks.check(session, &good);
    assert(good.preflight.status == OBS_PREFLIGHT_ACCEPTED);
    ObsSourceSnapshot bad = {.source = "!bad", .length = 4, .ticket = 8,
        .status = OBS_STAGE_OK};
    strcpy(bad.path, "bad.mon");
    hooks.check(session, &bad);
    assert(bad.preflight.status == OBS_PREFLIGHT_DIAGNOSTIC);
    assert(bad.preflight.diagnostic_count == 1);
    assert(strcmp(bad.preflight.code, "MONAD-C0002") == 0);
    assert(strcmp(bad.preflight.message, "rejected by isolated compiler") == 0);
    hooks.end(session);
    return NULL;
}
int main(void) {
    pthread_t worker;
    assert(pthread_create(&worker, NULL, thread_body, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(created == 1 && destroyed == 1 && checked == 2);
    puts("semantic preflight real-SDK ABI + ownership: PASS (test doubles)");
    return 0;
}
