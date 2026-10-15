/// live_binding_txn.c — Two-phase host callback publication regressions
#include "live_binding_txn.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// Retained compiler values / fake live host
static unsigned retains, releases, applications;
static int installed_frame;
static int installed_key[8];
static void retain(void *p) { (void)p; ++retains; }
static void release(void *p) { (void)p; ++releases; }
static bool apply(const ObsLiveOp *op, void *unused) {
    (void)unused;
    ++applications;
    if (op->action == OBS_LIVE_FRAME_BIND) {
        installed_frame = *(int *)op->value;
    } else {
        unsigned index = (unsigned)(op->key[0] - '0');
        assert(index < 8);
        installed_key[index] = op->action == OBS_LIVE_KEY_UNBIND ? 0 :
                               *(int *)op->value;
    }
    return true;
}
static bool failing_apply(const ObsLiveOp *op, void *unused) {
    (void)op; (void)unused; return false;
}

/// Rollback, commit and ownership
static void test_transactions(void) {
    ObsLiveBindingTxn txn = {0};
    int old = 55, value = 77, second = 88;
    installed_frame = old;
    installed_key[1] = old;
    assert(!obs_live_txn_begin(&txn, NULL, release));
    assert(obs_live_txn_begin(&txn, retain, release));
    assert(!obs_live_txn_begin(&txn, retain, release));
    assert(obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &value));
    assert(obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, "1", &value));
    assert(!obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, "bad", &value));
    assert(!obs_live_txn_append(&txn, OBS_LIVE_KEY_UNBIND, "1", &value));
    assert(!obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, "1", NULL));
    assert(obs_live_txn_finish(&txn, false, apply, NULL));
    assert(installed_frame == old && installed_key[1] == old);
    assert(retains == releases && applications == 0);
    assert(txn.rejected == 1 && !txn.active);
    assert(!obs_live_txn_append(&txn, OBS_LIVE_KEY_UNBIND, "1", NULL));

    assert(obs_live_txn_begin(&txn, retain, release));
    assert(obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &value));
    assert(obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, "1", &value));
    assert(obs_live_txn_append(&txn, OBS_LIVE_KEY_UNBIND, "1", NULL));
    assert(obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, "2", &second));
    assert(obs_live_txn_finish(&txn, true, apply, NULL));
    assert(installed_frame == 77 && installed_key[1] == 0 && installed_key[2] == 88);
    assert(retains == releases && applications == 4 && txn.committed == 1);

    assert(obs_live_txn_begin(&txn, retain, release));
    assert(obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, "3", &value));
    assert(!obs_live_txn_finish(&txn, true, failing_apply, NULL));
    assert(txn.application_failures == 1 && retains == releases);
    assert(obs_live_txn_begin(&txn, retain, release));
    assert(obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &second));
    obs_live_txn_abort(&txn);
    obs_live_txn_abort(&txn);
    assert(retains == releases && installed_frame == 77);
}

/// Preflight guards irreversible callback retirement
static unsigned validated, retired, allowed;
static bool validate(const ObsLiveOp *op, void *context) {
    (void)context;
    ++validated;
    return op->action == OBS_LIVE_FRAME_BIND ||
           (op->action == OBS_LIVE_KEY_BIND &&
            op->key[0] != '7' && (allowed & 1u));
}
static void retire(void *unused) { (void)unused; ++retired; }
static void test_checked_publication(void) {
    ObsLiveBindingTxn tx = {0};
    int value = 42;
    unsigned initial_applications = applications;
    allowed = 0;
    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "1", &value));
    assert(obs_live_txn_preflight(&tx, validate, NULL) == false);
    assert(tx.active && tx.count == 1 && retired == 0);
    assert(!obs_live_txn_finish_checked(&tx, true, validate, retire, apply, NULL));
    assert(retired == 0 && applications == initial_applications);
    assert(tx.preflight_failures == 1 && tx.application_failures == 0 &&
           tx.committed == 0 && !tx.active && retains == releases);

    allowed = 1;
    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "1", &value));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "7", &value));
    assert(!obs_live_txn_finish_checked(&tx, true, validate, retire, apply, NULL));
    assert(retired == 0 && applications == initial_applications);
    assert(retains == releases);

    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_FRAME_BIND, NULL, &value));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "2", &value));
    assert(obs_live_txn_finish_checked(&tx, true, validate, retire, apply, NULL));
    assert(retired == 1 && applications == initial_applications + 2);
    assert(installed_frame == 42 && installed_key[2] == 42);
    assert(retains == releases);

    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_FRAME_BIND, NULL, &value));
    assert(obs_live_txn_finish_checked(&tx, false, validate, retire, apply, NULL));
    assert(retired == 1 && applications == initial_applications + 2);
    assert(retains == releases);

    for (unsigned i = 0; i < 50000; ++i) {
        assert(obs_live_txn_begin(&tx, retain, release));
        assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND,
                                   (i % 5u == 0u) ? "7" : "3", &value));
        bool success = obs_live_txn_finish_checked(
            &tx, true, validate, retire, apply, NULL);
        assert(success == (i % 5u != 0u));
        assert(retains == releases && !tx.active);
    }
}

/// Saturation, malformed arguments, and repeated lifecycle fuzz
static void test_bounds(void) {
    ObsLiveBindingTxn txn = {0};
    int value = 1;
    assert(obs_live_txn_begin(&txn, retain, release));
    char invalid[OBS_LIVE_KEY_CAP + 2];
    memset(invalid, 'x', sizeof(invalid)); invalid[sizeof(invalid)-1] = 0;
    assert(!obs_live_txn_append(&txn, OBS_LIVE_KEY_BIND, invalid, &value));
    for (unsigned i = 0; i < OBS_LIVE_TXN_CAP; ++i)
        assert(obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &value));
    assert(!obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &value));
    obs_live_txn_abort(&txn);
    assert(retains == releases);
    uint32_t state = 0x9e3779b9u;
    for (unsigned i = 0; i < 50000; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        assert(obs_live_txn_begin(&txn, retain, release));
        unsigned count = state & 7u;
        for (unsigned j = 0; j < count; ++j)
            assert(obs_live_txn_append(&txn, OBS_LIVE_FRAME_BIND, NULL, &value));
        bool published = (state & 8u) != 0;
        assert(obs_live_txn_finish(&txn, published, apply, NULL));
        assert(retains == releases && !txn.active && txn.count == 0);
    }
}
/// Prepared host commit must not call application on rejected prepare
static unsigned prepare_calls, commit_calls;
static bool prepared_ok;
static bool fake_prepare(const ObsLiveBindingTxn *tx, void *ctx) {
    (void)ctx;
    ++prepare_calls;
    assert(tx->active && tx->count == 1);
    return prepared_ok;
}
static void fake_commit(const ObsLiveBindingTxn *tx, void *ctx) {
    (void)ctx;
    ++commit_calls;
    assert(tx->active && tx->count == 1 && prepared_ok);
}
static void test_prepared_commit(void) {
    ObsLiveBindingTxn tx = {0};
    int value = 111;
    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "C-x", &value));
    prepared_ok = false;
    assert(!obs_live_txn_finish_prepared(&tx, true,
        fake_prepare, fake_commit, NULL));
    assert(prepare_calls == 1 && commit_calls == 0 &&
           tx.preflight_failures == 1 && retains == releases);
    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "C-x", &value));
    prepared_ok = true;
    assert(obs_live_txn_finish_prepared(&tx, true,
        fake_prepare, fake_commit, NULL));
    assert(prepare_calls == 2 && commit_calls == 1 &&
           tx.committed == 1 && retains == releases);
    assert(obs_live_txn_begin(&tx, retain, release));
    assert(obs_live_txn_append(&tx, OBS_LIVE_KEY_BIND, "C-x", &value));
    assert(obs_live_txn_finish_prepared(&tx, false,
        fake_prepare, fake_commit, NULL));
    assert(prepare_calls == 2 && commit_calls == 1 &&
           tx.rejected == 1 && retains == releases);
}
int main(void) {
    test_transactions(); test_bounds(); test_checked_publication(); test_prepared_commit();
    printf("live binding publication: checked preflight, rollback/commit, 100,000 cycles PASS\n");
}
