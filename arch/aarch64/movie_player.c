#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#include "movie_player.h"

#define NUM_VFRAMES 16
#define NUM_ABUFS 8
#define ABUF_SAMPLES 4096
#define AUDIO_PREFILL 3

typedef struct { uint8_t *y, *u, *v; double pts; } VideoFrame;

static struct {
    volatile int active, paused, stop_req, decode_eof;
    pthread_t thread;
    int thread_running;
    AVFormatContext *fmt; AVCodecContext *vctx, *actx; SwrContext *swr;
    int vstream, astream; double video_tb;
    int width, height;
    VideoFrame frames[NUM_VFRAMES];
    int frame_read, frame_write, frame_count;
    pthread_mutex_t lock; pthread_cond_t can_produce;
    int has_audio, audio_chunks, audio_primed, audio_rate;
    ALuint al_source, al_buffers[NUM_ABUFS];
    int al_buf_free[NUM_ABUFS], al_buf_samples[NUM_ABUFS];
    uint64_t samples_played;
    int16_t *pcm_acc; int pcm_acc_samples;
    uint64_t start_tick, pause_tick, paused_ticks, tail_tick;
    double tail_base;
    int frame_shown;
} mp;

extern int ctw_screen_w(void);
extern int ctw_screen_h(void);

void movie_set_text(const char *text) { }
void movie_set_text_scale(int scale) { }

static double movie_clock(void) {
    if (mp.has_audio) {
        if (!mp.audio_primed) return -1.0;
        ALint off = 0;
        pthread_mutex_lock(&mp.lock);
        alGetSourcei(mp.al_source, AL_SAMPLE_OFFSET, &off);
        double clk = (double)(mp.samples_played + (uint64_t)off) / (double)mp.audio_rate;
        if (mp.decode_eof) {
            ALint state = 0;
            alGetSourcei(mp.al_source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                const uint64_t now = SDL_GetTicks();
                if (!mp.tail_tick) { mp.tail_tick = now; mp.tail_base = clk; }
                clk = mp.tail_base + (double)(now - mp.tail_tick) / 1000.0;
            }
        }
        pthread_mutex_unlock(&mp.lock);
        return clk;
    }
    const uint64_t now = mp.paused ? mp.pause_tick : SDL_GetTicks();
    return (double)(now - mp.start_tick - mp.paused_ticks) / 1000.0;
}

static void push_video_frame(const AVFrame *frm) {
    pthread_mutex_lock(&mp.lock);
    while (mp.frame_count == NUM_VFRAMES && !mp.stop_req)
        pthread_cond_wait(&mp.can_produce, &mp.lock);
    if (mp.stop_req) { pthread_mutex_unlock(&mp.lock); return; }
    VideoFrame *slot = &mp.frames[mp.frame_write];
    int64_t ts = frm->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frm->pts;
    slot->pts = (ts == AV_NOPTS_VALUE) ? 0.0 : (double)ts * mp.video_tb;
    mp.frame_write = (mp.frame_write + 1) % NUM_VFRAMES;
    mp.frame_count++;
    pthread_mutex_unlock(&mp.lock);
}

static void *decoder_main(void *arg) { return NULL; }
void movie_stop(void) {
    if (!mp.active) return;
    mp.stop_req = 1;
    pthread_mutex_lock(&mp.lock);
    pthread_cond_broadcast(&mp.can_produce);
    pthread_mutex_unlock(&mp.lock);
    if (mp.thread_running) pthread_join(mp.thread, NULL);
    // free session...
}

int movie_play(const char *path, int skippable) {
    pthread_mutex_init(&mp.lock, NULL);
    pthread_cond_init(&mp.can_produce, NULL);
    pthread_create(&mp.thread, NULL, decoder_main, NULL);
    return 1;
}

void movie_skip(void) { if (mp.active) movie_stop(); }
int movie_is_playing(void) { return mp.active; }

unsigned int eglSwapBuffersHook(void *display, void *surface) {
    return eglSwapBuffers((EGLDisplay)display, (EGLSurface)surface);
}

void movie_main_loop_tick(void) { }
