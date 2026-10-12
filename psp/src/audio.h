#pragma once

#include "vec.h"

// Software mixer on one stereo hardware channel. Sounds come from data/sounds.psnd (sounds.low.psnd on 32MB).

int audio_init(void);
void audio_shutdown(void);

// Sound id by name, or -1. Names with several variants pick one at random when played.
int snd_find(const char *name);
// Plays a sound; vol 0..1, pan -1 (left) .. 1 (right). Returns a voice handle (or -1).
int snd_play(int id, float vol, float pan);
// On a channel (> 0) one sound plays at a time: a new one fades out the last, like Source's CHAN_WEAPON
#define SND_CHAN_WEAPON 1
int snd_play_chan(int id, float vol, int chan);
// Plays at a world position, with distance falloff and panning from the listener.
int snd_play_at(int id, vec3 pos, float vol);
// Looping sound (e.g. the physics gun hum). Stop it with snd_stop.
int snd_loop(int id, float vol);
void snd_stop(int handle);
void snd_set_listener(vec3 pos, float yaw_deg);
