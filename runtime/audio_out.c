/* audout:u — nn::audio AudioOut, which Bayonetta 3's Wwise uses for its final mix (TOTK only uses the audio renderer,
   audren:u). Commands and layouts follow the public switchbrew description ("Audio services": IAudioOutManager, IAudioOut).
   Each AudioOut is a Core Audio AudioUnit: an appended buffer's samples are copied into a stereo ring; when the device has
   consumed them, the callback gives the buffer back (GetReleasedAudioOutBuffers) and signals the buffer event. On iOS,
   a large hardware callback permits a bounded window of copied PCM to return before physical playback, so three small
   guest buffers can supply one hardware block. Ownership is safe because the complete PCM is already in the host ring;
   credits remain paced by its real read head, and played_samples counts only frames actually read. The extra lookahead
   is about one observed quantum/rate (20 ms for 960 frames at 48 kHz), capped at RING_FRAMES/4; Stop preserves it and
   Flush resets it. 5.1 output is mixed down to stereo (Switch order FL FR FC LFE BL BR).
   BY3_AUDIO=0 keeps the silent real-time sink (each buffer held for its duration) for tests without a sound device. */
#include "runtime.h"
#include "ipc.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

enum { STATE_STARTED = 0, STATE_STOPPED = 1, FORMAT_PCM16 = 2, DEFAULT_RATE = 48000, QUEUE_MAX = 32 };
typedef struct { uint64_t next, sample_buffer, capacity, data_size, data_offset; } AudioOutBuffer;   /* the client's descriptor */
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");
typedef struct { uint64_t tag, end; int copied_pcm; } Pending;
typedef struct { uint64_t tag, bytes; } Queued;
typedef struct {
    pthread_mutex_t lock; pthread_t thread; int running, silent;
    uint32_t rate, channels, event; _Atomic uint32_t state; _Atomic float volume;
    AudioQueueRef aq;                                  /* macOS only: kept for older builds */
    AudioComponentInstance unit; int unit_ok;          /* iOS/macOS: the AudioUnit output (RemoteIO on iOS) */
    float *ring; _Atomic uint64_t wr, rd;              /* interleaved stereo float ring (the AudioUnit path) */
    Pending pending[QUEUE_MAX]; unsigned npending;   /* appended buffers waiting for the play head */
    Queued queue[QUEUE_MAX]; unsigned nqueue;          /* appended, not yet played (head first) */
    uint64_t released[QUEUE_MAX]; unsigned nreleased;  /* played, not yet collected by GetReleasedAudioOutBuffers */
    uint64_t played_samples;
    int diagnostics; uint64_t diagnostic_last_ns, diagnostic_appends, diagnostic_bytes;
    uint64_t diagnostic_starts, diagnostic_stops, diagnostic_callbacks, diagnostic_silent_completions;
    uint64_t diagnostic_pcm_samples, diagnostic_pcm_nonzero, diagnostic_pcm_energy;
    uint32_t diagnostic_pcm_maxabs;
    uint64_t diagnostic_output_samples, diagnostic_output_nonzero, diagnostic_output_energy;
    uint32_t diagnostic_output_maxabs;
    char diagnostic_ch_rms[256];
    float hold_l, hold_r; _Atomic uint64_t diag_pull_frames, diag_pull_nonzero, diag_pull_underruns;
    _Atomic uint32_t diag_pull_slice;
    uint32_t adaptive_ahead; uint64_t diagnostic_early_releases; /* queue lock; iOS only, bounded by RING_FRAMES/4 */
    uint64_t diagnostic_samples_out; float diagnostic_maxabs_out;
} AudioOut;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void need(int ok, const char *what) { if (!ok) runtime_fail(what); }

enum { RING_FRAMES = 65536 };   /* 1.36 s at 48 kHz */
/* Holding o->lock. Keep the largest observed large quantum until Flush: hardware
   can alternate short and long callbacks. A short callback must not discard the
   cushion needed by the next long one. The never-large 256/512 path is unchanged. */
static uint32_t audio_out_ahead_locked(AudioOut *o) {
#if TARGET_OS_IPHONE
    uint32_t quantum = atomic_load_explicit(&o->diag_pull_slice, memory_order_relaxed);
    if (quantum >= 768) {
        if (quantum > RING_FRAMES / 4) quantum = RING_FRAMES / 4;
        if (quantum > o->adaptive_ahead) o->adaptive_ahead = quantum;
    }
    return atomic_load_explicit(&o->state, memory_order_acquire) == STATE_STARTED ? o->adaptive_ahead : 0;
#else
    (void)o; return 0;
#endif
}
/* Holding o->lock, after any newly copied PCM has been published through wr.
   Returning a copied tag never advances rd/played_samples. Zero-frame tags are
   callback-paced so repeatedly appending an empty buffer cannot spin a producer. */
static int release_pending_locked(AudioOut *o, uint64_t played, int from_callback) {
    uint64_t limit = played + audio_out_ahead_locked(o);
    int released = 0;
    while (o->npending && o->pending[0].end <= limit && o->nreleased < QUEUE_MAX) {
        Pending p = o->pending[0];
        if (!p.copied_pcm && (!from_callback || p.end > played)) break;
        o->released[o->nreleased++] = p.tag;
        memmove(o->pending, o->pending + 1, (--o->npending) * sizeof o->pending[0]);
        if (p.end > played) o->diagnostic_early_releases++;
        released++;
    }
    return released;
}
static void audio_out_diagnostic(AudioOut *o, const char *event, int first);
static OSStatus au_pull(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    AudioOut *o = user; float *out = io->mBuffers[0].mData;
    atomic_store_explicit(&o->diag_pull_slice, frames, memory_order_relaxed);
    if (atomic_load_explicit(&o->state, memory_order_acquire) != STATE_STARTED) {
        memset(out, 0, (size_t)frames * 2 * sizeof(float)); return noErr;
    }
    uint64_t rd = atomic_load_explicit(&o->rd, memory_order_relaxed), wr = atomic_load_explicit(&o->wr, memory_order_acquire);
    uint64_t have = wr - rd;
    UInt32 n = frames < have ? frames : (UInt32)have;
    float gain = atomic_load_explicit(&o->volume, memory_order_relaxed), l = o->hold_l, r = o->hold_r;
    uint64_t nonzero = 0;
    for (UInt32 i = 0; i < n; i++) {
        uint64_t k = (rd + i) % RING_FRAMES;
        l = o->ring[2*k]; r = o->ring[2*k+1];
        out[2*i] = l * gain; out[2*i+1] = r * gain;
        nonzero += out[2*i] != 0.f || out[2*i+1] != 0.f;
    }
    /* A short read holds the last sample actually copied, never uninitialized output storage.
       This concealment does not consume source frames or complete additional guest buffers. */
    for (UInt32 i = n; i < frames; i++) { out[2*i] = l * gain; out[2*i+1] = r * gain; l *= 0.9995f; r *= 0.9995f; }
    o->hold_l = l; o->hold_r = r;
    atomic_fetch_add_explicit(&o->diag_pull_frames, frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&o->diag_pull_nonzero, nonzero, memory_order_relaxed);
    if (n < frames) atomic_fetch_add_explicit(&o->diag_pull_underruns, 1, memory_order_relaxed);
    atomic_store_explicit(&o->rd, rd + n, memory_order_release);
    pthread_mutex_lock(&o->lock);
    o->played_samples += n;
    int released = release_pending_locked(o, rd + n, 1);
    if (o->diagnostics && released) o->diagnostic_callbacks++;   /* no logging or formatting on the AudioUnit thread */
    pthread_mutex_unlock(&o->lock);
    if (released) runtime_event_signal(o->event);
    return noErr;
}
static void open_unit(AudioOut *o) {
    if (!o->ring) { o->ring = calloc((size_t)RING_FRAMES * 2, sizeof(float)); need(o->ring != NULL, "AudioOut ring buffer"); }
#if TARGET_OS_IPHONE
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
#else
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
#endif
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &o->unit)) { fprintf(stderr, "[totk-audio] AudioOut: no AudioUnit output; continuing silent\n"); return; }
    AudioStreamBasicDescription f = {(Float64)o->rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {au_pull, o};
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(o->unit)) {
        fprintf(stderr, "[totk-audio] AudioOut: AudioUnit setup failed; continuing silent\n"); AudioComponentInstanceDispose(o->unit); o->unit = NULL; return;
    }
    o->unit_ok = 1;
    fprintf(stderr, "[totk-audio] AudioOut %u Hz stereo via AudioUnit (RemoteIO/DefaultOutput)\n", o->rate);
}
/* Called with the existing queue lock held; reports bounded input-PCM statistics accumulated at append.
   First lifecycle events plus at most one periodic report every five seconds. */
static void audio_out_diagnostic(AudioOut *o, const char *event, int first) {
    if (!o->diagnostics) return;
    uint64_t now = now_ns();
    if (!first && o->diagnostic_last_ns && now - o->diagnostic_last_ns < 5000000000ull) return;
    o->diagnostic_last_ns = now;
    fprintf(stderr, "[totk-audio-diag] AudioUnit unit=%p ok=%d written=%llu read=%llu pending=%u pull_frames=%llu pull_nonzero=%llu pull_underruns=%llu pull_slice=%u lookahead_frames=%u early_releases=%llu\n", (void *)o->unit, o->unit_ok, (unsigned long long)atomic_load(&o->wr), (unsigned long long)atomic_load(&o->rd), o->npending, (unsigned long long)atomic_load(&o->diag_pull_frames), (unsigned long long)atomic_load(&o->diag_pull_nonzero), (unsigned long long)atomic_load(&o->diag_pull_underruns), atomic_load(&o->diag_pull_slice), o->adaptive_ahead, (unsigned long long)o->diagnostic_early_releases);
    fprintf(stderr, "[totk-audio-diag] AudioOut event=%u reason=%s state=%u rate=%u channels=%u silent=%d volume=%.6g queued=%u released=%u starts=%llu stops=%llu appends=%llu bytes=%llu callbacks=%llu silent_completions=%llu consumed_samples=%llu pcm_sampled=%llu pcm_nonzero=%llu pcm_maxabs=%u pcm_energy=%llu pcm_rms=%.8g pcm_max_per_append=2048 output_sampled=%llu output_nonzero=%llu output_maxabs=%u output_energy=%llu output_rms=%.8g output_max_per_append=2048\n",
            o->event, event, o->state, o->rate, o->channels, o->silent, o->volume, o->nqueue, o->nreleased,
            (unsigned long long)o->diagnostic_starts, (unsigned long long)o->diagnostic_stops,
            (unsigned long long)o->diagnostic_appends, (unsigned long long)o->diagnostic_bytes,
            (unsigned long long)o->diagnostic_callbacks, (unsigned long long)o->diagnostic_silent_completions,
            (unsigned long long)o->played_samples, (unsigned long long)o->diagnostic_pcm_samples,
            (unsigned long long)o->diagnostic_pcm_nonzero, o->diagnostic_pcm_maxabs,
            (unsigned long long)o->diagnostic_pcm_energy,
            o->diagnostic_pcm_samples ? sqrt((double)o->diagnostic_pcm_energy / (double)o->diagnostic_pcm_samples) / 32768.0 : 0.0,
            (unsigned long long)o->diagnostic_output_samples, (unsigned long long)o->diagnostic_output_nonzero,
            o->diagnostic_output_maxabs, (unsigned long long)o->diagnostic_output_energy,
            o->diagnostic_output_samples ? sqrt((double)o->diagnostic_output_energy / (double)o->diagnostic_output_samples) / 32768.0 : 0.0);
    o->diagnostic_pcm_samples = o->diagnostic_pcm_nonzero = o->diagnostic_pcm_energy = 0;
    o->diagnostic_ch_rms[0] = 0;
    o->diagnostic_pcm_maxabs = 0;
    o->diagnostic_output_samples = o->diagnostic_output_nonzero = o->diagnostic_output_energy = 0;
    o->diagnostic_output_maxabs = 0;
}

/* Debug only: inspect at most the first 2048 complete int16 samples of this descriptor,
   including short/unaligned buffers; never change the PCM or inspect beyond data_size.
   The queue lock is held so callbacks and reports see one consistent aggregate. */
static void audio_out_diagnostic_pcm(AudioOut *o, const AudioOutBuffer *buffer) {
    uint64_t count = buffer->data_size / sizeof(int16_t);
    if (count > 2048) count = 2048;
    if (!count) return;
    const uint8_t *pcm = ipc_guest(buffer->sample_buffer + buffer->data_offset, count * sizeof(int16_t));
    for (uint64_t i = 0; i < count; i++) {
        int16_t sample; memcpy(&sample, pcm + i * sizeof sample, sizeof sample);
        int32_t value = sample;
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude > o->diagnostic_pcm_maxabs) o->diagnostic_pcm_maxabs = magnitude;
        { static uint64_t ch_energy[8], ch_samples[8]; unsigned ch = o->channels ? (unsigned)(i % (o->channels >= 1 && o->channels <= 8 ? o->channels : 1)) : 0;
          if (ch < 8) { ch_energy[ch] += (uint64_t)magnitude * magnitude; ch_samples[ch]++;
            if ((i & 2047) == 2047 || i + 1 == count) {
              char *dst = o->diagnostic_ch_rms; size_t off = strlen(dst);
              double r = ch_samples[ch] ? sqrt((double)ch_energy[ch] / (double)ch_samples[ch]) / 32768.0 : 0.0;
              if (off < sizeof o->diagnostic_ch_rms - 16) snprintf(dst + off, sizeof o->diagnostic_ch_rms - off, "%u:%.4f ", ch, r);
            } } }
        o->diagnostic_pcm_nonzero += value != 0;
        o->diagnostic_pcm_energy += (uint64_t)((int64_t)value * value);
    }
    o->diagnostic_pcm_samples += count;
}

/* Inspect the actual stereo PCM about to be enqueued, without writing it. Output storage is
   the AudioQueue allocation and the bound is its initialized mAudioDataByteSize, not capacity. */

/* The head of the queue has been played: hand its tag back. Called with the lock held. */
static void release_head(AudioOut *o, int played) {
    if (!o->nqueue) return;
    const Queued head = o->queue[0];
    memmove(o->queue, o->queue + 1, (--o->nqueue) * sizeof *o->queue);
    if (o->nreleased < QUEUE_MAX) o->released[o->nreleased++] = head.tag;
    if (played) o->played_samples += head.bytes / (2ull * o->channels);
}
static void *silent_playback(void *arg) {
    AudioOut *o = arg; uint64_t due = 0;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (!o->running) { pthread_mutex_unlock(&o->lock); return NULL; }
        uint64_t now = now_ns();
        if (o->state == STATE_STARTED && o->nqueue) {
            if (!due) due = now + o->queue[0].bytes / (2ull * o->channels) * 1000000000ull / o->rate;
            if (now >= due) {
                release_head(o, 1); due = 0;
                if (o->diagnostics) { o->diagnostic_silent_completions++; audio_out_diagnostic(o, "silent-completion", o->diagnostic_silent_completions == 1); }
                pthread_mutex_unlock(&o->lock); runtime_event_signal(o->event); continue;
            }
        } else due = 0;
        pthread_mutex_unlock(&o->lock);
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
}
static void open_device(AudioOut *o) {
    const char *e = getenv("BY3_AUDIO");
    o->silent = e && !strcmp(e, "0");
    if (!o->silent) {
        AudioStreamBasicDescription f = {.mSampleRate = o->rate, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4, .mFramesPerPacket = 1,
            .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
        (void)f; open_unit(o); if (!o->unit_ok) { fprintf(stderr, "[totk-audio] AudioOut: no Core Audio output; continuing silent\n"); o->silent = 1; }
    }
    if (o->silent) need(pthread_create(&o->thread, NULL, silent_playback, o) == 0, "AudioOut playback thread");
    { const char *t = getenv("TOTK_AUDIO_TEST");   /* a 440/660 Hz test tone straight into the queue: is the output audible at all */
      if (t && *t && o->aq) {
          static int done; if (!done) { done = 1;
              AudioQueueStart(o->aq, NULL);
              for (int k = 0; k < 8; k++) {
                  AudioQueueBufferRef qb = NULL;
                  if (AudioQueueAllocateBuffer(o->aq, 48000, &qb) || !qb) break;
                  int16_t *pcm = qb->mAudioData; double f = 440.0 * (k % 2 ? 1.5 : 1.0);
                  for (int i = 0; i < 12000; i++) { int16_t v = (int16_t)(12000 * sin(2.0 * M_PI * f * i / 48000.0)); pcm[2*i] = v; pcm[2*i+1] = v; }
                  qb->mAudioDataByteSize = 48000; AudioQueueEnqueueBuffer(o->aq, qb, 0, NULL);
              }
              fprintf(stderr, "[totk-audio] TOTK_AUDIO_TEST: enqueued 8 buffers of a test tone\n");
          } }
    }
}
/* Interleaved PCM16 of the client's layout -> stereo PCM16 (the queue's format). */
static void to_stereo(const int16_t *in, int16_t *out, uint64_t frames, uint32_t channels) {
    for (uint64_t i = 0; i < frames; i++) {
        const int16_t *s = in + i * channels;
        if (channels == 2) { out[2 * i] = s[0]; out[2 * i + 1] = s[1]; continue; }
        if (channels == 1) { out[2 * i] = out[2 * i + 1] = s[0]; continue; }
        float c = s[2] * 0.7071f, l = s[0] + c + s[4] * 0.7071f, r = s[1] + c + s[5] * 0.7071f;   /* ITU-style 5.1 downmix, LFE left out */
        l *= 0.5f; r *= 0.5f;
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

static const IpcBuffer *in_buffer(const IpcRequest *rq) { return rq->na && rq->a[0].size ? &rq->a[0] : rq->nx && rq->x[0].size ? &rq->x[0] : NULL; }
static const IpcBuffer *out_buffer(const IpcRequest *rq, int *pointer) {
    if (rq->nb && rq->b[0].size) { *pointer = 0; return &rq->b[0]; }
    if (rq->nc && rq->c[0].size) { *pointer = 1; return &rq->c[0]; }
    return NULL;
}
static void append(AudioOut *o, const IpcRequest *rq) {
    const IpcBuffer *b = in_buffer(rq); need(b && b->size >= sizeof(AudioOutBuffer), "AppendAudioOutBuffer descriptor");
    AudioOutBuffer d; memcpy(&d, ipc_guest(b->addr, sizeof d), sizeof d);
    uint64_t tag = IPC_ARG(rq, uint64_t, 0);
    need(d.data_offset + d.data_size <= d.capacity || !d.capacity, "AudioOut buffer bounds");
    pthread_mutex_lock(&o->lock);
#if TARGET_OS_IPHONE
    /* Flush returns all unreleased tags into the same bounded released array;
       reserve that space before either admitting PCM or dropping a descriptor. */
    if (o->unit_ok) need(o->npending + o->nreleased < QUEUE_MAX, "AudioOut outstanding buffer queue full");
#endif
    if (!o->unit_ok) { need(o->nqueue < QUEUE_MAX, "AudioOut queue full"); o->queue[o->nqueue++] = (Queued){tag, d.data_size}; }
    if (o->diagnostics) {
        o->diagnostic_appends++; o->diagnostic_bytes += d.data_size;
        audio_out_diagnostic_pcm(o, &d);
        audio_out_diagnostic(o, "append", o->diagnostic_appends == 1);
    }
    if (o->silent) { pthread_mutex_unlock(&o->lock); return; }
    uint64_t frames = d.data_size / (2ull * o->channels);
    /* int16 PCM of the client's layout -> stereo float into the ring; the AudioUnit's callback pulls it */
    uint64_t wr = atomic_load_explicit(&o->wr, memory_order_relaxed), rd = atomic_load_explicit(&o->rd, memory_order_acquire);
    if (o->unit_ok && wr - rd + frames > RING_FRAMES) {   /* the device stalled: drop this buffer (counted) */
        if (o->diagnostics) o->diagnostic_silent_completions++;
        need(o->nreleased < QUEUE_MAX, "AudioOut released queue full");
        o->released[o->nreleased++] = tag;   /* return the dropped descriptor, not an unrelated silent-queue head */
        pthread_mutex_unlock(&o->lock);
        runtime_event_signal(o->event);
        return;
    }
    uint64_t w = wr;
    const int16_t *in = ipc_guest(d.sample_buffer + d.data_offset, d.data_size);
    for (uint64_t i = 0; i < frames; i++) {
        int16_t tmp[2];
        to_stereo(in + i * o->channels, tmp, 1, o->channels);
        uint64_t k = (w + i) % RING_FRAMES;
        o->ring[2*k] = tmp[0] / 32768.0f; o->ring[2*k+1] = tmp[1] / 32768.0f;
    }
    need(o->npending < QUEUE_MAX, "AudioOut pending queue full");
    o->pending[o->npending++] = (Pending){tag, w + frames, frames != 0};
    /* Publish the completion metadata before the PCM: a callback that sees these frames
       must also find their descriptor, without waiting for a second hardware callback. */
    atomic_store_explicit(&o->wr, w + frames, memory_order_release);
    int early = 0;
#if TARGET_OS_IPHONE
    if (frames && o->unit_ok) early = release_pending_locked(o, atomic_load_explicit(&o->rd, memory_order_acquire), 0);
#endif
    if (o->diagnostics) { float maxabs = 0; for (uint64_t i = 0; i < frames; i++) { float a = fabsf(o->ring[2*((w+i)%RING_FRAMES)]); if (a > maxabs) maxabs = a; } o->diagnostic_samples_out += frames; o->diagnostic_maxabs_out = maxabs > o->diagnostic_maxabs_out ? maxabs : o->diagnostic_maxabs_out; }
    pthread_mutex_unlock(&o->lock);
    if (early) runtime_event_signal(o->event);
}
static void released(AudioOut *o, const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer); need(b != NULL, "GetReleasedAudioOutBuffers buffer");
    uint64_t *out = ipc_guest(b->addr, b->size); uint32_t n = 0;
    pthread_mutex_lock(&o->lock);
    while (n < o->nreleased && (n + 1) * 8 <= b->size) { out[n] = o->released[n]; n++; }
    memmove(o->released, o->released + n, (o->nreleased - n) * sizeof *o->released); o->nreleased -= n;
    if (o->diagnostics) audio_out_diagnostic(o, "released-query", 0);
    pthread_mutex_unlock(&o->lock);
    if (n < b->size / 8) out[n] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, (uint64_t)n * 8, 0};
    IPC_OUT(rs, n);
}
static void set_state(AudioOut *o, uint32_t state) {
    atomic_store_explicit(&o->state, state, memory_order_release);
    /* The AudioUnit must be started/stopped by the control thread, never while holding
       the queue lock: AudioOutputUnitStop can wait for a callback that needs that lock. */
    if (o->unit_ok) {
        OSStatus status = state == STATE_STARTED ? AudioOutputUnitStart(o->unit) : AudioOutputUnitStop(o->unit);
        if (status) { atomic_store_explicit(&o->state, STATE_STOPPED, memory_order_release); fprintf(stderr, "[totk-audio] AudioUnit %s failed (%d)\n", state == STATE_STARTED ? "start" : "stop", (int)status); }
    }
    pthread_mutex_lock(&o->lock);
    int early = 0;
#if TARGET_OS_IPHONE
    if (state == STATE_STARTED && o->unit_ok && atomic_load_explicit(&o->state, memory_order_acquire) == STATE_STARTED)
        early = release_pending_locked(o, atomic_load_explicit(&o->rd, memory_order_acquire), 0);
#endif
    if (o->diagnostics) {
        uint64_t *count = state == STATE_STARTED ? &o->diagnostic_starts : &o->diagnostic_stops;
        (*count)++; audio_out_diagnostic(o, state == STATE_STARTED ? "start" : "stop", *count == 1);
    }
    pthread_mutex_unlock(&o->lock);
    if (early) runtime_event_signal(o->event);
    if (o->aq) { if (state == STATE_STARTED) AudioQueueStart(o->aq, NULL); else AudioQueuePause(o->aq); }
}
static void audio_out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    if (o->diagnostics) { pthread_mutex_lock(&o->lock); audio_out_diagnostic(o, "request", 0); pthread_mutex_unlock(&o->lock); }
    switch (rq->cmd) {
    case 0: { uint32_t v = o->state; IPC_OUT(rs, v); return; }                                   /* GetAudioOutState */
    case 1: set_state(o, STATE_STARTED); return;                                                /* Start */
    case 2: set_state(o, STATE_STOPPED); return;                                                /* Stop */
    case 3: case 7: append(o, rq); return;                                                     /* AppendAudioOutBuffer(Auto) */
    case 4: rs->copy[rs->ncopy++] = runtime_event_readable(o->event); return;                   /* RegisterBufferEvent */
    case 5: case 8: released(o, rq, rs); return;                                               /* GetReleasedAudioOutBuffers(Auto) */
    case 6: {                                                                                   /* ContainsAudioOutBuffer(tag) */
        uint64_t tag = IPC_ARG(rq, uint64_t, 0); uint8_t found = 0;
        pthread_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->npending; i++) found |= o->pending[i].tag == tag;
        for (unsigned i = 0; i < o->nqueue; i++) found |= o->queue[i].tag == tag;
        for (unsigned i = 0; i < o->nreleased; i++) found |= o->released[i] == tag;
        pthread_mutex_unlock(&o->lock); IPC_OUT(rs, found); return;
    }
    case 9: { pthread_mutex_lock(&o->lock); uint32_t v = o->unit_ok ? o->npending : o->nqueue; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutBufferCount */
    case 10: { pthread_mutex_lock(&o->lock); uint64_t v = o->played_samples; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutPlayedSampleCount */
    case 11: {                                                                                  /* FlushAudioOutBuffers */
        uint32_t previous = atomic_exchange_explicit(&o->state, STATE_STOPPED, memory_order_acq_rel);
        if (o->unit_ok) { OSStatus status = AudioOutputUnitStop(o->unit); need(!status, "AudioOut stop before flush"); }
        pthread_mutex_lock(&o->lock); uint8_t any = o->nqueue != 0 || o->npending != 0 ||
            atomic_load_explicit(&o->wr, memory_order_acquire) != atomic_load_explicit(&o->rd, memory_order_acquire);
        while (o->nqueue) release_head(o, 0);
        need(o->nreleased + o->npending <= QUEUE_MAX, "AudioOut released queue full during flush");
        for (unsigned i = 0; i < o->npending; i++) o->released[o->nreleased++] = o->pending[i].tag;
        o->npending = 0;
        atomic_store_explicit(&o->rd, atomic_load_explicit(&o->wr, memory_order_acquire), memory_order_release);
        o->hold_l = o->hold_r = 0;
        o->adaptive_ahead = 0; atomic_store_explicit(&o->diag_pull_slice, 0, memory_order_relaxed);
        pthread_mutex_unlock(&o->lock);
        if (o->aq) AudioQueueReset(o->aq);   /* its callbacks then find nothing left to release */
        atomic_store_explicit(&o->state, previous, memory_order_release);
        if (o->unit_ok && previous == STATE_STARTED) { OSStatus status = AudioOutputUnitStart(o->unit); need(!status, "AudioOut restart after flush"); }
        if (any) runtime_event_signal(o->event);
        IPC_OUT(rs, any); return;
    }
    case 12: o->volume = IPC_ARG(rq, float, 0); if (o->aq) AudioQueueSetParameter(o->aq, kAudioQueueParam_Volume, o->volume); return;   /* SetAudioOutVolume */
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }                                   /* GetAudioOutVolume */
    default: ipc_unimplemented(s, rq);
    }
}
static void audio_out_close(void *p) {
    AudioOut *o = p;
    atomic_store_explicit(&o->state, STATE_STOPPED, memory_order_release);
    pthread_mutex_lock(&o->lock); o->running = 0; pthread_mutex_unlock(&o->lock);
    if (o->unit) { AudioOutputUnitStop(o->unit); AudioUnitUninitialize(o->unit); AudioComponentInstanceDispose(o->unit); }
    if (o->aq) AudioQueueDispose(o->aq, true);
    if (o->silent) pthread_join(o->thread, NULL);
    free(o->ring); pthread_mutex_destroy(&o->lock); free(o);
}

static const char device_name[] = "DeviceOut";
static void write_name(const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer);
    if (!b) return;
    char *out = ipc_guest(b->addr, b->size); size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memcpy(out, device_name, n); if (n) out[n - 1] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, n, 0};
}
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: { write_name(rq, rs); uint32_t n = 1; IPC_OUT(rs, n); return; }            /* ListAudioOuts(Auto) */
    case 1: case 3: {                                                                           /* OpenAudioOut(Auto) */
        uint32_t rate = IPC_ARG(rq, uint32_t, 0); uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = DEFAULT_RATE;
        if (!channels) channels = 2;
        if (rate != DEFAULT_RATE || (channels != 1 && channels != 2 && channels != 6)) {
            fprintf(stderr, "[totk-audio] OpenAudioOut %u Hz x%u\n", rate, channels); runtime_fail("AudioOut format not supported");
        }
        AudioOut *o = calloc(1, sizeof *o); need(o != NULL, "AudioOut allocation");
        const char *diagnostics = getenv("TOTK_AUDIO_DIAG");
        o->diagnostics = diagnostics && !strcmp(diagnostics, "1");
        pthread_mutex_init(&o->lock, NULL); o->rate = rate; o->channels = channels; o->state = STATE_STOPPED; o->volume = 1.0f;
        o->event = runtime_event_new(); o->running = 1;
        open_device(o);
        if (o->diagnostics) { pthread_mutex_lock(&o->lock); audio_out_diagnostic(o, "open", 1); pthread_mutex_unlock(&o->lock); }
        write_name(rq, rs);
        uint32_t out[4] = {rate, channels, FORMAT_PCM16, STATE_STOPPED}; ipc_out(rs, out, sizeof out);
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioOut", audio_out_handler, o, audio_out_close);
        fprintf(stderr, "[totk-audio] AudioOut open %u Hz x%u PCM16 -> %s\n", rate, channels, o->silent ? "silent sink" : "Core Audio (stereo)");
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_audio_out(void) { runtime_ipc_register("audout:u", manager_handler, NULL, NULL); }
/* iOS audio session interruption: the TOTK-iPhone frontend calls this after an interruption ends.
   The GTA audio path is AudioQueue-based; macOS/iOS resume the queue's callbacks, so this is a
   bookkeeping hook (recreate is ignored for now). */
void runtime_audio_output_restart(int recreate) {
    (void)recreate;
    fprintf(stderr, "[totk-audio] iOS output restart requested (recreate=%d): AudioQueue resumes by itself\n", recreate);
}
