// hle_audio.c's output for other host files: an SDK 20+ audio output stream (hle_audio_stream.c) played on the same machinery as an AudioOut (real output / silent sink / background take-over)
#pragma once
#include <stddef.h>
#include <stdint.h>

// The stream's pull source, called on the consumer (the AudioUnit callback or the silent sink) while the output is started: fills frames × channels PCM16, no lock, no allocation, no log
typedef struct { uint32_t missing, filled; int signal; } dm_pcm_pull;   // frames it had nothing for (played as silence), frames left buffered after it, whether the output's event is due
typedef void (*dm_pcm_source)(void *ctx, int16_t *dst, uint32_t frames, dm_pcm_pull *r);
// key: the stream's host object; ev: its data event or NULL (bound auto-clear here, signaled when the source says so); lr: the game's call site for the log. Returns the output's number (#N)
int dm_pcmout_open_source(void *key, int ch, void *ev, const char *name, dm_pcm_source src, void *ctx, void *lr);
void dm_pcmout_start_at(void *k, void *lr);
void dm_pcmout_stop_at(void *k, void *lr);
void dm_pcmout_close_at(void *k, void *lr);   // stop, take it out of the registry, free the output; the key stays the caller's
void dm_audio_dump(const void *pcm, size_t bytes);   // DM_AUDIO_DUMP: PCM16 as the host plays it
