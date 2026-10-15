/// foreign — monad subsystem
#ifndef OBSIDIAN_MONAD_FOREIGN_H
#define OBSIDIAN_MONAD_FOREIGN_H
/* Single C -> Monad embedding boundary. No graphics/scene headers leak here. */
#include "runtime.h"
#include "object_identity.h"
#include <stddef.h>
#include <stdint.h>

/// Public interface

typedef monad_status_t (*ObsHostCallback)(monad_object_space_t *, size_t,
    monad_value_t *const *, monad_value_t **, void *, monad_error_t *);
typedef struct {
    const char *name;
    const char *description;
    size_t arity;
    ObsHostCallback callback;
    void *context; /* NULL selects the owning runtime. */
} ObsHostBinding;

monad_status_t obs_host_invalid(monad_error_t *error, const char *message);
bool obs_host_owner(const ObsMonadRuntime *host);
bool obs_host_register(ObsMonadRuntime *host, const ObsHostBinding *entries,
                       size_t count, monad_error_t *error);
bool obs_host_require(monad_value_t *value, ObsMonadRuntime *host,
                      const char *type, const void *pointer, uint64_t epoch,
                      monad_error_t *error);
bool obs_host_borrow_identity(monad_object_space_t *space, ObsMonadRuntime *host,
                              const char *type, ObsIdentityRegistry *registry,
                              ObsIdentityView view, size_t index,
                              monad_value_t **out, monad_error_t *error);
bool obs_host_resolve_identity(monad_value_t *value, ObsMonadRuntime *host,
                               const char *type, ObsIdentityRegistry *registry,
                               ObsIdentityView view, size_t *index,
                               monad_error_t *error);
bool obs_host_borrow(monad_object_space_t *space, ObsMonadRuntime *host,
                     const char *type, void *pointer, uint64_t epoch,
                     monad_value_t **out, monad_error_t *error);
/* Epoch contains runtime incarnation and subsystem structural revision. */
uint64_t obs_host_epoch(const ObsMonadRuntime *host, uint64_t revision);
/* Host ref pointers are never dereferenced before equality-to-current-owner. */
bool obs_host_ref_value(monad_value_t *value, ObsMonadRuntime *host,
                        const char *type, uint64_t epoch,
                        void **pointer, monad_error_t *error);
#endif
