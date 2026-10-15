/// Exercise real audio mixer and lifetime API with only miniaudio IO mocked
#include "audio.h"
#include "audio_contract.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static ma_device *current;
static ma_uint64 fixture_frames = 12, fixture_decoded = 12;
static int read_result = MA_SUCCESS;
ma_device_config ma_device_config_init(int type) { (void)type; return (ma_device_config){0}; }
ma_result ma_device_init(void *unused, const ma_device_config *config,ma_device *device) {
    (void)unused; device->config=*config; current=device; return MA_SUCCESS;
}
ma_result ma_device_start(ma_device *device) { (void)device; return MA_SUCCESS; }
void ma_device_uninit(ma_device *device) { if (current == device) current=NULL; }
ma_result ma_mutex_init(ma_mutex *mutex) { return pthread_mutex_init(mutex,NULL) == 0 ? MA_SUCCESS : MA_ERROR; }
void ma_mutex_uninit(ma_mutex *mutex) { assert(pthread_mutex_destroy(mutex)==0); }
void ma_mutex_lock(ma_mutex *mutex) { assert(pthread_mutex_lock(mutex)==0); }
void ma_mutex_unlock(ma_mutex *mutex) { assert(pthread_mutex_unlock(mutex)==0); }
ma_decoder_config ma_decoder_config_init(int fmt,unsigned channels,unsigned rate) {
    (void)fmt;(void)channels;(void)rate;return (ma_decoder_config){0};
}
ma_result ma_decoder_init_file(const char *path,const ma_decoder_config *cfg,ma_decoder *dec) {
    (void)cfg;(void)dec;return strcmp(path,"missing") == 0 ? MA_ERROR : MA_SUCCESS;
}
ma_result ma_decoder_get_length_in_pcm_frames(ma_decoder *dec,ma_uint64 *count) {
    (void)dec;*count=fixture_frames;return MA_SUCCESS;
}
ma_result ma_decoder_read_pcm_frames(ma_decoder *dec,void *out,ma_uint64 count,ma_uint64 *read) {
    (void)dec;assert(count==fixture_frames);*(float*)out=0.75f;*read=fixture_decoded;
    return read_result;
}
void ma_decoder_uninit(ma_decoder *dec) {(void)dec;}

int main(void) {
    assert(audio_buffer_create(5)==NULL);
    assert(audio_load_wav("x")==NULL);
    assert(audio_init());
    assert(!audio_buffer_create(0));
    AudioBuffer *buf=audio_buffer_create(4);
    assert(buf && buf->samples && buf->frame_count==4);
    float samples[8]={1,0, 0.5f,0, 0.25f,0, 0.125f,0};
    audio_buffer_fill(buf,samples,4);
    audio_buffer_play(buf);
    float mix[8]={0};
    current->config.dataCallback(current,mix,NULL,4);
    assert(mix[0]==1.f && mix[2]==0.5f && mix[6]==0.125f && !buf->playing);
    buf->frame_count=0;buf->loop=true;audio_buffer_play(buf);
    current->config.dataCallback(current,mix,NULL,4);
    assert(!buf->playing); // No infinite loop on zero-length looping playback
    buf->frame_count=4;buf->loop=false;
    audio_buffer_seek(buf,2);assert(buf->cursor==2);
    audio_buffer_stop(buf);assert(!buf->playing);
    assert(!audio_load_wav("missing"));
    fixture_frames=(ma_uint64)UINT32_MAX+1u;
    assert(!audio_load_wav("huge"));
    fixture_frames=0;
    assert(!audio_load_wav("empty"));
    fixture_frames=12;fixture_decoded=11;
    assert(!audio_load_wav("truncated"));
    fixture_decoded=12;read_result=MA_ERROR;
    assert(!audio_load_wav("broken"));
    read_result=MA_SUCCESS;
    AudioBuffer *loaded=audio_load_wav("valid");
    assert(loaded && loaded->frame_count==12 && loaded->samples[0]==0.75f);
    audio_buffer_destroy(loaded);
    audio_buffer_destroy(buf);
    assert(!audio_buffer_create(0));
    // Full pool is bounded; rejection never returns a dangling unregistered buffer.
    AudioBuffer *all[256];
    for (int i=0;i<256;i++) assert((all[i]=audio_buffer_create(1)));
    assert(!audio_buffer_create(1));
    for (int i=0;i<256;i++) audio_buffer_destroy(all[i]);
    audio_shutdown();
    assert(!audio_buffer_create(1));
    assert(audio_init());
    AudioBuffer *after=audio_buffer_create(1);
    assert(after);
    audio_shutdown();
    audio_buffer_destroy(after); // No mutex use after shutdown
    puts("audio lifetime: zero-loop, decode bounds, mutex, pool cap and restart PASS");
    return 0;
}
