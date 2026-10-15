/// semantic_preflight.c — Off-thread, compiler-owned source preflight
#define _POSIX_C_SOURCE 200809L
#include "semantic_preflight.h"
#include <monad/compiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// Thread-affine compiler session
/* Allocated, used and destroyed on the same source-stage worker thread.
 * This is NOT a candidate compilation: check_source has no native image. */
static void *preflight_begin(void) {
    if (monad_compiler_abi_version() != MONAD_COMPILER_ABI_VERSION)
        return NULL;
    monad_compiler_session_t *session = NULL;
    monad_error_t error = {0};
    if (monad_compiler_session_create(&session, &error) != MONAD_OK)
        return NULL;
    return session;
}

static void preflight_end(void *context) {
    monad_compiler_session_destroy((monad_compiler_session_t *)context);
}

/// Bounded diagnostics independent of compiler result lifetimes
static uint64_t elapsed_us(struct timespec from, struct timespec to) {
    if (to.tv_sec < from.tv_sec ||
        (to.tv_sec == from.tv_sec && to.tv_nsec < from.tv_nsec))
        return 0;
    uint64_t sec = (uint64_t)(to.tv_sec - from.tv_sec);
    int64_t nanos = (int64_t)to.tv_nsec - (int64_t)from.tv_nsec;
    if (nanos < 0) { --sec; nanos += 1000000000; }
    if (sec > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    return sec * UINT64_C(1000000) + (uint64_t)nanos / 1000;
}

static void preflight_check(void *context, ObsSourceSnapshot *snapshot) {
    if (!context || !snapshot || snapshot->status != OBS_STAGE_OK ||
        !snapshot->source) return;
    ObsPreflightReport *report = &snapshot->preflight;
    report->status = OBS_PREFLIGHT_UNAVAILABLE;
    struct timespec start = {0}, end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = snapshot->path;
    descriptor.bytes = snapshot->source;
    descriptor.byte_count = snapshot->length;
    descriptor.version = snapshot->ticket;
    monad_compiler_source_t *source = NULL;
    monad_compiler_diagnostic_set_t *diagnostics = NULL;
    monad_error_t error = {0};
    if (monad_compiler_source_create(&descriptor, &source, &error) == MONAD_OK &&
        source) {
        monad_status_t status = monad_compiler_check_source(
            (monad_compiler_session_t *)context, source, &diagnostics, &error);
        if (status == MONAD_OK) {
            report->status = OBS_PREFLIGHT_ACCEPTED;
            size_t count = monad_compiler_diagnostic_count(diagnostics);
            report->diagnostic_count = (uint32_t)(count > UINT32_MAX ? UINT32_MAX : count);
            for (size_t i = 0; i < count; ++i) {
                const monad_compiler_diagnostic_t *d =
                    monad_compiler_diagnostic_at(diagnostics, i);
                if (!d) continue;
                /* Preserve the first error, or the first available warning.
                 * Diagnostics are not retained across source-result destruction. */
                if (d->severity == MONAD_COMPILER_DIAGNOSTIC_ERROR ||
                    (i == 0 && d->message)) {
                    if (d->severity == MONAD_COMPILER_DIAGNOSTIC_ERROR)
                        report->status = OBS_PREFLIGHT_DIAGNOSTIC;
                    snprintf(report->code, sizeof(report->code), "%s",
                             d->code ? d->code : "MONAD-C");
                    snprintf(report->message, sizeof(report->message), "%s",
                             d->message ? d->message : "compiler diagnostic");
                    if (d->severity == MONAD_COMPILER_DIAGNOSTIC_ERROR) break;
                }
            }
        }
    }
    if (report->status == OBS_PREFLIGHT_UNAVAILABLE)
        snprintf(report->message, sizeof(report->message), "%s",
                 error.message ? error.message : "compiler preflight unavailable");
    if (diagnostics) monad_compiler_diagnostic_set_destroy(diagnostics);
    if (source) monad_compiler_source_destroy(source);
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    report->elapsed_us = elapsed_us(start, end);
}

ObsStagePreflightHooks obs_semantic_preflight_hooks(void) {
    return (ObsStagePreflightHooks){preflight_begin, preflight_check, preflight_end};
}
