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

#endif
