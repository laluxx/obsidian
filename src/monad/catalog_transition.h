/// catalog_transition.h — Engine-side visibility into native ABI changes
#ifndef OBSIDIAN_MONAD_CATALOG_TRANSITION_H
#define OBSIDIAN_MONAD_CATALOG_TRANSITION_H
#include "export_catalog.h"
#include <stdbool.h>
#include <stddef.h>

/// Transition facts, never QTT or state-migration certificates
/* This report is POST-publication with the current MonadC load API. A future
 * compiler-owned candidate API may use the same comparison before commit,
 * but host observations MUST NOT masquerade as a compiler compatibility proof.
 */
typedef struct ObsCatalogTransition {
    size_t added;
    size_t removed; /* absent from next SOURCE RESULT, not removed from JIT */
    size_t abi_changed;
    size_t abi_unknown;
    size_t proof_changed;
    size_t proof_removed; /* prior nonzero certificate fingerprint -> zero */
    size_t unchanged;
    bool complete;      /* both SOURCE RESULT samples include complete ABIs */
    bool has_previous;  /* comparable source-unit result exists; NOT full-image */
} ObsCatalogTransition;

ObsCatalogTransition obs_catalog_compare(const ObsNativeCatalog *previous,
                                         const ObsNativeCatalog *current);
#endif
