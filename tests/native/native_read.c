/// native_read.c — SDK-typed deterministic coherent publication/read tests
#include "native_read.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/// Stub compiler SDK: use the *real headers*, never invented prototypes
struct monad { unsigned tag; };
struct monad_read { uint64_t generation; };
static struct monad_read s_read = { .generation = 73 };
static unsigned s_begins, s_ends, s_lookups, s_fails;
static bool s_refuse_begin, s_refuse_symbol;
static int64_t compiled_step(int64_t value) { return value + 42; }

monad_status_t monad_read_begin(monad_t *host, monad_read_t **read,
                                 monad_error_t *error) {
    (void)error;
    ++s_begins;
    if (s_refuse_begin || !host) { *read = NULL; ++s_fails; return MONAD_ERROR_INVALID_ARGUMENT; }
    *read = &s_read;
    return MONAD_OK;
}
monad_status_t monad_read_begin_signature(monad_t *host,
    const monad_abi_signature_t *signature, monad_read_t **read,
    monad_error_t *error) {
    if (!signature || signature->result != MONAD_ABI_I64)
        return MONAD_ERROR_SIGNATURE_MISMATCH;
    return monad_read_begin(host, read, error);
}
monad_status_t monad_read_find_native(const monad_read_t *read,
    const char *name, monad_native_address_t *address, monad_error_t *error) {
    (void)error;
    assert(read == &s_read);
    if (strcmp(name, "generic-step")) return MONAD_ERROR_NOT_FOUND;
    /* This test never *calls* an erased function pointer with the wrong ABI. */
    *address = (monad_native_address_t)compiled_step;
    return MONAD_OK;
}
uint64_t monad_read_generation(const monad_read_t *read) { return read->generation; }
monad_status_t monad_read_find_i64_function(const monad_read_t *read,
    const char *name, monad_i64_function_t *function, monad_error_t *error) {
    (void)error;
    assert(read == &s_read);
    ++s_lookups;
    if (s_refuse_symbol || strcmp(name, "step")) { *function = NULL; return MONAD_ERROR_INVALID_ARGUMENT; }
    *function = compiled_step;
    return MONAD_OK;
}
void monad_read_end(monad_read_t *read) { assert(read == &s_read); ++s_ends; }

/// State machine, rollback, and generation-pinning contracts
int main(void) {
    struct monad host = { .tag = 1 };
    ObsPublication publication = {0};
    ObsNativeRead read = {0};
    monad_error_t error = {0};
    obs_publication_reset(&publication);
    assert(!obs_native_read_begin(&read, &host, &publication, &error));
    assert(s_begins == 0 && publication.active_calls == 0);
    assert(obs_publication_begin_atomic_load(&publication));
    obs_publication_finish_atomic_load(&publication, true);
    assert(publication.phase == OBS_LIVE_READY);

    for (unsigned i = 0; i < 50000; ++i) {
        assert(obs_native_read_begin(&read, &host, &publication, &error));
        assert(read.generation == 73 && publication.active_calls == 1);
        assert(!obs_native_read_begin(&read, &host, &publication, &error));
        assert(!obs_publication_begin_atomic_load(&publication));
        monad_i64_function_t function = NULL;
        assert(obs_native_read_find_i64(&read, "step", &function, &error));
        assert(function((int64_t)i) == (int64_t)i + 42);
        assert(!obs_native_read_find_i64(&read, "unknown", &function, &error));
        assert(function == NULL);
        obs_native_read_end(&read);
        assert(publication.active_calls == 0 && read.snapshot == NULL);
        obs_native_read_end(&read); /* idempotent release */
    }
    /* Signature-parametric snapshot uses the same code-retirement gate. */
    monad_abi_signature_t sig = { .result = MONAD_ABI_I64 };
    assert(obs_native_read_begin_signature(&read, &host, &publication,
                                           &sig, &error));
    monad_native_address_t address = NULL;
    assert(obs_native_read_find(&read, "generic-step", &address, &error));
    assert(address != NULL);
    obs_native_read_end(&read);
    sig.result = MONAD_ABI_F64;
    assert(!obs_native_read_begin_signature(&read, &host, &publication,
                                            &sig, &error));
    assert(publication.active_calls == 0);
    assert(!obs_native_read_begin_signature(&read, &host, &publication,
                                            NULL, &error));
    assert(publication.active_calls == 0);
    s_refuse_begin = true;
    assert(!obs_native_read_begin(&read, &host, &publication, &error));
    assert(publication.active_calls == 0);
    s_refuse_begin = false;
    s_refuse_symbol = true;
    assert(obs_native_read_begin(&read, &host, &publication, &error));
    monad_i64_function_t fn = compiled_step;
    assert(!obs_native_read_find_i64(&read, "step", &fn, &error));
    assert(fn == NULL);
    obs_native_read_end(&read);
    assert(s_begins == s_ends + s_fails);
    printf("PASS: coherent native generation reads, 50,000 leases, rejection cleanup (%u opens)\n", s_begins);
    return 0;
}
