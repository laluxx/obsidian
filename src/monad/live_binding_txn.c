#define _POSIX_C_SOURCE 200809L
/// live_binding_txn.c — Two-phase live host binding journal
#include "live_binding_txn.h"
#include <string.h>

/// Validation and staging
bool obs_live_txn_begin(ObsLiveBindingTxn *tx,
                        ObsLiveRetain retain, ObsLiveRelease release) {
    if (!tx || tx->active || !retain || !release) return false;
    tx->count = 0;
    tx->retain = retain;
    tx->release = release;
    tx->active = true;
    return true;
}

bool obs_live_txn_append(ObsLiveBindingTxn *tx, ObsLiveAction action,
                         const char *key, void *value) {
    if (!tx || !tx->active || tx->count >= OBS_LIVE_TXN_CAP ||
        (action != OBS_LIVE_KEY_BIND && action != OBS_LIVE_KEY_UNBIND &&
         action != OBS_LIVE_FRAME_BIND)) return false;
    if ((action == OBS_LIVE_KEY_BIND || action == OBS_LIVE_FRAME_BIND) !=
        (value != NULL)) return false;
    size_t length = 0;
    if (action == OBS_LIVE_KEY_BIND || action == OBS_LIVE_KEY_UNBIND) {
        if (!key) return false;
        length = strnlen(key, OBS_LIVE_KEY_CAP);
        if (!length || length == OBS_LIVE_KEY_CAP) return false;
    } else if (key) return false;
    ObsLiveOp *op = &tx->operations[tx->count];
    *op = (ObsLiveOp){.action = action, .value = value};
    if (length) memcpy(op->key, key, length + 1);
    if (value) tx->retain(value);
    ++tx->count;
    return true;
}

/// Non-destructive host preflight
bool obs_live_txn_preflight(const ObsLiveBindingTxn *tx,
                            ObsLiveValidate validate, void *context) {
    if (!tx || !tx->active || !validate) return false;
    for (size_t i = 0; i < tx->count; ++i)
        if (!validate(&tx->operations[i], context)) return false;
    return true;
}

/// Commit and rollback
void obs_live_txn_abort(ObsLiveBindingTxn *tx) {
    if (!tx || !tx->active) return;
    for (size_t i = 0; i < tx->count; ++i) {
        if (tx->operations[i].value)
            tx->release(tx->operations[i].value);
        tx->operations[i] = (ObsLiveOp){0};
    }
    tx->count = 0;
    tx->active = false;
    tx->retain = NULL;
    tx->release = NULL;
}

bool obs_live_txn_finish_checked(ObsLiveBindingTxn *tx, bool published,
    ObsLiveValidate validate, ObsLiveBeforeCommit before_commit,
    ObsLiveApply apply, void *context) {
    if (!tx || !tx->active || (published && (!apply || !validate))) return false;
    if (published && !obs_live_txn_preflight(tx, validate, context)) {
        ++tx->preflight_failures;
        obs_live_txn_abort(tx);
        return false;
    }
    if (published && before_commit) before_commit(context);
    bool success = true;
    if (published) {
        for (size_t i = 0; i < tx->count; ++i) {
            if (!apply(&tx->operations[i], context)) {
                ++tx->application_failures;
                success = false;
            }
        }
        if (success) ++tx->committed;
    } else {
        ++tx->rejected;
    }
    obs_live_txn_abort(tx);
    return success;
}

/* Backward-compatible test/host entry point: does not preflight or promise
 * all-or-nothing application; prefer finish_checked for engine callbacks. */
bool obs_live_txn_finish(ObsLiveBindingTxn *tx, bool published,
                         ObsLiveApply apply, void *context) {
    if (!tx || !tx->active || (published && !apply)) return false;
    bool success = true;
    if (published) {
        for (size_t i = 0; i < tx->count; ++i)
            if (!apply(&tx->operations[i], context)) {
                ++tx->application_failures;
                success = false;
            }
        if (success) ++tx->committed;
    } else ++tx->rejected;
    obs_live_txn_abort(tx);
    return success;
}

/// Infallible host commit after successful preparation
bool obs_live_txn_finish_prepared(ObsLiveBindingTxn *tx, bool published,
    ObsLivePrepare prepare, ObsLiveCommit commit, void *context) {
    if (!tx || !tx->active || (published && (!prepare || !commit)))
        return false;
    if (!published) {
        ++tx->rejected;
        obs_live_txn_abort(tx);
        return true;
    }
    /* A false prepare must leave the live host state untouched. */
    if (!prepare(tx, context)) {
        ++tx->preflight_failures;
        obs_live_txn_abort(tx);
        return false;
    }
    /* Compiler image publication was already accepted; no host operation
     * below is allowed to fail or perform an allocation. */
    commit(tx, context);
    ++tx->committed;
    obs_live_txn_abort(tx);
    return true;
}
