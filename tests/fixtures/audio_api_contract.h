/// audio_api_contract — test-only declaration seam for the REAL Monad bridge
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio.h"
typedef struct monad {int placeholder;} monad_t;
typedef struct ObsPublication {int placeholder;} ObsPublication;
typedef struct monad_object_space {int active;} monad_object_space_t;
typedef struct monad_i64_pair_step {
    const struct monad_value *cdr;
    int64_t car;
} monad_i64_pair_step_t;
typedef int monad_status_t;
typedef struct monad_error {int status; const char *message;} monad_error_t;
typedef struct monad_value {
    int kind;
    double number;
    int64_t integer;
    const struct monad_value *cdr;
} monad_value_t;
typedef int64_t (*monad_i64_function_t)(int64_t);
typedef struct ObsNativeRead {
    int active;
    const char *function_name;
} ObsNativeRead;
typedef struct ObsMonadRuntime {
    int active;
    monad_object_space_t *space;
    monad_t *runtime;
    ObsPublication publication;
} ObsMonadRuntime;
#define MONAD_OK 0
#define MONAD_VALUE_NIL 0
#define MONAD_VALUE_BOOL 1
#define MONAD_VALUE_I64 2
#define MONAD_VALUE_F64 3
#define MONAD_VALUE_CONS 6
#define MONAD_VALUE_PROCEDURE 8
#define OBS_EFFECT_WRITE 2u
#define OBS_LIFETIME_ENGINE 2u
typedef struct ObsCodeLease { int active; } ObsCodeLease;
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
ObsMonadRuntime *obs_monad_engine(void);
bool obs_native_read_begin(ObsNativeRead *read, monad_t *runtime,
                           ObsPublication *publication, monad_error_t *error);
bool obs_native_read_find_i64(const ObsNativeRead *read, const char *name,
                              monad_i64_function_t *function,
                              monad_error_t *error);
void obs_native_read_end(ObsNativeRead *read);
monad_status_t obs_host_invalid(monad_error_t *error, const char *message);
bool obs_host_register(ObsMonadRuntime *host, const ObsHostBinding *entries,
                       size_t n, monad_error_t *error);
monad_i64_pair_step_t monad_value_i64_pair_step_borrowed(
    const monad_value_t *pair);
int monad_value_kind(const monad_value_t *value);
monad_value_t *monad_value_bool(monad_object_space_t *sp, bool b);
void audio_buffer_fill_mono_i16(AudioBuffer *buf, const int16_t *samples,
                                uint32_t frame_count);
void audio_buffer_stop(AudioBuffer *buf);
void audio_buffer_play(AudioBuffer *buf);
AudioBuffer *audio_buffer_create(uint32_t frame_count);
void audio_buffer_destroy(AudioBuffer *buf);
bool obs_monad_register_audio_api(ObsMonadRuntime *host, monad_error_t *error);
void obs_monad_audio_typed_character(uint32_t codepoint);
