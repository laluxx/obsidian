/// native_read.c — Host leases composed with compiler-owned JIT image pins
#include "native_read.h"
#include <string.h>

/// Coherent read lifecycle
static bool read_open(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication,
                      const monad_abi_signature_t *signature,
                      monad_error_t *error) {
    if (!read || !runtime || !publication) return false;
    /* No active read may be overwritten: callers must end the previous one. */
    if (read->snapshot || read->lease.active) return false;
    if (!obs_publication_begin_call(publication, &read->lease)) return false;
    read->publication = publication;
    monad_read_t *snapshot = NULL;
    monad_status_t status = signature
        ? monad_read_begin_signature(runtime, signature, &snapshot, error)
        : monad_read_begin(runtime, &snapshot, error);
    if (status != MONAD_OK || !snapshot) {
        /* The host lease was acquired, but no native image was pinned. */
        (void)obs_publication_end_call(publication, &read->lease);
        read->publication = NULL;
        return false;
    }
    read->snapshot = snapshot;
    read->generation = monad_read_generation(snapshot);
    return true;
}

bool obs_native_read_begin(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication, monad_error_t *error) {
    return read_open(read, runtime, publication, NULL, error);
}

bool obs_native_read_begin_signature(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication,
                           const monad_abi_signature_t *signature,
                           monad_error_t *error) {
    if (!signature) return false;
    return read_open(read, runtime, publication, signature, error);
}

bool obs_native_read_find(ObsNativeRead *read, const char *name,
                          monad_native_address_t *address, monad_error_t *error) {
    if (address) *address = NULL;
    if (!read || !read->snapshot || !read->lease.active || !name || !*name ||
        !address) return false;
    return monad_read_find_native(read->snapshot, name, address, error)
        == MONAD_OK && *address != NULL;
}

bool obs_native_read_find_i64(const ObsNativeRead *read, const char *name,
                              monad_i64_function_t *function, monad_error_t *error) {
    if (function) *function = NULL;
    if (!read || !read->snapshot || !read->lease.active || !name || !*name ||
        !function) return false;
    return monad_read_find_i64_function(read->snapshot, name, function, error)
            == MONAD_OK && *function != NULL;
}

void obs_native_read_end(ObsNativeRead *read) {
    if (!read) return;
    if (read->snapshot) monad_read_end(read->snapshot);
    if (read->publication && read->lease.active)
        (void)obs_publication_end_call(read->publication, &read->lease);
    /* Drop any address/generation data retained by the host-side wrapper. */
    memset(read, 0, sizeof(*read));
}
