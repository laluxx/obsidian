/// catalog_transition.c — Bounded, structural export comparison
#include "catalog_transition.h"
#include <string.h>

/// Exact C-call ABI equality (not a QTT proof)
static bool same_abi(const ObsNativeExportSummary *a,
                     const ObsNativeExportSummary *b) {
    if (!a->signature_complete || !b->signature_complete) return false;
    if (a->abi_version != b->abi_version ||
        a->calling_convention != b->calling_convention ||
        a->result_type != b->result_type ||
        a->parameter_count != b->parameter_count) return false;
    for (size_t i = 0; i < a->parameter_count; ++i)
        if (a->parameters[i] != b->parameters[i]) return false;
    return true;
}

static const ObsNativeExportSummary *find_export(const ObsNativeCatalog *c,
                                                  const char *name) {
    for (size_t i = 0; i < c->stored_exports; ++i)
        if (strcmp(c->exports[i].name, name) == 0) return &c->exports[i];
    return NULL;
}

ObsCatalogTransition obs_catalog_compare(const ObsNativeCatalog *previous,
                                         const ObsNativeCatalog *current) {
    ObsCatalogTransition delta = {0};
    if (!current || !current->publication_epoch) return delta;
    delta.complete = !current->truncated;
    for (size_t i = 0; i < current->stored_exports; ++i)
        if (!current->exports[i].signature_complete ||
            current->exports[i].truncated_name) delta.complete = false;
    if (!previous || !previous->publication_epoch ||
        !current->source_unit_complete || !previous->source_unit_complete ||
        strcmp(current->source_unit, previous->source_unit) != 0)
        return delta;
    delta.has_previous = true;
    if (previous->truncated) delta.complete = false;
    for (size_t i = 0; i < previous->stored_exports; ++i) {
        const ObsNativeExportSummary *before = &previous->exports[i];
        if (!before->signature_complete || before->truncated_name)
            delta.complete = false;
        const ObsNativeExportSummary *after = find_export(current, before->name);
        if (!after) {
            ++delta.removed;
            continue;
        }
        if (!before->signature_complete || !after->signature_complete ||
            before->truncated_name || after->truncated_name) {
            ++delta.abi_unknown;
            delta.complete = false;
        } else if (!same_abi(before, after)) ++delta.abi_changed;
        else ++delta.unchanged;
        if (before->qtt_fingerprint != after->qtt_fingerprint) ++delta.proof_changed;
        if (before->qtt_fingerprint && !after->qtt_fingerprint) ++delta.proof_removed;
    }
    for (size_t i = 0; i < current->stored_exports; ++i)
        if (!find_export(previous, current->exports[i].name)) ++delta.added;
    /* A truncated snapshot makes missing-name counts lower bounds, never
     * definitive removal/addition claims. Callers must inspect complete. */
    return delta;
}
