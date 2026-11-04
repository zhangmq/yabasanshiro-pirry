/*  src/sndstretch.h: the decoupled audio path shared by the sound sinks.

    Yabause's standalone sink (sndsdl.c) and the libretro sink in
    libretro/libretro.c used to carry two copies of the same idea.  This module
    is the single implementation: a ring at the emulated sample rate, a slow
    resampling-ratio regulator that holds the ring at a fill target, and
    packet-loss concealment (repeat of the last pitch period, fading out) when
    the ring runs dry.

    The producer is always the SCSP thread (SndStretchWrite).  The consumer
    differs per sink: sndsdl drains from its SDL callback, libretro from
    retro_run() -- both call SndStretchRead().

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/
#ifndef SNDSTRETCH_H
#define SNDSTRETCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring size and fill target follow pirry's sndsdl.c: 8 blocks of 512 stereo
   frames, fill target at 4 fields of the current video format. */
#define SNDSTRETCH_BLOCKSAMPLES 512
#define SNDSTRETCH_NUMBLOCKS    8
#define SNDSTRETCH_RING_FRAMES  (SNDSTRETCH_BLOCKSAMPLES * SNDSTRETCH_NUMBLOCKS)

void     SndStretchInit(int samplerate, int vertfreq);
void     SndStretchDeInit(void);
/* Drop everything (pause/load/reset): the queued audio belongs to a timeline
   that is being replaced. */
void     SndStretchReset(void);
/* Producer (SCSP thread): resample one field into the ring and run the
   regulator.  Caller holds no lock. */
void     SndStretchWrite(const int32_t *left, const int32_t *right, unsigned num_samples);
/* Consumer: fill exactly `frames` stereo frames, concealing a gap with PLC.
   Always returns `frames`. */
unsigned SndStretchRead(int16_t *out, unsigned frames);
/* Free / used space in frames. */
unsigned SndStretchSpace(void);
unsigned SndStretchSpaceIn(void);   /* space in input frames, ratio-corrected */
unsigned SndStretchFillTarget(void);
double   SndStretchRatio(void);
unsigned SndStretchFill(void);
/* Fill target override (the load/pause handshake needs it). */
void     SndStretchSetFillTarget(unsigned frames);
/* Muting is applied by the consumer, like the old sndsdl path. */
void     SndStretchSetMuted(int muted);
/* Output volume in percent (100 = unchanged), applied by the writer. */
void     SndStretchSetVolume(int volume);
int      SndStretchGetMuted(void);
/* Append silence at the write end until the ring reaches its fill target (the
   load/pause handshake needs the ring topped up synchronously). */
void     SndStretchPrefill(void);
/* Append silence until `frames` are queued (the sink's cushion, sized on the spot). */
void     SndStretchPrefillTo(unsigned frames);
/* Force the emergency trim band (a gap was just concealed). */
void     SndStretchMarkRebuild(void);
/* Diagnostics: frames concealed / clamped since the last call. */
unsigned SndStretchTakeConcealed(void);
unsigned SndStretchTakeClipped(void);

#ifdef __cplusplus
}
#endif

#endif /* SNDSTRETCH_H */
