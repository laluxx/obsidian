#pragma once
#include <stdint.h>
#include <stdbool.h>

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BUFFER_SIZE   2048   // frames

// A buffer of PCM float samples you fill yourself
typedef struct {
    float   *samples;     // interleaved stereo: [L, R, L, R, ...]
    uint32_t frame_count; // total frames (samples / AUDIO_CHANNELS)
    uint32_t cursor;      // playback position in frames
    bool     loop;
    bool     playing;
    float    volume;      // 0.0 - 1.0
} AudioBuffer;

bool audio_init(void);
void audio_shutdown(void);

AudioBuffer *audio_buffer_create(uint32_t frame_count);
void         audio_buffer_destroy(AudioBuffer *buf);

void audio_buffer_fill(AudioBuffer *buf, const float *samples, uint32_t frame_count);
void audio_buffer_play(AudioBuffer *buf);
void audio_buffer_stop(AudioBuffer *buf);
void audio_buffer_seek(AudioBuffer *buf, uint32_t frame);

AudioBuffer *audio_load_wav(const char *path);
