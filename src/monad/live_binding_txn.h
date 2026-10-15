/// live_binding_txn.h — Transactional, image-scoped host callback registrations
#ifndef OBSIDIAN_MONAD_LIVE_BINDING_TXN_H
#define OBSIDIAN_MONAD_LIVE_BINDING_TXN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Bounded publication journal
/* Pure host adapter; keeps compiler-owned callback values alive through the
 * compiler publication decision. It does NOT substitute for a QTT proof or
 * atomic native-image publication. No host mutation occurs during staging. */
#define OBS_LIVE_TXN_CAP 128u
#define OBS_LIVE_KEY_CAP 96u

typedef enum ObsLiveAction {
    OBS_LIVE_KEY_BIND = 1,
    OBS_LIVE_KEY_UNBIND,
    OBS_LIVE_FRAME_BIND
} ObsLiveAction;

typedef struct ObsLiveOp {
    ObsLiveAction action;
    char key[OBS_LIVE_KEY_CAP];
    void *value; /* retained until finish/abort */
} ObsLiveOp;

typedef void (*ObsLiveRetain)(void *);
typedef void (*ObsLiveRelease)(void *);
typedef bool (*ObsLiveApply)(const ObsLiveOp *, void *);
typedef bool (*ObsLiveValidate)(const ObsLiveOp *, void *);
typedef void (*ObsLiveBeforeCommit)(void *);

typedef struct ObsLiveBindingTxn {
    ObsLiveOp operations[OBS_LIVE_TXN_CAP];
    size_t count;
    bool active;
    ObsLiveRetain retain;
    ObsLiveRelease release;
    uint64_t rejected;
    uint64_t committed;
    uint64_t application_failures;
    uint64_t preflight_failures;
} ObsLiveBindingTxn;

/// Transaction lifecycle
bool obs_live_txn_begin(ObsLiveBindingTxn *, ObsLiveRetain, ObsLiveRelease);
bool obs_live_txn_append(ObsLiveBindingTxn *, ObsLiveAction,
                         const char *key, void *value);
/* apply() may fail due to keymap allocation; journal ownership is still
 * released exactly once. The native compiler's published image is NOT rolled
 * back by such a failure. The engine must report degraded host installation. */
bool obs_live_txn_finish(ObsLiveBindingTxn *, bool published,
                         ObsLiveApply apply, void *context);
/* Check all operations before touching the existing host callback state.
 * Validation is advisory with respect to concurrent/non-journal allocation:
 * the engine owner thread must remain the exclusive mutator during commit. */
bool obs_live_txn_preflight(const ObsLiveBindingTxn *, ObsLiveValidate,
                            void *context);
/* The before_commit hook runs only if every operation has passed validation.
 * Application can still fail (e.g. an allocator); that must be reported and
 * never misrepresented as atomic compiler+host publication. */
bool obs_live_txn_finish_checked(ObsLiveBindingTxn *, bool published,
    ObsLiveValidate, ObsLiveBeforeCommit, ObsLiveApply, void *context);
/* Two-stage host publication: all allocations and validation happen in
 * prepare; commit is declared infallible and MUST NOT allocate. The caller
 * owns the prepared state and must dispose it after either result. */
typedef bool (*ObsLivePrepare)(const ObsLiveBindingTxn *, void *);
typedef void (*ObsLiveCommit)(const ObsLiveBindingTxn *, void *);
bool obs_live_txn_finish_prepared(ObsLiveBindingTxn *, bool published,
    ObsLivePrepare prepare, ObsLiveCommit commit, void *context);
void obs_live_txn_abort(ObsLiveBindingTxn *);
#endif
