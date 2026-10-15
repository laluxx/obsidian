/// foreign.c — Single registration, object identity and safety boundary
/// Implementation
#include "foreign.h"

monad_status_t obs_host_invalid(monad_error_t *error, const char *message) {
    if (error) { error->status = MONAD_ERROR_INVALID_ARGUMENT; error->message = message; }
    return MONAD_ERROR_INVALID_ARGUMENT;
}
bool obs_host_owner(const ObsMonadRuntime *host) {
    return host && host->active && host->space && host == obs_monad_engine() &&
           obs_monad_on_owner_thread();
}
uint64_t obs_host_epoch(const ObsMonadRuntime *host, uint64_t revision) {
    /* Each revision is monotone independently. This is an epoch *fingerprint*,
       not a mathematical bijection: an astronomically unlikely collision is
       still possible. A future object registry should use stable IDs. */
    uint64_t g = host ? host->generation : 0;
    uint64_t x = revision ^ (g + UINT64_C(0x9e3779b97f4a7c15) +
                            (revision << 6) + (revision >> 2));
    return x ? x : UINT64_MAX;
}
bool obs_host_ref_value(monad_value_t *value, ObsMonadRuntime *host,
                        const char *type, uint64_t epoch,
                        void **pointer, monad_error_t *error) {
    void *candidate = NULL;
    uint64_t received = 0;
    if (!obs_host_owner(host) || !value || !type) {
        obs_host_invalid(error, "foreign reference requires owning engine thread");
        return false;
    }
    if (monad_value_host_ref(value, type, &candidate, &received, error) != MONAD_OK)
        return false;
    if (!candidate || received != epoch) {
        obs_host_invalid(error, "expired or foreign host object");
        return false;
    }
    if (pointer) *pointer = candidate;
    return true;
}
bool obs_host_require(monad_value_t *value, ObsMonadRuntime *host,
                      const char *type, const void *pointer, uint64_t epoch,
                      monad_error_t *error) {
    void *candidate = NULL;
    if (!obs_host_ref_value(value, host, type, epoch, &candidate, error))
        return false;
    if (candidate != pointer) {
        obs_host_invalid(error, "foreign reference does not belong to this engine");
        return false;
    }
    return true;
}
/// Stable logical foreign identity
bool obs_host_borrow_identity(monad_object_space_t *space, ObsMonadRuntime *host,
                              const char *type, ObsIdentityRegistry *registry,
                              ObsIdentityView view, size_t index,
                              monad_value_t **out, monad_error_t *error) {
    const void *token = NULL;
    uint32_t generation = 0;
    if (!space || !obs_host_owner(host) || !type || !out ||
        !obs_identity_borrow(registry, view, index, &token, &generation)) {
        obs_host_invalid(error, "cannot borrow live scene identity");
        return false;
    }
    return monad_value_intern_host_ref(space, type, (void *)token,
                obs_host_epoch(host, generation), out, error) == MONAD_OK;
}
bool obs_host_resolve_identity(monad_value_t *value, ObsMonadRuntime *host,
                               const char *type, ObsIdentityRegistry *registry,
                               ObsIdentityView view, size_t *index,
                               monad_error_t *error) {
    void *token = NULL;
    uint64_t epoch = 0;
    uint32_t generation = 0;
    if (!obs_host_owner(host) || !value || !type || !index ||
        monad_value_host_ref(value, type, &token, &epoch, error) != MONAD_OK ||
        !obs_identity_generation(registry, view, token, &generation) ||
        epoch != obs_host_epoch(host, generation) ||
        !obs_identity_resolve(registry, view, token, generation, index)) {
        obs_host_invalid(error, "expired, foreign or mismatched scene identity");
        return false;
    }
    return true;
}
bool obs_host_borrow(monad_object_space_t *space, ObsMonadRuntime *host,
                     const char *type, void *pointer, uint64_t epoch,
                     monad_value_t **out, monad_error_t *error) {
    if (!space || !obs_host_owner(host) || !type || !pointer || !out) {
        obs_host_invalid(error, "cannot borrow an unavailable engine object");
        return false;
    }
    return monad_value_intern_host_ref(space, type, pointer, epoch, out, error) == MONAD_OK;
}
bool obs_host_register(ObsMonadRuntime *host, const ObsHostBinding *entries,
                       size_t count, monad_error_t *error) {
    if (!obs_host_owner(host) || (!entries && count)) {
        obs_host_invalid(error, "host registrations require the active runtime");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const ObsHostBinding *entry = &entries[i];
        if (!entry->name || !entry->callback) {
            obs_host_invalid(error, "invalid host binding descriptor");
            return false;
        }
        monad_dynamic_procedure_descriptor_t desc;
        monad_dynamic_procedure_descriptor_init(&desc);
        desc.name = entry->name;
        desc.docstring = entry->description;
        desc.minimum_arity = desc.maximum_arity = entry->arity;
        desc.function = entry->callback;
        desc.context = entry->context ? entry->context : host;
        monad_dynamic_binding_t *binding = NULL;
        if (monad_dynamic_define_procedure(host->space, &desc, &binding, error) != MONAD_OK)
            return false;
    }
    return true;
}
