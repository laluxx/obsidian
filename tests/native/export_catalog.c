/// export_catalog.c — Real Monad SDK declarations, deterministic result doubles
#include "export_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct monad_qtt_report {
    size_t count;
    monad_qtt_definition_evidence_t entries[4];
};
struct monad_source_result {
    int valid, published;
    size_t count;
    monad_source_native_export_t entries[80];
    struct monad_qtt_report report;
};
int monad_source_result_is_valid(const monad_source_result_t *p) { return p->valid; }
int monad_source_result_is_published(const monad_source_result_t *p) { return p->published; }
size_t monad_source_result_native_export_count(const monad_source_result_t *p) { return p->count; }
const monad_source_native_export_t *monad_source_result_native_export_at(
    const monad_source_result_t *p, size_t i) { return i < p->count ? &p->entries[i] : NULL; }
const monad_qtt_report_t *monad_source_result_qtt_report(const monad_source_result_t *p) {
    return &p->report;
}
size_t monad_qtt_report_count(const monad_qtt_report_t *p) { return p->count; }
const monad_qtt_definition_evidence_t *monad_qtt_report_at(const monad_qtt_report_t *p,
                                                           size_t i) {
    return i < p->count ? &p->entries[i] : NULL;
}
int main(void) {
    struct monad_source_result r = {0};
    ObsNativeCatalog catalog = {0};
    static char names[80][24];
    monad_abi_type_t params[2] = {MONAD_ABI_I64, MONAD_ABI_F32};
    assert(!obs_export_catalog_capture(&catalog, &r, 1));
    r.valid = r.published = 1;
    r.count = 80;
    for (size_t i = 0; i < r.count; ++i) {
        snprintf(names[i], sizeof(names[i]), "export-%u", (unsigned)i);
        r.entries[i].name = names[i];
        r.entries[i].signature.struct_size = sizeof(monad_abi_signature_t);
        r.entries[i].signature.abi_version = 1;
        r.entries[i].signature.calling_convention = MONAD_ABI_C;
        r.entries[i].signature.parameters = params;
        r.entries[i].signature.result = MONAD_ABI_I64;
        r.entries[i].signature.parameter_count = 2;
        r.entries[i].qtt_certificate_fingerprint = i + 1;
    }
    r.report.count = 4;
    r.report.entries[0].stage = MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED;
    r.report.entries[0].resource_verified = 1;
    r.report.entries[0].destructor_plan_available = 1;
    r.report.entries[0].resource_certificate_fingerprint = 99;
    r.report.entries[1].stage = MONAD_QTT_EVIDENCE_INFERRED;
    r.report.entries[2].stage = MONAD_QTT_EVIDENCE_RESOURCE_VERIFIED;
    r.report.entries[2].resource_verified = 1;
    r.report.entries[2].destructor_plan_available = 0;
    r.report.entries[3] = r.report.entries[0];
    assert(obs_export_catalog_capture(&catalog, &r, 4));
    assert(catalog.publication_epoch == 4);
    assert(catalog.export_count == 80 && catalog.stored_exports == 64 && catalog.truncated);
    assert(catalog.qtt_definitions == 4 && catalog.resource_verified == 2 &&
           catalog.qtt_inferred_only == 2);
    assert(!strcmp(obs_export_catalog_at(&catalog, 63)->name, "export-63"));
    assert(obs_export_catalog_at(&catalog, 63)->signature_complete);
    assert(obs_export_catalog_at(&catalog, 63)->parameters[1] == MONAD_ABI_F32);
    assert(obs_export_catalog_at(&catalog, 64) == NULL);
    /* Source result destruction/mutation cannot change owned data. */
    names[63][0] = 'X';
    params[1] = MONAD_ABI_U8;
    assert(obs_export_catalog_at(&catalog, 63)->parameters[1] == MONAD_ABI_F32);
    assert(!strcmp(obs_export_catalog_at(&catalog, 63)->name, "export-63"));
    assert(obs_export_catalog_at(&catalog, 63)->signature_complete);
    assert(obs_export_catalog_at(&catalog, 63)->parameters[1] == MONAD_ABI_F32);
    r.published = 0;
    assert(!obs_export_catalog_capture(&catalog, &r, 5));
    assert(catalog.publication_epoch == 4);
    r.published = 1;
    r.entries[0].name = NULL;
    assert(!obs_export_catalog_capture(&catalog, &r, 5));
    assert(catalog.publication_epoch == 4);
    r.entries[0].name = names[0];
    r.count = 1;
    r.report.count = 0;
    assert(obs_export_catalog_capture(&catalog, &r, 6));
    assert(catalog.export_count == 1 && catalog.qtt_definitions == 0 &&
           catalog.publication_epoch == 6);
    r.count = 2;
    r.entries[1].name = r.entries[0].name;
    assert(!obs_export_catalog_capture(&catalog, &r, 7));
    assert(catalog.publication_epoch == 6);
    obs_export_catalog_clear(&catalog);
    assert(catalog.export_count == 0 && catalog.publication_epoch == 0);
    puts("PASS: QTT/native catalog copied, bounded, rejected publication preserved");
    return 0;
}
