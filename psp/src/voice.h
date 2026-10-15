#pragma once

#include <stdint.h>

// Voice chat: the PSP's microphone (the PSP-3000's built-in one, or a headset's), push to talk.
// The mic is only opened while talking. Audio is captured at 11025 Hz and packed with IMA ADPCM
// (4 bits a sample) into frames of VOICE_FRAME samples for the network; game.c sends and relays them.

#define VOICE_RATE 11025
#define VOICE_FRAME 512                     // samples per frame (46 ms)
#define VOICE_FRAME_BYTES (VOICE_FRAME / 2) // ADPCM data per frame

typedef struct {
    int16_t predictor; // ADPCM state at the frame's start
    uint8_t index, pad;
    uint8_t data[VOICE_FRAME_BYTES];
} VoiceFrame;

// Starts the capture thread (idle until talking). 0 if the mic can't be used on this PSP.
int voice_init(void);
int voice_available(void);
void voice_shutdown(void);
// Push to talk: capture while on
void voice_set_talking(int on);
int voice_talking(void);
// The next captured frame, packed; returns 1 when one was ready
int voice_take_frame(VoiceFrame *out);
// A frame from someone else (stream 0..3, one per player): decoded and queued for the speaker
void voice_play(int stream, const VoiceFrame *f);
