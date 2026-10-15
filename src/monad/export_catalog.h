/// export_catalog.h — Owned snapshots of compiler-authored native/QTT evidence
#ifndef OBSIDIAN_MONAD_EXPORT_CATALOG_H
#define OBSIDIAN_MONAD_EXPORT_CATALOG_H
#include <monad/monad.h>
#include <monad/qtt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Limits — never retain borrowed compiler-result pointers
#define OBS_EXPORT_CATALOG_CAP 64u
#define OBS_EXPORT_NAME_CAP 96u
#define OBS_EXPORT_UNIT_CAP 192u
#define OBS_EXPORT_PARAMETERS_CAP 16u

typedef struct ObsNativeExportSummary {
    char name[OBS_EXPORT_NAME_CAP];
    monad_abi_type_t result_type;
    size_t parameter_count;
    uint32_t abi_version;
    monad_abi_calling_convention_t calling_convention;
    monad_abi_type_t parameters[OBS_EXPORT_PARAMETERS_CAP];
    bool signature_complete;
    uint64_t qtt_fingerprint;
    bool truncated_name;
} ObsNativeExportSummary;

typedef struct ObsNativeCatalog {
    uint64_t publication_epoch;
    char source_unit[OBS_EXPORT_UNIT_CAP];
    bool source_unit_complete; /* source unit, not a whole image snapshot */
    size_t export_count;           /* total certified native exports */
    size_t stored_exports;         /* bounded, copied sample */
    size_t qtt_definitions;        /* evidence, including inferred only */
    size_t resource_verified;      /* certified resource/destructor evidence */
    size_t qtt_inferred_only;
    bool truncated;
    ObsNativeExportSummary exports[OBS_EXPORT_CATALOG_CAP];
} ObsNativeCatalog;

void obs_export_catalog_clear(ObsNativeCatalog *catalog);
/* Only call after an actual successful, published MonadC transaction.
 * A rejected load must leave this snapshot of the previous publication intact. */
bool obs_export_catalog_capture(ObsNativeCatalog *catalog,
                                const monad_source_result_t *published,
                                uint64_t publication_epoch);
const ObsNativeExportSummary *obs_export_catalog_at(const ObsNativeCatalog *catalog,
                                                    size_t index);
#endif
