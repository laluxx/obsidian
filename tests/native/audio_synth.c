#define _POSIX_C_SOURCE 200809L
#include "audio_api_contract.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static monad_object_space_t space = {.active = 1};
static monad_t native_runtime;
static ObsMonadRuntime runtime = {
    .active = 1, .space = &space, .runtime = &native_runtime
};
static ObsHostBinding host[8];
static size_t n_host;
static monad_value_t made;
static bool owner = true, effect = true;
static bool fail_alloc;
static unsigned read_begins, read_ends;
static uint32_t last_typed_codepoint;
static bool bad_native_sample, missing_sample_function;

bool obs_host_owner(const ObsMonadRuntime *h) { return h && h->active && owner; }
bool obs_monad_effect_allowed(unsigned effects, unsigned lifetime, bool frame) {
    assert(effects == OBS_EFFECT_WRITE && lifetime == OBS_LIFETIME_ENGINE && !frame);
    return effect;
}
ObsMonadRuntime *obs_monad_engine(void) { return &runtime; }
bool obs_native_read_begin(ObsNativeRead *read, monad_t *native,
                           ObsPublication *publication, monad_error_t *error) {
    (void)error;
    if (!owner || !read || native != &native_runtime ||
        publication != &runtime.publication)
        return false;
    read->active = 1;
    ++read_begins;
    return true;
}

static int64_t fake_sample_count(int64_t unused) {
    (void)unused;
    return 4;
}
static int64_t fake_sample(int64_t packed) {
    const int64_t codepoint = packed / 2048;
    const int64_t frame = packed - codepoint * 2048;
    last_typed_codepoint = (uint32_t)codepoint;
    if (bad_native_sample) return INT16_MAX + 1;
    const int16_t samples[] = {0, 10000, -10000, 0};
    return frame >= 0 && frame < 4 ? samples[frame] : 0;
}
bool obs_native_read_find_i64(const ObsNativeRead *read, const char *name,
                              monad_i64_function_t *function,
                              monad_error_t *error) {
    (void)error;
    if (!read || !read->active || !name || !function) return false;
    if (!strcmp(name, "obsidian_typing_pcm_frames")) {
        *function = fake_sample_count;
        return true;
    }
    if (!strcmp(name, "obsidian_typing_pcm_sample") &&
        !missing_sample_function) {
        *function = fake_sample;
        return true;
    }
    *function = NULL;
    return false;
}
void obs_native_read_end(ObsNativeRead *read) {
    assert(read && read->active);
    read->active = 0;
    ++read_ends;
}
monad_status_t obs_host_invalid(monad_error_t *e, const char *message) {
    if (e) {e->status = 1; e->message = message;}
    return 1;
}
bool obs_host_register(ObsMonadRuntime *h, const ObsHostBinding *entries,
                       size_t count, monad_error_t *e) {
    (void)e;
    assert(h == &runtime && count <= 8);
    memcpy(host, entries, count * sizeof(host[0]));
    n_host = count;
    return true;
}
int monad_value_kind(const monad_value_t *value) {
    return value ? value->kind : -1;
}
monad_i64_pair_step_t monad_value_i64_pair_step_borrowed(
    const monad_value_t *pair) {
    if (!pair || pair->kind != MONAD_VALUE_CONS || !pair->cdr ||
        pair->cdr->kind != MONAD_VALUE_I64)
        return (monad_i64_pair_step_t){0};
    return (monad_i64_pair_step_t){
        .cdr = pair->cdr->cdr,
        .car = pair->cdr->integer
    };
}
monad_value_t *monad_value_bool(monad_object_space_t *sp, bool b) {
    assert(sp == &space);
    made = (monad_value_t){.kind = MONAD_VALUE_BOOL, .integer = b ? 1 : 0};
    return &made;
}

/* Fake output device: record submitted PCM, not device callbacks. */
static int create_count, destroy_count, fill_count, play_count, stop_count;
static AudioBuffer *last_buffer;
AudioBuffer *audio_buffer_create(uint32_t frames) {
    if (fail_alloc || !frames) return NULL;
    AudioBuffer *buffer = calloc(1, sizeof(*buffer));
    assert(buffer);
    buffer->samples = calloc((size_t)frames * AUDIO_CHANNELS, sizeof(float));
    assert(buffer->samples);
    buffer->frame_count = frames;
    buffer->volume = 1.0f;
    last_buffer = buffer;
    ++create_count;
    return buffer;
}
void audio_buffer_destroy(AudioBuffer *buffer) {
    if (!buffer) return;
    ++destroy_count;
    free(buffer->samples);
    free(buffer);
}
void audio_buffer_fill_mono_i16(AudioBuffer *buffer, const int16_t *samples,
                                uint32_t frames) {
    assert(buffer && samples && frames <= buffer->frame_count);
    for (uint32_t i = 0; i < frames; ++i) {
        float value = (float)samples[i] / 32768.0f;
        buffer->samples[2 * i] = value;
        buffer->samples[2 * i + 1] = value;
    }
    ++fill_count;
}
void audio_buffer_stop(AudioBuffer *buffer) {
    assert(buffer);
    buffer->playing = false;
    ++stop_count;
}
void audio_buffer_play(AudioBuffer *buffer) {
    assert(buffer);
    buffer->cursor = 0;
    buffer->playing = true;
    ++play_count;
}

static monad_status_t call(const char *name, size_t argc, monad_value_t **args,
                           monad_value_t **out, monad_error_t *error) {
    for (size_t i = 0; i < n_host; ++i) {
        if (!strcmp(host[i].name, name)) {
            if (host[i].arity != argc) return 1;
            return host[i].callback(&space, argc, args, out,
                                    host[i].context ? host[i].context : &runtime,
                                    error);
        }
    }
    return 1;
}

static monad_value_t pcm_cons[5];
static monad_value_t pcm_values[5];
static monad_value_t pcm_nil = {.kind = MONAD_VALUE_NIL};
static monad_value_t *pcm(uint32_t count) {
    const int16_t values[] = {0, 32767, INT16_MIN, -1234, 4321};
    for (uint32_t i = 0; i < count; ++i) {
        pcm_cons[i] = (monad_value_t){
            .kind = MONAD_VALUE_CONS,
            .cdr = &pcm_values[i]
        };
        pcm_values[i] = (monad_value_t){
            .kind = MONAD_VALUE_I64,
            .integer = values[i]
        };
    }
    for (uint32_t i = 0; i < count; ++i)
        pcm_values[i].cdr = i + 1 < count ? &pcm_cons[i + 1] : &pcm_nil;
    return &pcm_cons[0];
}

int main(void) {
    monad_error_t error = {0};
    assert(obs_monad_register_audio_api(&runtime, &error));
    assert(n_host == 1);
    assert(!strcmp(host[0].name, "audio-play-samples!") && host[0].arity == 1);

    monad_value_t *samples = pcm(3);
    monad_value_t *args[] = {samples};
    monad_value_t *out = NULL;
    assert(call("audio-play-samples!", 1, args, &out, &error) == MONAD_OK);
    assert(out && out->kind == MONAD_VALUE_BOOL && out->integer == 1);
    assert(create_count == 1 && fill_count == 1 && play_count == 1);
    /* Replaying an equal-sized note reuses its device buffer. */
    assert(call("audio-play-samples!", 1, args, &out, &error) == MONAD_OK);
    assert(create_count == 1 && stop_count == 1 && fill_count == 2 &&
           play_count == 2);

    monad_value_t *four_samples = pcm(4);
    monad_value_t *four_args[] = {four_samples};
    assert(call("audio-play-samples!", 1, four_args, &out, &error) == MONAD_OK);
    assert(create_count == 2 && destroy_count == 1);
    assert(last_buffer && last_buffer->frame_count == 4);
    assert(fabsf(last_buffer->samples[0]) < 1e-7f);
    assert(fabsf(last_buffer->samples[2] - (32767.0f / 32768.0f)) < 1e-6f);
    assert(last_buffer->samples[4] == -1.0f);
    for (unsigned i = 0; i < 4; ++i)
        assert(last_buffer->samples[2 * i] ==
               last_buffer->samples[2 * i + 1]);

    monad_value_t empty = {.kind = MONAD_VALUE_NIL};
    monad_value_t *empty_args[] = {&empty};
    monad_value_t malformed = {.kind = MONAD_VALUE_CONS,
                               .cdr = &empty};
    monad_value_t *malformed_args[] = {&malformed};
    int fills = fill_count, plays = play_count;
    assert(call("audio-play-samples!", 1, empty_args, &out, &error) != MONAD_OK);
    assert(call("audio-play-samples!", 1, malformed_args, &out, &error) != MONAD_OK);

    monad_value_t bad_samples[] = {
        {.kind = MONAD_VALUE_I64, .integer = INT16_MAX + 1},
        {.kind = MONAD_VALUE_NIL}
    };
    monad_value_t bad_cons = {.kind = MONAD_VALUE_CONS, .cdr = &bad_samples[0]};
    bad_samples[0].cdr = &bad_samples[1];
    monad_value_t *bad_args[] = {&bad_cons};
    assert(call("audio-play-samples!", 1, bad_args, &out, &error) != MONAD_OK);
    assert(fill_count == fills && play_count == plays);

    effect = false;
    assert(call("audio-play-samples!", 1, args, &out, &error) != MONAD_OK);
    effect = true;
    owner = false;
    assert(call("audio-play-samples!", 1, args, &out, &error) != MONAD_OK);
    owner = true;

    fail_alloc = true;
    monad_value_t *five_samples = pcm(5);
    monad_value_t *five_args[] = {five_samples};
    assert(call("audio-play-samples!", 1, five_args, &out, &error) != MONAD_OK);
    fail_alloc = false;
    assert(fill_count == fills && play_count == plays);

    const unsigned reads_before_typing = read_begins;
    const int typing_fills_before = fill_count;
    const int typing_plays_before = play_count;
    obs_monad_audio_typed_character(0x1f600);
    assert(read_begins == reads_before_typing + 1 &&
           read_ends == read_begins);
    assert(last_typed_codepoint == 0x1f600);
    assert(fill_count == typing_fills_before + 1 &&
           play_count == typing_plays_before + 1);
    assert(last_buffer->frame_count == 4);
    assert(fabsf(last_buffer->samples[2] - (10000.0f / 32768.0f)) < 1e-6f);
    assert(fabsf(last_buffer->samples[4] - (-10000.0f / 32768.0f)) < 1e-6f);

    bad_native_sample = true;
    obs_monad_audio_typed_character('b');
    bad_native_sample = false;
    missing_sample_function = true;
    obs_monad_audio_typed_character('m');
    missing_sample_function = false;
    assert(fill_count == typing_fills_before + 1 &&
           play_count == typing_plays_before + 1);

    const unsigned typing_reads = read_begins;
    owner = false;
    obs_monad_audio_typed_character('x');
    owner = true;
    assert(read_begins == typing_reads && read_ends == typing_reads);

    puts("ok: Monad PCM playback, typed native synthesis, validation, and reuse PASS");
    return 0;
}
