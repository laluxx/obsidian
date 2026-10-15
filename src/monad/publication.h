/// publication.h — QTT-aware publication policy and live-code leases
#ifndef OBSIDIAN_MONAD_PUBLICATION_H
#define OBSIDIAN_MONAD_PUBLICATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Compiler-authored identity model
/* These descriptors represent *verified compiler outputs*, not declarations
 * parsed from script text. The current Monad embedding SDK cannot supply
 * them yet. In particular a C string is never proof of a QTT theorem. */
typedef enum {
    OBS_QUANTITY_ERASED = 0,
    OBS_QUANTITY_LINEAR = 1,
    OBS_QUANTITY_UNRESTRICTED = 2
} ObsQuantity;
typedef enum {
    OBS_LIFETIME_VALUE = 0,
    OBS_LIFETIME_FRAME = 1,
    OBS_LIFETIME_ENGINE = 2
} ObsLifetime;
enum {
    OBS_EFFECT_READ = 1u << 0,
    OBS_EFFECT_WRITE = 1u << 1,
    OBS_EFFECT_GPU = 1u << 2,
    OBS_EFFECT_AUDIO = 1u << 3,
    OBS_EFFECT_IO = 1u << 4,
    OBS_EFFECT_ALL = (1u << 5) - 1
};
typedef struct {
    const char *name;
    uint64_t type_identity; /* Compiler-supplied structural type identity. */
    uint32_t effects;
    ObsQuantity quantity;
    ObsLifetime lifetime;
} ObsExportContract;
typedef struct {
    const ObsExportContract *exports;
    size_t export_count;
    uint64_t state_schema; /* Stable type/layout identity for persistent state. */
    uint32_t schema_version;
    bool has_verified_migration; /* Set only after compiler verifies migration. */
    bool compiler_verified;
} ObsImageContract;
typedef enum {
    OBS_CONTRACT_OK = 0,
    OBS_CONTRACT_UNVERIFIED,
    OBS_CONTRACT_INVALID,
    OBS_CONTRACT_MISSING_EXPORT,
    OBS_CONTRACT_TYPE_MISMATCH,
    OBS_CONTRACT_QUANTITY_MISMATCH,
    OBS_CONTRACT_LIFETIME_MISMATCH,
    OBS_CONTRACT_EFFECT_ESCALATION,
    OBS_CONTRACT_STATE_MIGRATION_REQUIRED
} ObsContractResult;
ObsContractResult obs_publication_validate(const ObsImageContract *old_image,
                                           const ObsImageContract *candidate);

/// Host-side execution phase and leases
/* Only the engine owner thread mutates this state. A lease records the code
 * image's epoch and prevents attempting an in-place mutation while script
 * calls are active. It does NOT pin JIT allocations or guarantee rollback. */
typedef enum {
    OBS_LIVE_EMPTY = 0,
    OBS_LIVE_READY,
    OBS_LIVE_LOADING,
    OBS_LIVE_POISONED
} ObsLivePhase;
typedef struct {
    uint64_t epoch;
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    unsigned active_calls;
    ObsLivePhase phase;
    ObsLivePhase phase_before_atomic_load;
    bool atomic_loading;
} ObsPublication;
typedef struct {
    uint64_t epoch;
    bool active;
} ObsCodeLease;
void obs_publication_reset(ObsPublication *state);
bool obs_publication_begin_call(ObsPublication *state, ObsCodeLease *lease);
bool obs_publication_end_call(ObsPublication *state, ObsCodeLease *lease);
bool obs_publication_begin_legacy_load(ObsPublication *state);
/* For an SDK that guarantees rejected source publishes nothing. */
bool obs_publication_begin_atomic_load(ObsPublication *state);
void obs_publication_finish_atomic_load(ObsPublication *state, bool published);
void obs_publication_finish_legacy_load(ObsPublication *state, bool success);
bool obs_publication_can_retire(const ObsPublication *state);
/* A compiler-source load is NOT a transaction for external scene/GPU edits.
 * Runtime mutators require a fully published and usable native image. */
bool obs_publication_mutations_allowed(const ObsPublication *state);
#endif
