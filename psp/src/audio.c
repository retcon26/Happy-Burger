#include "audio.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "pak.h"
#include "pmap_format.h"

#define OUT_RATE 44100
#define BLOCK 1024 // samples per channel per output block
#define MAX_VOICES 16
#define MAX_SOUNDS 128
#define HEAR_DIST 1800.0f

typedef struct {
    char name[24];
    uint32_t first;
    uint32_t count;
    uint32_t loop;
} SndEntry;

#define MASTER_GAIN 192   // /256: headroom, since every sound is normalised to full scale
#define LIMIT 30000.0f    // the limiter keeps the mix under this instead of clipping
#define ATTACK 0.5f       // per sample: how fast the limiter turns down (about 2 samples)
#define RELEASE 0.00015f  // per sample: how fast it comes back (about a quarter of a second)
#define FADE_LEN 256      // samples (6 ms) to fade out a sound that's cut off, so it doesn't click

typedef struct {
    const short *data;
    uint32_t len;
    uint32_t idx;  // current sample
    uint32_t frac; // and the fraction past it, 0..65535
    uint32_t step; // 16.16 samples per output sample
    int vol_l, vol_r; // 0..256
    int loop;
    int active;
    int serial;
    int sound; // which sound name (first entry of its variants)
    int chan;  // > 0: a channel like CHAN_WEAPON in Source, one sound at a time
    int fade;  // > 0: fading out, samples left
} Voice;

#define MAX_SAME 3 // copies of one sound at once: rapid fire reuses the oldest

static void *g_file;
static const SndEntry *g_sounds;
static int g_num_sounds;
static const short *g_pcm;
static uint32_t g_rate = 22050;

static Voice g_voices[MAX_VOICES];
static int g_serial = 1;
static SceUID g_sema = -1;
static SceUID g_thread = -1;
static int g_channel = -1;
static volatile int g_run;
static short __attribute__((aligned(64))) g_out[2][BLOCK * 2];

static vec3 g_listener;
static float g_listener_yaw;

// Voice chat streams: written by the game thread, read by the mixer
#define STREAMS 4
#define STREAM_LEN 8192   // samples (0.74 s at 11025 Hz); a power of two
#define STREAM_PREBUF 2048 // starts playing once 0.19 s is waiting
#define STREAM_STEP 16384 // 11025 / 44100 in 16.16
#define STREAM_VOL 300    // /256: speech a bit over the game
static short g_stream[STREAMS][STREAM_LEN];
static volatile uint32_t g_stream_w[STREAMS], g_stream_r[STREAMS];
static uint32_t g_stream_frac[STREAMS];
static volatile int g_stream_on[STREAMS];

void audio_stream_push(int s, const short *pcm, int n) {
    if (s < 0 || s >= STREAMS) return;
    uint32_t w = g_stream_w[s];
    if (w + (uint32_t)n - g_stream_r[s] > STREAM_LEN) return; // too far behind: drop it rather than garble
    for (int i = 0; i < n; i++) g_stream[s][(w + (uint32_t)i) & (STREAM_LEN - 1)] = pcm[i];
    g_stream_w[s] = w + (uint32_t)n;
}

int audio_stream_active(int s) {
    return s >= 0 && s < STREAMS && g_stream_on[s];
}

static void mix_streams(int *mix) {
    for (int s = 0; s < STREAMS; s++) {
        uint32_t avail = g_stream_w[s] - g_stream_r[s];
        if (!g_stream_on[s]) {
            if (avail < STREAM_PREBUF) continue;
            g_stream_on[s] = 1;
        }
        uint32_t r = g_stream_r[s], frac = g_stream_frac[s];
        for (int i = 0; i < BLOCK; i++) {
            if (r == g_stream_w[s]) {
                g_stream_on[s] = 0; // ran dry: wait for the next words to build up again
                break;
            }
            int a = g_stream[s][r & (STREAM_LEN - 1)];
            int b = (r + 1 != g_stream_w[s]) ? g_stream[s][(r + 1) & (STREAM_LEN - 1)] : a;
            int v = ((a + (((b - a) * (int)frac) >> 16)) * STREAM_VOL) >> 8;
            mix[i * 2] += v;
            mix[i * 2 + 1] += v;
            frac += STREAM_STEP;
            r += frac >> 16;
            frac &= 0xFFFF;
        }
        g_stream_frac[s] = frac;
        g_stream_r[s] = r;
    }
}

static int mixer_thread(SceSize args, void *argp) {
    int buf = 0;
    static int mix[BLOCK * 2];
    float gain = 1.0f; // limiter
    pspSdkDisableFPUExceptions(); // per thread: see main()
    while (g_run) {
        memset(mix, 0, sizeof(mix));
        sceKernelWaitSema(g_sema, 1, 0);
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice *vc = &g_voices[v];
            if (!vc->active) continue;
            for (int i = 0; i < BLOCK; i++) {
                if (vc->idx >= vc->len) {
                    if (vc->loop) {
                        vc->idx -= vc->len;
                    } else {
                        vc->active = 0;
                        break;
                    }
                }
                // Linear interpolation between neighbouring samples (sounds may be stored at a lower rate)
                uint32_t nx = vc->idx + 1;
                if (nx >= vc->len) nx = vc->loop ? 0 : vc->idx;
                int a = vc->data[vc->idx], b = vc->data[nx];
                int s = a + (((b - a) * (int)vc->frac) >> 16);
                if (vc->fade > 0) {
                    s = s * vc->fade / FADE_LEN;
                    if (--vc->fade == 0) {
                        vc->active = 0;
                        break;
                    }
                }
                mix[i * 2] += (s * vc->vol_l) >> 8;
                mix[i * 2 + 1] += (s * vc->vol_r) >> 8;
                vc->frac += vc->step;
                vc->idx += vc->frac >> 16;
                vc->frac &= 0xFFFF;
            }
        }
        sceKernelSignalSema(g_sema, 1);
        mix_streams(mix);

        // Master volume, then a limiter that follows the mix sample by sample: when overlapping
        // sounds get too loud it turns them down within a couple of samples and brings the level
        // back gradually, so there are no clicks from sudden volume steps and nothing hard-clips.
        short *out = g_out[buf];
        for (int i = 0; i < BLOCK; i++) {
            int l = (mix[i * 2] * MASTER_GAIN) >> 8, r = (mix[i * 2 + 1] * MASTER_GAIN) >> 8;
            int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            float a = (float)(al > ar ? al : ar);
            float want = a > LIMIT ? LIMIT / a : 1.0f;
            if (want < gain) {
                gain += (want - gain) * ATTACK;
            } else {
                gain += (1.0f - gain) * RELEASE;
            }
            int g = (int)(gain * 65536.0f);
            l = (int)(((long long)l * g) >> 16);
            r = (int)(((long long)r * g) >> 16);
            out[i * 2] = (short)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
            out[i * 2 + 1] = (short)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        }
        sceAudioOutputBlocking(g_channel, PSP_AUDIO_VOLUME_MAX, out);
        buf ^= 1;
    }
    return 0;
}

int audio_init(void) {
    const char *path = mem_tier() == MEM_TIER_HIGH ? "data/sounds.psnd" : "data/sounds.low.psnd";
    if (!pak_file_exists(path)) path = "data/sounds.psnd";
    unsigned int size;
    g_file = pak_read_file(path, &size);
    if (!g_file) return -1;
    const PmapHeader *h = g_file;
    if (memcmp(h->magic, "PSND", 4) != 0) {
        free(g_file);
        g_file = NULL;
        return -2;
    }
    const PmapSection *s = pak_find(g_file, PMAP_SECTION_ID('S', 'N', 'D', 'S'));
    const PmapSection *p = pak_find(g_file, PMAP_SECTION_ID('P', 'C', 'M', ' '));
    const PmapSection *r = pak_find(g_file, PMAP_SECTION_ID('R', 'A', 'T', 'E'));
    if (!s || !p) return -3;
    g_sounds = (const SndEntry *)((const char *)g_file + s->offset);
    g_num_sounds = (int)s->count;
    g_pcm = (const short *)((const char *)g_file + p->offset);
    if (r) g_rate = *(const uint32_t *)((const char *)g_file + r->offset);

    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (g_channel < 0) return -4;
    g_sema = sceKernelCreateSema("mix", 0, 1, 1, NULL);
    g_run = 1;
    g_thread = sceKernelCreateThread("mixer", mixer_thread, 0x12, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL);
    return 0;
}

void audio_shutdown(void) {
    if (g_thread >= 0) {
        g_run = 0;
        sceKernelWaitThreadEnd(g_thread, NULL);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_channel >= 0) sceAudioChRelease(g_channel);
    g_channel = -1;
}

int snd_find(const char *name) {
    for (int i = 0; i < g_num_sounds; i++) {
        if (strncmp(g_sounds[i].name, name, sizeof(g_sounds[i].name)) == 0) return i;
    }
    return -1;
}

// Fade a voice out over a few milliseconds instead of cutting it (which clicks)
static void fade_out(Voice *vc) {
    if (vc->active && vc->fade == 0) vc->fade = FADE_LEN;
}

static int start_voice(int id, float vol, float pan, int loop, int chan) {
    if (id < 0 || id >= g_num_sounds || g_channel < 0 || vol <= 0.01f) return -1;
    // Pick a random variant among consecutive entries with the same name
    int n = 1;
    while (id + n < g_num_sounds && strcmp(g_sounds[id + n].name, g_sounds[id].name) == 0) n++;
    const SndEntry *e = &g_sounds[id + (n > 1 ? rand() % n : 0)];

    if (vol > 1.0f) vol = 1.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    sceKernelWaitSema(g_sema, 1, 0);
    // A channel plays one sound at a time, like Source's CHAN_WEAPON: the next shot replaces the last.
    // Otherwise a sound already playing MAX_SAME times loses its oldest copy.
    int same = 0, oldest = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (!v->active || v->fade || v->loop) continue;
        if (chan > 0 && v->chan == chan) {
            fade_out(v);
        } else if (v->sound == id) {
            same++;
            if (oldest < 0 || v->serial < g_voices[oldest].serial) oldest = i;
        }
    }
    if (same >= MAX_SAME) fade_out(&g_voices[oldest]);
    // A free voice, else the quietest one (fading voices count as silent)
    int best = -1;
    for (int i = 0; i < MAX_VOICES && best < 0; i++) {
        if (!g_voices[i].active) best = i;
    }
    if (best < 0) {
        int best_vol = 1 << 30;
        best = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            const Voice *v = &g_voices[i];
            int vv = v->fade ? (v->vol_l + v->vol_r) * v->fade / FADE_LEN : v->vol_l + v->vol_r;
            if (!v->loop && vv < best_vol) {
                best_vol = vv;
                best = i;
            }
        }
    }
    Voice *vc = &g_voices[best];
    vc->data = g_pcm + e->first;
    vc->len = e->count;
    vc->idx = 0;
    vc->frac = 0;
    // Bits 8-11 of the flags: the sound is stored at the pack's rate divided by 2^that (voices)
    vc->step = (uint32_t)(((uint64_t)(g_rate >> ((e->loop >> 8) & 15)) << 16) / OUT_RATE);
    vc->vol_l = (int)(vol * 256.0f * (pan > 0.0f ? 1.0f - pan * 0.8f : 1.0f));
    vc->vol_r = (int)(vol * 256.0f * (pan < 0.0f ? 1.0f + pan * 0.8f : 1.0f));
    vc->loop = loop || (e->loop & 1);
    vc->serial = g_serial++;
    vc->sound = id;
    vc->chan = chan;
    vc->fade = 0;
    vc->active = 1;
    int handle = best | (vc->serial << 8);
    sceKernelSignalSema(g_sema, 1);
    return handle;
}

int snd_play(int id, float vol, float pan) {
    return start_voice(id, vol, pan, 0, 0);
}

int snd_play_chan(int id, float vol, int chan) {
    return start_voice(id, vol, 0.0f, 0, chan);
}

int snd_loop(int id, float vol) {
    return start_voice(id, vol, 0.0f, 1, 0);
}

void snd_stop(int handle) {
    if (handle < 0) return;
    int v = handle & 0xFF;
    if (v >= MAX_VOICES) return;
    sceKernelWaitSema(g_sema, 1, 0);
    if (g_voices[v].active && g_voices[v].serial == (handle >> 8)) fade_out(&g_voices[v]);
    sceKernelSignalSema(g_sema, 1);
}

void snd_set_listener(vec3 pos, float yaw_deg) {
    g_listener = pos;
    g_listener_yaw = yaw_deg;
}

int snd_play_at(int id, vec3 pos, float vol) {
    vec3 d = v3_sub(pos, g_listener);
    float dist = v3_len(d);
    float att = 1.0f - dist / HEAR_DIST;
    if (att <= 0.0f) return -1;
    float yaw = g_listener_yaw * (3.14159265f / 180.0f);
    vec3 right = v3(sinf(yaw), -cosf(yaw), 0.0f);
    float pan = dist > 1.0f ? v3_dot(d, right) / dist : 0.0f;
    return start_voice(id, vol * att * att, pan, 0, 0);
}
