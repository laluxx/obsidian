/// repl_service.c — Compiler-backed REPL prepare, QTT evidence, and publication
#include "repl_service.h"
#include "dispatch_metrics.h"
#include <monad/qtt.h>
#include <stdio.h>
#include <string.h>

/// Persistent model (one engine owner)
static ObsReplModel s_repl;
static bool s_repl_initialized;
ObsReplModel *obs_monad_repl_model(void) {
    if (!s_repl_initialized) {
        obs_repl_model_init(&s_repl);
        obs_repl_model_line(&s_repl, OBS_REPL_INFO,
            "Monad native REPL | :check | :images | :state | :help | C-c r");
        s_repl_initialized = true;
    }
    return &s_repl;
}
void obs_monad_repl_reset(void) {
    obs_repl_model_init(&s_repl);
    s_repl_initialized = true;
}

/// Error and proof reporting
static void print_diagnostics(ObsReplModel *model,
                              const monad_source_result_t *result) {
    if (!result) return;
    size_t n = monad_source_result_diagnostic_count(result);
    for (size_t i = 0; i < n && i < 20; ++i) {
        const char *code = monad_source_result_diagnostic_code(result, i);
        const char *message = monad_source_result_diagnostic_message(result, i);
        char line[OBS_REPL_LINE_CAP];
        snprintf(line, sizeof(line), "%s: %s", code ? code : "Monad",
                 message ? message : "no diagnostic text");
        obs_repl_model_line(model, OBS_REPL_ERROR, line);
    }
    if (n > 20) obs_repl_model_line(model, OBS_REPL_INFO,
                                   "Additional diagnostics omitted from bounded transcript");
}
static void report_qtt(ObsReplModel *model, const monad_source_result_t *result) {
    const monad_qtt_report_t *report = monad_source_result_qtt_report(result);
    if (!report) return;
    size_t inferred = 0, verified = 0;
    for (size_t i = 0, n = monad_qtt_report_count(report); i < n; ++i) {
        const monad_qtt_definition_evidence_t *e = monad_qtt_report_at(report, i);
        if (!e) continue;
        ++inferred;
        if (e->stage == MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED &&
            e->resource_verified && e->destructor_plan_available &&
            e->resource_certificate_fingerprint)
            ++verified;
    }
    char summary[OBS_REPL_LINE_CAP];
    snprintf(summary, sizeof(summary), "QTT evidence: %zu inferred, %zu resource-verified",
             inferred, verified);
    obs_repl_model_line(model, OBS_REPL_INFO, summary);
}

/// Owner-thread compilation checkpoint
void obs_monad_repl_pump(ObsMonadRuntime *host) {
    ObsReplModel *model = obs_monad_repl_model();
    if (!host || !host->active || !obs_monad_on_owner_thread() ||
        host->publication.active_calls ||
        host->publication.phase == OBS_LIVE_LOADING ||
        !obs_repl_model_begin(model)) return;

    const char *source = model->pending;
    if (strcmp(source, ":help") == 0) {
        obs_repl_model_line(model, OBS_REPL_INFO,
            "Submit Monad definitions; :check <source> verifies without publishing");
        obs_repl_model_line(model, OBS_REPL_INFO,
            "Commands: :help :clear :images :state :queue :latency :dispatch :preflight :cancel-boot :exports :qtt :diff");
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":clear") == 0) {
        /* Clear after submission, never discard a pending compiler command. */
        model->count = model->head = 0;
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":queue") == 0) {
        const ObsPublishQueue *q = &host->requests;
        char message[OBS_REPL_LINE_CAP];
        snprintf(message, sizeof(message),
            "Pending=%zu, busy=%u, started=%llu, completed=%llu, coalesced=%llu, cancelled=%llu",
            obs_publish_queue_pending(q), (unsigned)q->busy,
            (unsigned long long)q->started,
            (unsigned long long)q->completed,
            (unsigned long long)q->coalesced,
            (unsigned long long)q->cancelled);
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":latency") == 0) {
        ObsPublishMetrics metrics = {0};
        obs_monad_engine_publish_metrics(&metrics);
        char message[OBS_REPL_LINE_CAP];
        snprintf(message, sizeof(message),
            "Checkpoint CPU last=%llu us, p95=%llu us, p99=%llu us; budget=%llu us, over=%llu/%llu",
            (unsigned long long)metrics.all.last_duration_us,
            (unsigned long long)obs_publish_latency_percentile(&metrics.all, 95, false),
            (unsigned long long)obs_publish_latency_percentile(&metrics.all, 99, false),
            (unsigned long long)metrics.budget_us,
            (unsigned long long)metrics.all.over_budget,
            (unsigned long long)metrics.all.attempts);
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        snprintf(message, sizeof(message),
            "Queue wait p95=%llu us, boot CPU p95=%llu us, REPL CPU p95=%llu us",
            (unsigned long long)obs_publish_latency_percentile(&metrics.all, 95, true),
            (unsigned long long)obs_publish_latency_percentile(&metrics.lane[OBS_PUBLISH_BOOT], 95, false),
            (unsigned long long)obs_publish_latency_percentile(&metrics.lane[OBS_PUBLISH_REPL], 95, false));
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        obs_repl_model_line(model, OBS_REPL_INFO,
            "CPU checkpoint timing only; GPU p95/p99 and nonblocking native compilation remain unverified");
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":dispatch") == 0) {
        ObsDispatchMetrics metrics = {0};
        obs_dispatch_metrics_snapshot(&metrics);
        if (!metrics.enabled) {
            obs_repl_model_line(model, OBS_REPL_INFO,
                "CPU dispatch profiler is off; use OBSIDIAN_MONAD_PROFILE_FFI=1");
        } else {
            static const char *labels[] = {"frame", "camera"};
            for (unsigned i = 0; i < OBS_DISPATCH_LANE_COUNT; ++i) {
                const ObsDispatchLaneStats *lane = &metrics.lanes[i];
                char line[OBS_REPL_LINE_CAP];
                snprintf(line, sizeof(line),
                    "%s callbacks=%llu failures=%llu total-p95<=%llu ns guest-p95<=%llu ns max=%llu ns",
                    labels[i],
                    (unsigned long long)lane->phases[OBS_DISPATCH_TOTAL].samples,
                    (unsigned long long)lane->failures,
                    (unsigned long long)obs_dispatch_histogram_percentile_ns(
                        &lane->phases[OBS_DISPATCH_TOTAL], 95),
                    (unsigned long long)obs_dispatch_histogram_percentile_ns(
                        &lane->phases[OBS_DISPATCH_EXECUTE], 95),
                    (unsigned long long)lane->phases[OBS_DISPATCH_TOTAL].max_ns);
                obs_repl_model_line(model, OBS_REPL_INFO, line);
            }
            obs_repl_model_line(model, OBS_REPL_INFO,
                "Log2 bucket p95 upper bounds; CPU-only, not GPU or independent native compilation");
        }
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":preflight") == 0) {
        ObsPreflightReport report = {0};
        obs_monad_engine_preflight_report(&report);
        char message[OBS_REPL_LINE_CAP];
        snprintf(message, sizeof(message),
            "Advisory compiler preflight status=%u diagnostics=%u elapsed=%llu us",
            (unsigned)report.status, (unsigned)report.diagnostic_count,
            (unsigned long long)report.elapsed_us);
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        if (report.message[0]) obs_repl_model_line(model, OBS_REPL_INFO,
                                                    report.message);
        obs_repl_model_line(model, OBS_REPL_INFO,
            "Separate compiler session only; MonadC's live publication is authoritative");
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":cancel-boot") == 0) {
        const bool cancelled = obs_publish_queue_cancel(&host->requests,
                                                         OBS_PUBLISH_BOOT);
        obs_repl_model_line(model, OBS_REPL_INFO, cancelled
            ? "Cancelled queued module reload (active compilation cannot be cancelled)"
            : "No queued module reload to cancel");
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":images") == 0) {
        char message[OBS_REPL_LINE_CAP];
        snprintf(message, sizeof(message),
                 "Compiler-retained native image count: %zu",
                 monad_live_native_image_count(host->runtime));
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":state") == 0) {
        char message[OBS_REPL_LINE_CAP];
        snprintf(message, sizeof(message),
                 "Host epoch=%llu, published=%llu, rejected=%llu, active=%u",
                 (unsigned long long)host->publication.epoch,
                 (unsigned long long)host->publication.successes,
                 (unsigned long long)host->publication.failures,
                 host->publication.active_calls);
        obs_repl_model_line(model, OBS_REPL_INFO, message);
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":exports") == 0) {
        const ObsNativeCatalog *catalog = &host->last_publication;
        char line[OBS_REPL_LINE_CAP];
        snprintf(line, sizeof(line), "Latest publication epoch=%llu: %zu native exports (%zu shown)%s",
            (unsigned long long)catalog->publication_epoch, catalog->export_count,
            catalog->stored_exports, catalog->truncated ? " [bounded]" : "");
        obs_repl_model_line(model, OBS_REPL_INFO, line);
        for (size_t i = 0; i < catalog->stored_exports; ++i) {
            const ObsNativeExportSummary *e = obs_export_catalog_at(catalog, i);
            snprintf(line, sizeof(line), "%s (%zu args, ABI result %u, QTT cert %s)",
                e->name, e->parameter_count, (unsigned)e->result_type,
                e->qtt_fingerprint ? "yes" : "none");
            obs_repl_model_line(model, OBS_REPL_INFO, line);
        }
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":diff") == 0) {
        const ObsCatalogTransition *d = &host->last_transition;
        char line[OBS_REPL_LINE_CAP];
        if (!host->last_publication.publication_epoch)
            obs_repl_model_line(model, OBS_REPL_INFO,
                "No complete compiler publication snapshot available");
        else if (!d->has_previous)
            obs_repl_model_line(model, OBS_REPL_INFO,
                "No comparable previous publication for this source unit");
        else {
            snprintf(line, sizeof(line),
                "Native exports: +%zu -%zu ABI changed=%zu unknown=%zu, QTT drift=%zu, lost cert fingerprints=%zu%s",
                d->added, d->removed, d->abi_changed, d->abi_unknown, d->proof_changed,
                d->proof_removed, d->complete ? "" : " [PARTIAL / INCONCLUSIVE]");
            obs_repl_model_line(model, OBS_REPL_INFO, line);
            obs_repl_model_line(model, OBS_REPL_INFO,
                "Per-source-result observations only (NOT the whole image); state migration is NOT proven");
        }
        obs_repl_model_finish(model, true);
        return;
    }
    if (strcmp(source, ":qtt") == 0) {
        const ObsNativeCatalog *catalog = &host->last_publication;
        char line[OBS_REPL_LINE_CAP];
        snprintf(line, sizeof(line),
            "Last published source: %zu QTT definitions, %zu resource-verified, %zu inferred-only",
            catalog->qtt_definitions, catalog->resource_verified,
            catalog->qtt_inferred_only);
        obs_repl_model_line(model, OBS_REPL_INFO, line);
        obs_repl_model_finish(model, true);
        return;
    }
    bool check_only = strncmp(source, ":check ", 7) == 0;
    if (check_only) source += 7;
    if (!*source) {
        obs_repl_model_line(model, OBS_REPL_ERROR, "Empty Monad source");
        obs_repl_model_finish(model, false);
        return;
    }
    monad_error_t error = {0};
    if (check_only) {
        monad_source_result_t *prepared = NULL;
        const monad_status_t preflight = monad_prepare_source(host->runtime,
            "obsidian://repl", source, strlen(source), &prepared, &error);
        const bool acceptable = preflight == MONAD_OK && prepared &&
                                monad_source_result_is_valid(prepared);
        if (!acceptable) {
            print_diagnostics(model, prepared);
            obs_repl_model_line(model, OBS_REPL_ERROR,
                error.message ? error.message : "Source preparation rejected");
        } else {
            char summary[OBS_REPL_LINE_CAP];
            snprintf(summary, sizeof(summary),
                "Checked %zu definitions (semantic preparation only; not QTT-certified publication)",
                monad_source_result_definition_count(prepared));
            obs_repl_model_line(model, OBS_REPL_SUCCESS, summary);
            report_qtt(model, prepared);
        }
        if (prepared) monad_source_result_destroy(prepared);
        obs_repl_model_finish(model, acceptable);
        return;
    }
    /* MonadC load_source already parses, infers, QTT-certifies and publishes.
     * Do not prepare a second time: that doubled synchronous compiler work and
     * reported a possibly different snapshot from the final transaction. */
    /* Existing engine publication/closure gate is authoritative. No second VM. */
    monad_source_result_t *published = NULL;
    bool ok = obs_monad_load_source(host, "obsidian://repl", source,
                                    &published, &error);
    if (!ok) {
        print_diagnostics(model, published);
        obs_repl_model_line(model, OBS_REPL_ERROR,
            error.message ? error.message : "Monad rejected publication");
    } else {
        char summary[OBS_REPL_LINE_CAP];
        snprintf(summary, sizeof(summary), "Published %zu definitions, %zu certified native exports",
                 monad_source_result_definition_count(published),
                 monad_source_result_native_export_count(published));
        obs_repl_model_line(model, OBS_REPL_SUCCESS, summary);
        report_qtt(model, published);
    }
    if (published) monad_source_result_destroy(published);
    obs_repl_model_finish(model, ok);
}
