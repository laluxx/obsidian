/// Audio callback contract test seam — no audio server required
#ifndef OBS_AUDIO_CONTRACT_H
#define OBS_AUDIO_CONTRACT_H
#include <stdint.h>
#include <pthread.h>
typedef uint32_t ma_uint32;
typedef uint64_t ma_uint64;
typedef int ma_result;
#define MA_SUCCESS 0
#define MA_ERROR -1
#define ma_device_type_playback 1
#define ma_format_f32 2
typedef struct ma_device ma_device;
typedef struct {
    struct { int format; unsigned channels; } playback;
    unsigned sampleRate;
    unsigned periodSizeInFrames;
    void (*dataCallback)(ma_device*,void*,const void*,ma_uint32);
} ma_device_config;
struct ma_device { ma_device_config config; };
typedef pthread_mutex_t ma_mutex;
typedef struct { int dummy; } ma_decoder;
typedef struct { int dummy; } ma_decoder_config;
ma_device_config ma_device_config_init(int type);
ma_result ma_device_init(void *context, const ma_device_config *config, ma_device *device);
ma_result ma_device_start(ma_device *device);
void ma_device_uninit(ma_device *device);
ma_result ma_mutex_init(ma_mutex *mutex);
void ma_mutex_uninit(ma_mutex *mutex);
void ma_mutex_lock(ma_mutex *mutex);
void ma_mutex_unlock(ma_mutex *mutex);
ma_decoder_config ma_decoder_config_init(int format, unsigned channels, unsigned sample_rate);
ma_result ma_decoder_init_file(const char *path, const ma_decoder_config *config, ma_decoder *decoder);
ma_result ma_decoder_get_length_in_pcm_frames(ma_decoder *decoder, ma_uint64 *frames);
ma_result ma_decoder_read_pcm_frames(ma_decoder *decoder, void *out, ma_uint64 frames, ma_uint64 *read);
void ma_decoder_uninit(ma_decoder *decoder);
#endif
