/*  YabaSanshiro — libretro port: SCSP output adapter (header).

    This file is part of YabaSanshiro.
*/

#ifndef LIBRETRO_SNDLIBRETRO_H
#define LIBRETRO_SNDLIBRETRO_H

#include "scsp.h"

/* Sound core id.  scsp.h has 0 = dummy, 10 = wav; this one is not used by any
   other port. */
#define SNDCORE_LIBRETRO 11

extern SoundInterface_struct SNDLibretro;

/* Starts the consumer thread: 512-frame periods at 44100 Hz, every period
   handed to fn(pcm, 512) as interleaved s16.  fn is called from that thread and
   may block on the front end; returns 0 on success, -1 when the thread cannot
   be started (fn == NULL starts nothing). */
int SNDLibretroStart(void (*fn)(const s16 *pcm, unsigned frames));

/* Stops the consumer thread (joins it). */
void SNDLibretroStop(void);

/* Async path (RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK): the front end's audio
   thread calls SNDLibretroPump() when it can take audio, and libretro.c hands
   the frames to audio_batch_cb from inside that same callback.  At most one
   period per call (512 frames, the standalone's device period); returns the
   frames taken (0 when the ring holds nothing). */
unsigned SNDLibretroPump(s16 *dst);

/* Drops everything queued: stale audio from a timeline that has already moved
   on (pause/resume, or the start of a session). */
void SNDLibretroDropBacklog(void);

/* retro_run() calls this once per frame.  The consumer thread and the pump use
   it to tell a real stall from a pause: the front end not calling retro_run is
   the only pause signal libretro offers (there is no pause environment call;
   env 22's set_state is about the audio driver, not the game). */
void SNDLibretroHeartbeat(void);
unsigned SNDLibretroSinceHeartbeatMs(void);

/* Current resampling ratio (output frames per input frame), for the log. */
double SNDLibretroRatio(void);

/* Pause state and transition counters (for the once-per-second log). */
int SNDLibretroPaused(void);
unsigned SNDLibretroPauseEvents(void);
unsigned SNDLibretroResumeEvents(void);

/* Per-second counters: produced / consumed / dropped frames, ring fill. */
void SNDLibretroSnapshot(u32 *produced, u32 *consumed, u32 *dropped,
                         u32 *underrun, unsigned *fill_frames);

#endif
