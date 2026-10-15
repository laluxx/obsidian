/// native_read.h — Coherent, generation-pinned native reads from MonadC
#ifndef OBSIDIAN_MONAD_NATIVE_READ_H
#define OBSIDIAN_MONAD_NATIVE_READ_H

#include <stdbool.h>
#include <stdint.h>
#include <monad/monad.h>
#include "publication.h"

/// Ownership
/* A read is a *borrow* of one immutable published native generation.
 * begin() acquires the host execution gate and a compiler-owned generation
 * pin. finish() must be called on every successful begin() path. Neither
 * the host epoch nor QTT certificates substitute for Monad's native pin.
 * This wrapper is owner-thread-only: callers enforce that separately. */
typedef struct ObsNativeRead {
    monad_read_t *snapshot;
    ObsPublication *publication;
    ObsCodeLease lease;
    uint64_t generation;
} ObsNativeRead;

/* Signature-specific snapshots support future typed physics/game callbacks
 * without installing a generic boxed interpreter in the hot path. */
bool obs_native_read_begin_signature(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication,
                           const monad_abi_signature_t *signature,
                           monad_error_t *error);
bool obs_native_read_find(ObsNativeRead *read, const char *name,
                          monad_native_address_t *address, monad_error_t *error);

bool obs_native_read_begin(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication, monad_error_t *error);
bool obs_native_read_find_i64(const ObsNativeRead *read, const char *name,
                              monad_i64_function_t *function, monad_error_t *error);
void obs_native_read_end(ObsNativeRead *read);

#endif
