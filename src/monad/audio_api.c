/// audio_api — Monad-produced mono PCM submitted to the audio backend
#ifdef OBS_MONAD_AUDIO_TEST
#include "audio_api_contract.h"
#else
#include "audio_api.h"
#include "bindings.h"
#include "foreign.h"
#include "effect_policy.h"
#include "native_read.h"
#endif
#include "audio.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define OBS_AUDIO_PCM_MAX_FRAMES (AUDIO_SAMPLE_RATE / 2)

/* A short reusable conversion buffer; no allocation or synthesis runs in the
 * platform callback. Monad owns waveform/envelope generation. */
static int16_t s_pcm[OBS_AUDIO_PCM_MAX_FRAMES];
static AudioBuffer *s_pcm_buffer;

static bool admitted(void *ctx) {
    return obs_host_owner(ctx) &&
        obs_monad_effect_allowed(OBS_EFFECT_WRITE,
                                 OBS_LIFETIME_ENGINE, false);
}

static bool submit_pcm(uint32_t frames) {
    if (!frames || frames > OBS_AUDIO_PCM_MAX_FRAMES) return false;
    AudioBuffer *buffer = s_pcm_buffer;
    bool replace = !buffer || buffer->frame_count != frames;
    if (replace) {
        buffer = audio_buffer_create(frames);
        if (!buffer) return false;
    } else {
        audio_buffer_stop(buffer);
    }
    audio_buffer_fill_mono_i16(buffer, s_pcm, frames);
    buffer->volume = 1.0f;
    audio_buffer_play(buffer);
    if (replace) {
        AudioBuffer *previous = s_pcm_buffer;
        s_pcm_buffer = buffer;
        audio_buffer_destroy(previous);
    }
    return true;
}

static monad_status_t play_samples(monad_object_space_t *space, size_t argc,
    monad_value_t *const *args, monad_value_t **out, void *ctx,
    monad_error_t *error) {
    if (argc != 1 || !args || !out || !admitted(ctx) ||
        monad_value_kind(args[0]) != MONAD_VALUE_CONS)
        return obs_host_invalid(error,
            "audio-play-samples!: requires a non-empty Monad list and engine mutation");

    const monad_value_t *node = args[0];
    uint32_t frames = 0;
    while (monad_value_kind(node) == MONAD_VALUE_CONS) {
        if (frames == OBS_AUDIO_PCM_MAX_FRAMES)
            return obs_host_invalid(error,
                "audio-play-samples!: PCM exceeds the 500 ms limit");
        monad_i64_pair_step_t step =
            monad_value_i64_pair_step_borrowed(node);
        if (!step.cdr || step.car < INT16_MIN || step.car > INT16_MAX)
            return obs_host_invalid(error,
                "audio-play-samples!: expected signed 16-bit integer samples");
        s_pcm[frames++] = (int16_t)step.car;
        node = step.cdr;
    }
    if (!frames || monad_value_kind(node) != MONAD_VALUE_NIL)
        return obs_host_invalid(error,
            "audio-play-samples!: expected a proper non-empty sample list");

    if (!submit_pcm(frames))
        return obs_host_invalid(error,
            "audio-play-samples!: audio buffer allocation failed");
    *out = monad_value_bool(space, true);
    return MONAD_OK;
}

void obs_monad_audio_typed_character(uint32_t codepoint) {
    ObsMonadRuntime *host = obs_monad_engine();
    if (!obs_host_owner(host) || !host->runtime)
        return;
    ObsNativeRead read = {0};
    monad_error_t error = {0};
    if (!obs_native_read_begin(&read, host->runtime, &host->publication,
                               &error))
        return;

    monad_i64_function_t sample_at = NULL, sample_count = NULL;
    bool valid =
        obs_native_read_find_i64(&read, "obsidian_typing_pcm_sample",
                                 &sample_at, &error) &&
        obs_native_read_find_i64(&read, "obsidian_typing_pcm_frames",
                                 &sample_count, &error);
    int64_t frames = valid ? sample_count(0) : 0;
    valid = valid && frames > 0 &&
            frames <= (int64_t)OBS_AUDIO_PCM_MAX_FRAMES;
    for (int64_t frame = 0; valid && frame < frames; ++frame) {
        const int64_t packed = (int64_t)codepoint * 2048 + frame;
        const int64_t sample = sample_at(packed);
        if (sample < INT16_MIN || sample > INT16_MAX) {
            valid = false;
            break;
        }
        s_pcm[frame] = (int16_t)sample;
    }
    obs_native_read_end(&read);
    if (valid) (void)submit_pcm((uint32_t)frames);
}

bool obs_monad_register_audio_api(ObsMonadRuntime *host,
                                  monad_error_t *error) {
    static const ObsHostBinding entries[] = {
        {"audio-play-samples!",
         "Play a proper list of signed 16-bit mono PCM samples at 48 kHz (max 500 ms).",
         1, play_samples, NULL},
    };
    return obs_host_register(host, entries,
        sizeof(entries) / sizeof(entries[0]), error);
}
