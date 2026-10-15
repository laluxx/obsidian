/// catalog_transition.c — Structural native ABI regression and uncertainty tests
#include "catalog_transition.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Self-contained fake certified catalog (NOT a MonadC compiler proof)
static ObsNativeCatalog make_catalog(uint64_t epoch) {
    ObsNativeCatalog c = {0};
    c.publication_epoch = epoch;
    strcpy(c.source_unit, "obsidian://repl");
    c.source_unit_complete = true;
    c.export_count = c.stored_exports = 2;
    ObsNativeExportSummary *a = &c.exports[0];
    strcpy(a->name, "move");
    a->signature_complete = true;
    a->abi_version = 1;
    a->calling_convention = MONAD_ABI_C;
    a->result_type = MONAD_ABI_I64;
    a->parameter_count = 2;
    a->parameters[0] = MONAD_ABI_I64;
    a->parameters[1] = MONAD_ABI_F32;
    a->qtt_fingerprint = 131;
    ObsNativeExportSummary *b = &c.exports[1];
    *b = *a;
    strcpy(b->name, "render");
    b->qtt_fingerprint = 456;
    return c;
}

int main(void) {
    ObsNativeCatalog before = make_catalog(1);
    ObsNativeCatalog after = make_catalog(2);
    ObsCatalogTransition d = obs_catalog_compare(NULL, &after);
    assert(!d.has_previous && d.complete);
    d = obs_catalog_compare(&before, &after);
    assert(d.has_previous && d.complete && d.unchanged == 2);
    assert(!d.added && !d.removed && !d.abi_changed && !d.proof_changed);
    after.exports[0].parameters[1] = MONAD_ABI_F64;
    d = obs_catalog_compare(&before, &after);
    assert(d.abi_changed == 1 && d.unchanged == 1 && d.complete);
    after.exports[0].parameters[1] = MONAD_ABI_F32;
    after.exports[0].qtt_fingerprint = 0;
    d = obs_catalog_compare(&before, &after);
    assert(!d.abi_changed && d.proof_changed == 1 && d.proof_removed == 1);
    after = make_catalog(3);
    strcpy(after.exports[1].name, "new-object");
    d = obs_catalog_compare(&before, &after);
    assert(d.added == 1 && d.removed == 1 && d.unchanged == 1);
    after = make_catalog(4);
    after.exports[0].signature_complete = false;
    d = obs_catalog_compare(&before, &after);
    assert(!d.complete && d.abi_changed == 0 && d.abi_unknown == 1);
    after = make_catalog(5);
    after.truncated = true;
    d = obs_catalog_compare(&before, &after);
    assert(!d.complete);
    before = make_catalog(6);
    before.truncated = true;
    after = make_catalog(7);
    d = obs_catalog_compare(&before, &after);
    assert(!d.complete);
    /* Irrespective of permutation, identical distinct exports remain equal. */
    before = make_catalog(8);
    after = make_catalog(9);
    strcpy(after.source_unit, "obsidian://boot");
    d = obs_catalog_compare(&before, &after);
    assert(!d.has_previous && !d.abi_changed && !d.removed);
    after = make_catalog(9);
    after.source_unit_complete = false;
    d = obs_catalog_compare(&before, &after);
    assert(!d.has_previous && !d.abi_changed);

    for (unsigned i = 0; i < 50000; ++i) {
        after = make_catalog(9);
        if (i & 1u) {
            ObsNativeExportSummary tmp = after.exports[0];
            after.exports[0] = after.exports[1]; after.exports[1] = tmp;
        }
        if (i % 29u == 0) after.exports[1].parameters[0] = MONAD_ABI_U32;
        d = obs_catalog_compare(&before, &after);
        assert(d.complete && d.abi_changed == (i % 29u == 0));
    }
    puts("PASS: 50,000 ABI transition comparisons, proof drift, incomplete catalog");
    return 0;
}
