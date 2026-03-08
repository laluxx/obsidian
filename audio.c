#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define MAX_BUFFERS 256

static ma_device  s_device;
static ma_mutex   s_mutex;
static bool       s_initialized = false;

static AudioBuffer *s_buffers[MAX_BUFFERS];
static int          s_buffer_count = 0;

// Called on the audio thread. Mix all playing buffers into `output`.
static void data_callback(ma_device *device, void *output, const void *input,
                           ma_uint32 frame_count)
{
    (void)device; (void)input;

    float *out = (float *)output;
    // Zero the output first
    memset(out, 0, frame_count * AUDIO_CHANNELS * sizeof(float));

    ma_mutex_lock(&s_mutex);
    for (int i = 0; i < s_buffer_count; i++) {
        AudioBuffer *buf = s_buffers[i];
        if (!buf || !buf->playing) continue;

        uint32_t frames_remaining = frame_count;
        uint32_t out_pos = 0;

        while (frames_remaining > 0) {
            uint32_t available = buf->frame_count - buf->cursor;
            uint32_t to_copy   = frames_remaining < available ? frames_remaining : available;

            float *src = buf->samples + buf->cursor * AUDIO_CHANNELS;
            for (uint32_t f = 0; f < to_copy * AUDIO_CHANNELS; f++) {
                out[out_pos * AUDIO_CHANNELS + f] += src[f] * buf->volume;
            }
            buf->cursor       += to_copy;
            out_pos           += to_copy;
            frames_remaining  -= to_copy;

            if (buf->cursor >= buf->frame_count) {
                if (buf->loop) {
                    buf->cursor = 0;
                } else {
                    buf->playing = false;
                    break;
                }
            }
        }
    }
    ma_mutex_unlock(&s_mutex);
}

/// API

bool audio_init(void) {
    if (s_initialized) return true;

    if (ma_mutex_init(&s_mutex) != MA_SUCCESS) return false;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = AUDIO_CHANNELS;
    cfg.sampleRate        = AUDIO_SAMPLE_RATE;
    cfg.dataCallback      = data_callback;
    cfg.periodSizeInFrames = AUDIO_BUFFER_SIZE;

    if (ma_device_init(NULL, &cfg, &s_device) != MA_SUCCESS) {
        ma_mutex_uninit(&s_mutex);
        return false;
    }
    if (ma_device_start(&s_device) != MA_SUCCESS) {
        ma_device_uninit(&s_device);
        ma_mutex_uninit(&s_mutex);
        return false;
    }

    s_initialized = true;
    return true;
}

void audio_shutdown(void) {
    if (!s_initialized) return;
    ma_device_uninit(&s_device);
    ma_mutex_uninit(&s_mutex);
    s_initialized = false;
}

AudioBuffer *audio_buffer_create(uint32_t frame_count) {
    AudioBuffer *buf = calloc(1, sizeof(AudioBuffer));
    buf->samples     = calloc(frame_count * AUDIO_CHANNELS, sizeof(float));
    buf->frame_count = frame_count;
    buf->volume      = 1.0f;

    ma_mutex_lock(&s_mutex);
    if (s_buffer_count < MAX_BUFFERS)
        s_buffers[s_buffer_count++] = buf;
    ma_mutex_unlock(&s_mutex);

    return buf;
}

void audio_buffer_destroy(AudioBuffer *buf) {
    if (!buf) return;
    ma_mutex_lock(&s_mutex);
    for (int i = 0; i < s_buffer_count; i++) {
        if (s_buffers[i] == buf) {
            s_buffers[i] = s_buffers[--s_buffer_count];
            break;
        }
    }
    ma_mutex_unlock(&s_mutex);
    free(buf->samples);
    free(buf);
}

void audio_buffer_fill(AudioBuffer *buf, const float *samples, uint32_t frame_count) {
    if (!buf) return;
    uint32_t copy = frame_count < buf->frame_count ? frame_count : buf->frame_count;
    memcpy(buf->samples, samples, copy * AUDIO_CHANNELS * sizeof(float));
}

void audio_buffer_play(AudioBuffer *buf) {
    if (!buf) return;
    buf->cursor  = 0;
    buf->playing = true;
}

void audio_buffer_stop(AudioBuffer *buf) {
    if (!buf) return;
    buf->playing = false;
}

void audio_buffer_seek(AudioBuffer *buf, uint32_t frame) {
    if (!buf) return;
    buf->cursor = frame < buf->frame_count ? frame : 0;
}

// Minimal WAV loader (PCM 16-bit or 32-bit float)
AudioBuffer *audio_load_wav(const char *path) {
    ma_decoder decoder;
    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);
    if (ma_decoder_init_file(path, &dcfg, &decoder) != MA_SUCCESS) {
        fprintf(stderr, "audio_load_wav: failed to open %s\n", path);
        return NULL;
    }

    ma_uint64 frame_count;
    ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count);

    AudioBuffer *buf = audio_buffer_create((uint32_t)frame_count);
    ma_decoder_read_pcm_frames(&decoder, buf->samples, frame_count, NULL);
    ma_decoder_uninit(&decoder);
    return buf;
}
