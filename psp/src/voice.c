#include "voice.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspsdk.h>
#include <string.h>

#include "audio.h"

#define MIC_GAIN 4      // sceAudioInputInit's gain (hardware); the automatic gain below does the rest
#define CAP_FRAMES 8    // captured frames waiting to be sent
#define AGC_TARGET 14000.0f
#define AGC_MAX 24.0f   // most it turns a quiet mic up
#define GATE 400        // frames quieter than this (after gain) are sent as silence: no hiss between words

static int g_ok;
static volatile int g_run, g_talking;
static SceUID g_thread = -1;
static short __attribute__((aligned(64))) g_cap[CAP_FRAMES][VOICE_FRAME];
static volatile unsigned int g_cap_w, g_cap_r;
static float g_agc = 4.0f;
// Encoder state (continues from frame to frame; each frame also carries it, so a lost packet only
// costs that frame)
static int g_enc_pred, g_enc_index;

static const short k_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const signed char k_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// One ADPCM step: updates *pred and *index for this 4-bit code
static void adpcm_step(int code, int *pred, int *index) {
    int step = k_step[*index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    *pred += (code & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += k_index[code];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
}

static int adpcm_code(int sample, int pred, int index) {
    int step = k_step[index];
    int diff = sample - pred, code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    if (diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
    }
    if (diff >= step >> 2) code |= 1;
    return code;
}

static int capture_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    static short __attribute__((aligned(64))) buf[VOICE_FRAME];
    pspSdkDisableFPUExceptions();
    while (g_run) {
        if (!g_talking) {
            sceKernelDelayThread(20000); // the mic is left alone unless someone's talking
            continue;
        }
        if (sceAudioInputBlocking(VOICE_FRAME, VOICE_RATE, buf) < 0) {
            sceKernelDelayThread(20000);
            continue;
        }
        unsigned int w = g_cap_w;
        if (w - g_cap_r < CAP_FRAMES) { // else the game isn't keeping up: drop it
            memcpy(g_cap[w % CAP_FRAMES], buf, sizeof(buf));
            g_cap_w = w + 1;
        }
    }
    return 0;
}

int voice_init(void) {
    if (g_ok) return 1;
    if (sceAudioInputInit(0, MIC_GAIN, 0) < 0) return 0;
    g_run = 1;
    g_thread = sceKernelCreateThread("voice", capture_thread, 0x14, 0x4000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
        g_run = 0;
        return 0;
    }
    g_ok = 1;
    return 1;
}

int voice_available(void) {
    return g_ok;
}

void voice_shutdown(void) {
    if (!g_ok) return;
    g_talking = 0;
    g_run = 0;
    sceKernelWaitThreadEnd(g_thread, NULL);
    sceKernelDeleteThread(g_thread);
    g_ok = 0;
}

void voice_set_talking(int on) {
    if (!g_ok) return;
    if (on && !g_talking) {
        g_cap_r = g_cap_w; // start fresh
        g_enc_pred = 0;
        g_enc_index = 0;
    }
    g_talking = on;
}

int voice_talking(void) {
    return g_ok && g_talking;
}

int voice_take_frame(VoiceFrame *out) {
    if (!g_ok || g_cap_r == g_cap_w) return 0;
    const short *pcm = g_cap[g_cap_r % CAP_FRAMES];
    // Automatic gain: towards a comfortable level, quickly down and slowly up
    int peak = 1;
    long dc = 0;
    for (int i = 0; i < VOICE_FRAME; i++) dc += pcm[i];
    int mean = (int)(dc / VOICE_FRAME); // the mic's DC offset
    for (int i = 0; i < VOICE_FRAME; i++) {
        int a = pcm[i] - mean;
        if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    float want = AGC_TARGET / (float)peak;
    if (want > AGC_MAX) want = AGC_MAX;
    g_agc += (want - g_agc) * (want < g_agc ? 0.5f : 0.05f);
    int gate = (int)((float)peak * g_agc) < GATE;
    out->predictor = (int16_t)g_enc_pred;
    out->index = (uint8_t)g_enc_index;
    out->pad = 0;
    for (int i = 0; i < VOICE_FRAME; i += 2) {
        int nib[2];
        for (int k = 0; k < 2; k++) {
            int s = gate ? 0 : (int)((float)(pcm[i + k] - mean) * g_agc);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            nib[k] = adpcm_code(s, g_enc_pred, g_enc_index);
            adpcm_step(nib[k], &g_enc_pred, &g_enc_index);
        }
        out->data[i / 2] = (uint8_t)(nib[0] | (nib[1] << 4));
    }
    g_cap_r++;
    return 1;
}

void voice_play(int stream, const VoiceFrame *f) {
    short pcm[VOICE_FRAME];
    int pred = f->predictor, index = f->index > 88 ? 88 : f->index;
    for (int i = 0; i < VOICE_FRAME; i += 2) {
        uint8_t b = f->data[i / 2];
        adpcm_step(b & 15, &pred, &index);
        pcm[i] = (short)pred;
        adpcm_step(b >> 4, &pred, &index);
        pcm[i + 1] = (short)pred;
    }
    audio_stream_push(stream, pcm, VOICE_FRAME);
}
