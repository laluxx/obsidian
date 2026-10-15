/// sculpt_api_contract — test-only declaration seam for the REAL Monad bridge
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct ObsMonadRuntime {int active;} ObsMonadRuntime;
typedef struct monad_object_space {int active;} monad_object_space_t;
typedef struct monad_error {int status; const char *message;} monad_error_t;
typedef struct monad_value { int kind; double number; int64_t integer; } monad_value_t;
typedef int monad_status_t;
#define MONAD_OK 0
#define OBS_EFFECT_WRITE 2u
#define OBS_EFFECT_GPU 4u
#define OBS_LIFETIME_ENGINE 2u
typedef monad_status_t (*ObsHostCallback)(monad_object_space_t *, size_t,
    monad_value_t *const *, monad_value_t **, void *, monad_error_t *);
typedef struct {
    const char *name, *description;
    size_t arity;
    ObsHostCallback callback;
    void *context;
} ObsHostBinding;
bool obs_host_owner(const ObsMonadRuntime *host);
bool obs_monad_effect_allowed(unsigned effects, unsigned lifetime, bool frame_active);
monad_status_t obs_host_invalid(monad_error_t *error, const char *message);
bool obs_host_register(ObsMonadRuntime *host, const ObsHostBinding *entries,
                       size_t n, monad_error_t *error);
monad_status_t monad_value_as_f64(monad_value_t *v, double *out, monad_error_t *e);
monad_status_t monad_value_as_i64(monad_value_t *v, int64_t *out, monad_error_t *e);
monad_status_t monad_value_make_i64(monad_object_space_t *sp, int64_t n,
                                    monad_value_t **out, monad_error_t *e);
monad_status_t monad_value_make_f64(monad_object_space_t *sp, double n,
                                    monad_value_t **out, monad_error_t *e);
monad_value_t *monad_value_nil(monad_object_space_t *sp);
