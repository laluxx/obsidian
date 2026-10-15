/// publication.c — Deterministic host-lease and QTT contract regressions
#include "publication.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Compiler-authored export contract policy
static const ObsExportContract old_exports[] = {
    {"tick", UINT64_C(0xabcdf123), OBS_EFFECT_READ | OBS_EFFECT_WRITE,
        OBS_QUANTITY_LINEAR, OBS_LIFETIME_FRAME},
    {"scene", UINT64_C(0xabcdef56), OBS_EFFECT_READ,
        OBS_QUANTITY_UNRESTRICTED, OBS_LIFETIME_ENGINE},
};
static ObsImageContract baseline(void) {
    return (ObsImageContract){.exports=old_exports, .export_count=2,
        .state_schema=117, .schema_version=1, .compiler_verified=true};
}
static void test_contracts(void) {
    ObsImageContract old = baseline();
    ObsImageContract candidate = baseline();
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_OK);
    assert(obs_publication_validate(NULL, &candidate) == OBS_CONTRACT_OK);
    candidate.compiler_verified = false;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_UNVERIFIED);
    candidate = baseline(); candidate.export_count = 0;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_MISSING_EXPORT);
    candidate = baseline(); candidate.state_schema++;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_STATE_MIGRATION_REQUIRED);
    candidate.has_verified_migration = true;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_OK);
    ObsExportContract exports[2]; memcpy(exports, old_exports, sizeof(exports));
    candidate = baseline(); candidate.exports = exports;
    exports[0].type_identity++;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_TYPE_MISMATCH);
    exports[0] = old_exports[0]; exports[0].quantity = OBS_QUANTITY_UNRESTRICTED;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_QUANTITY_MISMATCH);
    exports[0] = old_exports[0]; exports[0].lifetime = OBS_LIFETIME_ENGINE;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_LIFETIME_MISMATCH);
    exports[0] = old_exports[0]; exports[0].effects |= OBS_EFFECT_IO;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_EFFECT_ESCALATION);
    exports[0] = old_exports[0]; exports[0].effects = OBS_EFFECT_READ;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_OK);
    exports[0] = old_exports[0]; exports[0].type_identity = 0;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_INVALID);
    exports[0] = old_exports[0]; exports[1].name = "tick";
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_INVALID);
    exports[1] = old_exports[1]; exports[1].effects = UINT32_MAX;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_INVALID);
    candidate.exports = NULL;
    assert(obs_publication_validate(&old, &candidate) == OBS_CONTRACT_INVALID);
}

/// Lease and publication state machine
static void test_host_lifecycle(void) {
    ObsPublication s = {0};
    ObsCodeLease a = {0}, b = {0};
    assert(!obs_publication_begin_call(&s, &a));
    assert(!obs_publication_begin_call(NULL, &a));
    assert(!obs_publication_begin_legacy_load(NULL));
    assert(obs_publication_begin_legacy_load(&s));
    assert(!obs_publication_begin_legacy_load(&s));
    assert(!obs_publication_can_retire(&s));
    obs_publication_finish_legacy_load(&s, true);
    assert(s.phase == OBS_LIVE_READY && s.epoch == 1 && s.successes == 1);
    assert(obs_publication_begin_call(&s, &a));
    assert(!obs_publication_begin_call(&s, &a));
    assert(obs_publication_begin_call(&s, &b));
    assert(s.active_calls == 2);
    assert(!obs_publication_begin_legacy_load(&s));
    assert(!obs_publication_can_retire(&s));
    assert(!obs_publication_end_call(&s, &(ObsCodeLease){.active=true,.epoch=999}));
    assert(obs_publication_end_call(&s, &b));
    assert(obs_publication_end_call(&s, &a));
    assert(!obs_publication_end_call(&s, &a));
    assert(obs_publication_can_retire(&s));
    assert(obs_publication_begin_legacy_load(&s));
    obs_publication_finish_legacy_load(&s, false);
    assert(s.phase == OBS_LIVE_POISONED && s.epoch == 1 && s.failures == 1);
    assert(!obs_publication_begin_call(&s, &a));
    assert(obs_publication_begin_legacy_load(&s)); /* manual retry */
    obs_publication_finish_legacy_load(&s, true);
    assert(s.epoch == 2 && s.successes == 2 && s.attempts == 3);
    assert(obs_publication_begin_call(&s, &a));
    assert(a.epoch == 2 && obs_publication_end_call(&s, &a));
    obs_publication_reset(&s);
    assert(s.phase == OBS_LIVE_EMPTY && s.epoch == 0 && s.active_calls == 0);
}

/// New MonadC ABI v2: rejected native source leaves old generation alive.
static void test_atomic_reload(void) {
    ObsPublication state = {0};
    ObsCodeLease old = {0};
    assert(obs_publication_begin_atomic_load(&state));
    obs_publication_finish_atomic_load(&state, false);
    assert(state.phase == OBS_LIVE_EMPTY && state.epoch == 0);
    assert(!obs_publication_mutations_allowed(&state));
    assert(obs_publication_begin_atomic_load(&state));
    obs_publication_finish_atomic_load(&state, true);
    assert(state.phase == OBS_LIVE_READY && state.epoch == 1);
    assert(obs_publication_mutations_allowed(&state));
    assert(obs_publication_begin_call(&state, &old));
    assert(!obs_publication_begin_atomic_load(&state));
    assert(obs_publication_end_call(&state, &old));
    assert(obs_publication_begin_atomic_load(&state));
    assert(!obs_publication_begin_call(&state, &old));
    assert(!obs_publication_mutations_allowed(&state));
    obs_publication_finish_atomic_load(&state, false);
    assert(obs_publication_mutations_allowed(&state));
    assert(state.phase == OBS_LIVE_READY && state.epoch == 1);
    assert(obs_publication_begin_call(&state, &old));
    assert(obs_publication_end_call(&state, &old));
    assert(obs_publication_begin_atomic_load(&state));
    obs_publication_finish_atomic_load(&state, true);
    assert(state.phase == OBS_LIVE_READY && state.epoch == 2);
    assert(state.attempts == 4 && state.successes == 2 && state.failures == 2);
}

/// Deterministic state-machine fuzz
static void test_fuzz(void) {
    uint32_t r = 0x6a09e667u;
    ObsPublication state = {0};
    ObsCodeLease leases[16] = {{0}};
    for (unsigned i = 0; i < 50000; ++i) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        unsigned slot = r % 16;
        switch ((r >> 8) % 5) {
        case 0: (void)obs_publication_begin_legacy_load(&state); break;
        case 1: obs_publication_finish_legacy_load(&state, !!(r & 0x10000u)); break;
        case 2: (void)obs_publication_begin_call(&state, &leases[slot]); break;
        case 3: (void)obs_publication_end_call(&state, &leases[slot]); break;
        default: (void)obs_publication_can_retire(&state); break;
        }
        unsigned counted = 0;
        for (unsigned j = 0; j < 16; ++j) counted += leases[j].active;
        assert(state.active_calls == counted);
        assert(state.phase != OBS_LIVE_LOADING || state.active_calls == 0);
        assert(!state.active_calls || state.phase == OBS_LIVE_READY);
        assert(!state.epoch || state.successes == state.epoch);
    }
}
int main(void) {
    test_contracts(); test_host_lifecycle(); test_atomic_reload(); test_fuzz();
    puts("publication: QTT contract/leases/state-machine fuzz PASS");
    return 0;
}
