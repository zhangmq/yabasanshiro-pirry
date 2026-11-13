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

/* Pull side.  There is no consumer thread of our own: the two legal libretro
   contexts call SNDLibretroPump() instead.
     - RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK (env 22, asynchronous audio): the
       front end's audio thread notifies us and we hand the frames to
       audio_batch_cb from inside that callback.
     - Synchronous front ends: retro_run() calls SNDLibretroReadyPeriods() and
       pumps that many periods from inside retro_run.
   One call renders at most one period (512 frames, the standalone's device
   period) and returns the frames written. */
unsigned SNDLibretroPump(s16 *dst);

/* Whole 512-frame periods the ring can feed right now (0 while the startup
   cushion is still filling).  Synchronous delivery loop: pump this many. */
unsigned SNDLibretroReadyPeriods(void);

/* Drops everything queued: audio from a timeline that has already moved on
   (state load, or the start of a session).  A front end pause needs no such
   call: the SCSP runs on the machine's clock, so nothing is produced while the
   machine is parked and the ring simply keeps what it already holds. */
void SNDLibretroDropBacklog(void);

/* Current resampling ratio (output frames per input frame), for the log. */
double SNDLibretroRatio(void);

/* Cumulative counters: produced / consumed / dropped frames, ring fill, and how
   the underrun frames were concealed (repeated pitch period vs. faded to
   silence).  The caller takes differences, like it does for the others. */
void SNDLibretroSnapshot(u32 *produced, u32 *consumed, u32 *dropped,
                         u32 *underrun, unsigned *fill_frames,
                         u32 *plc_frames, u32 *fade_frames);

#endif
