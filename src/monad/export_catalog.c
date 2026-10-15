/// export_catalog.c — No dangling compiler or ORC-image pointers in UI state
#include "export_catalog.h"
#include <string.h>

/// Snapshot lifecycle
void obs_export_catalog_clear(ObsNativeCatalog *catalog) {
    if (catalog) memset(catalog, 0, sizeof(*catalog));
}

const ObsNativeExportSummary *obs_export_catalog_at(const ObsNativeCatalog *catalog,
                                                    size_t index) {
    if (!catalog || index >= catalog->stored_exports) return NULL;
    return &catalog->exports[index];
}

/// Compiler-owned certificate capture
bool obs_export_catalog_capture(ObsNativeCatalog *catalog,
                                const monad_source_result_t *published,
                                uint64_t publication_epoch) {
    if (!catalog || !published || !publication_epoch ||
        !monad_source_result_is_valid(published) ||
        !monad_source_result_is_published(published)) return false;

    /* Build privately, then publish the complete snapshot at the owner-thread
     * safe point. No borrowed SDK pointer is stored in engine memory. */
    ObsNativeCatalog next = {0};
    next.publication_epoch = publication_epoch;
    next.export_count = monad_source_result_native_export_count(published);
    next.stored_exports = next.export_count < OBS_EXPORT_CATALOG_CAP
        ? next.export_count : OBS_EXPORT_CATALOG_CAP;
    next.truncated = next.stored_exports != next.export_count;
    for (size_t i = 0; i < next.stored_exports; ++i) {
        const monad_source_native_export_t *entry =
            monad_source_result_native_export_at(published, i);
        if (!entry || !entry->name) return false;
        ObsNativeExportSummary *out = &next.exports[i];
        size_t len = 0;
        while (len < OBS_EXPORT_NAME_CAP && entry->name[len]) ++len;
        out->truncated_name = len >= OBS_EXPORT_NAME_CAP;
        if (out->truncated_name) {
            len = OBS_EXPORT_NAME_CAP - 1;
            next.truncated = true;
        }
        memcpy(out->name, entry->name, len);
        out->name[len] = '\0';
        for (size_t j = 0; j < i; ++j)
            if (strcmp(out->name, next.exports[j].name) == 0)
                return false;
        out->result_type = entry->signature.result;
        out->parameter_count = entry->signature.parameter_count;
        out->abi_version = entry->signature.abi_version;
        out->calling_convention = entry->signature.calling_convention;
        out->signature_complete = entry->signature.struct_size >= sizeof(monad_abi_signature_t) &&
            entry->signature.abi_version != 0 &&
            entry->signature.calling_convention == MONAD_ABI_C &&
            out->parameter_count <= OBS_EXPORT_PARAMETERS_CAP &&
            (!out->parameter_count || entry->signature.parameters);
        if (out->signature_complete && out->parameter_count)
            memcpy(out->parameters, entry->signature.parameters,
                   out->parameter_count * sizeof(out->parameters[0]));
        if (!out->signature_complete || out->truncated_name)
            next.truncated = true;
        out->qtt_fingerprint = entry->qtt_certificate_fingerprint;
    }
    const monad_qtt_report_t *report = monad_source_result_qtt_report(published);
    if (report) {
        next.qtt_definitions = monad_qtt_report_count(report);
        for (size_t i = 0; i < next.qtt_definitions; ++i) {
            const monad_qtt_definition_evidence_t *e = monad_qtt_report_at(report, i);
            if (!e) return false;
            if (e->stage == MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED &&
                e->resource_verified && e->destructor_plan_available &&
                e->resource_certificate_fingerprint)
                ++next.resource_verified;
            else
                ++next.qtt_inferred_only;
        }
    }
    *catalog = next;
    return true;
}
