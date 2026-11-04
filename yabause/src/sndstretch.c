/*  src/sndstretch.c: decoupled audio path (ring + ratio regulator + PLC).

    Ported from pirry's standalone sndsdl.c "decoupled sound path"; that file now
    calls into this module instead of carrying its own copy, and the libretro
    sink does the same.  See sndstretch.h for the interface and the rationale.

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "sndstretch.h"

/* Ratio regulator: +-1% normally, +-1.5% while the ring is being rebuilt or is
   far from the target.  Same constants as sndsdl.c. */
#define SNDSTRETCH_RATIOMIN     0.99
#define SNDSTRETCH_RATIOMAX     1.01
#define SNDSTRETCH_RATIOGAIN    0.10
#define SNDSTRETCH_RATIOSLEW    0.10
#define SNDSTRETCH_RATIOMIN_EMG 0.985
#define SNDSTRETCH_RATIOMAX_EMG 1.015
#define SNDSTRETCH_LOWFILL      0.60
#define SNDSTRETCH_HIGHFILL     0.85

/* PLC: periods searched, and how many consecutive concealed fields still get
   the tonal repeat before the fade takes over. */
#define PLC_PMIN    32
#define PLC_PMAX    480
#define PLC_RUNMAX  4
#define PLC_SCOREWIN 240

static int16_t ring[SNDSTRETCH_RING_FRAMES * 2];
static unsigned rd, wr, count;
static double ratio = 1.0;      /* output frames per input frame */
static double frac;             /* fractional read position in the field */
static int32_t lastl, lastr;
static unsigned fill_target = (44100 / 60) * 4;
static int muted;
static int volume = 100;        /* percent, applied like sndsdl's PushFrame */
static int rebuild;
static int plc_run, plc_lastP;
static unsigned concealed;
static unsigned clipped;       /* frames clamped by the writer (diagnostics) */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

void SndStretchInit(int samplerate, int vertfreq)
{
   pthread_mutex_lock(&mtx);
   rd = wr = count = 0;
   ratio = 1.0;
   frac = 0.0;
   lastl = lastr = 0;
   muted = 0;
   rebuild = 0;
   plc_run = plc_lastP = 0;
   concealed = 0;
   fill_target = (unsigned)(samplerate / (vertfreq ? vertfreq : 60)) * 4;
   pthread_mutex_unlock(&mtx);
}

void SndStretchDeInit(void)
{
   SndStretchReset();
}

void SndStretchReset(void)
{
   pthread_mutex_lock(&mtx);
   rd = wr = count = 0;
   frac = 0.0;
   ratio = 1.0;
   lastl = lastr = 0;
   rebuild = 0;
   plc_run = plc_lastP = 0;
   pthread_mutex_unlock(&mtx);
}

unsigned SndStretchSpace(void)
{
   unsigned free_frames;
   pthread_mutex_lock(&mtx);
   free_frames = SNDSTRETCH_RING_FRAMES - count;
   pthread_mutex_unlock(&mtx);
   return free_frames;
}

/* Same figure sndsdl.c's GetAudioSpace_Stretch() reports: input frames that fit
   given the current resampling ratio, minus one for the fractional carry. */
double SndStretchRatio(void)
{
   double r;
   pthread_mutex_lock(&mtx);
   r = ratio;
   pthread_mutex_unlock(&mtx);
   return r;
}

unsigned SndStretchFillTarget(void)
{
   unsigned t;
   pthread_mutex_lock(&mtx);
   t = fill_target;
   pthread_mutex_unlock(&mtx);
   return t;
}

unsigned SndStretchSpaceIn(void)
{
   unsigned free_frames;
   double r;
   pthread_mutex_lock(&mtx);
   free_frames = SNDSTRETCH_RING_FRAMES - count;
   r = ratio;
   pthread_mutex_unlock(&mtx);
   {
      double in = (double)free_frames / (r > 0.0 ? r : 1.0);
      if (in < 2.0) return 0;
      return (unsigned)in - 1;
   }
}

unsigned SndStretchFill(void)
{
   unsigned frames;
   pthread_mutex_lock(&mtx);
   frames = count;
   pthread_mutex_unlock(&mtx);
   return frames;
}

void SndStretchSetFillTarget(unsigned frames)
{
   pthread_mutex_lock(&mtx);
   if (frames == 0) frames = 1;
   if (frames > SNDSTRETCH_RING_FRAMES) frames = SNDSTRETCH_RING_FRAMES;
   fill_target = frames;
   pthread_mutex_unlock(&mtx);
}

/* Append silence at the write end until `frames` are queued.  Added for the
   libretro sink: after the frontend has been away (its menu, a load) the frames
   it needs back are silence, and they have to be queued behind whatever the
   machine already produced rather than concealed note by note. */
void SndStretchPrefillTo(unsigned frames)
{
   pthread_mutex_lock(&mtx);
   if (frames > SNDSTRETCH_RING_FRAMES)
      frames = SNDSTRETCH_RING_FRAMES;
   while (count < frames)
   {
      ring[wr * 2 + 0] = 0;
      ring[wr * 2 + 1] = 0;
      wr = (wr + 1) % SNDSTRETCH_RING_FRAMES;
      count++;
   }
   pthread_mutex_unlock(&mtx);
}

void SndStretchPrefill(void)
{
   SndStretchPrefillTo(fill_target);
}

void SndStretchSetMuted(int m) { muted = m; }
void SndStretchSetVolume(int v)
{
   if (v < 0) v = 0;
   if (v > 200) v = 200;
   volume = v;
}
int  SndStretchGetMuted(void) { return muted; }
void SndStretchMarkRebuild(void) { rebuild = 1; }

unsigned SndStretchTakeClipped(void)
{
   unsigned n;
   pthread_mutex_lock(&mtx);
   n = clipped;
   clipped = 0;
   pthread_mutex_unlock(&mtx);
   return n;
}

unsigned SndStretchTakeConcealed(void)
{
   unsigned n;
   pthread_mutex_lock(&mtx);
   n = concealed;
   concealed = 0;
   pthread_mutex_unlock(&mtx);
   return n;
}

/* sndsdl.c PlcScore(): normalised cross-correlation of the last PLC_SCOREWIN
   frames against the same window one candidate period earlier. */
static double plc_score(unsigned pos_f, int P)
{
   double cross = 0.0, en = 1e-9;
   int k;
   for (k = 0; k < PLC_SCOREWIN; k += 2)
   {
      unsigned ai = (pos_f + SNDSTRETCH_RING_FRAMES - 1 - (unsigned)k) % SNDSTRETCH_RING_FRAMES;
      unsigned bi = (pos_f + SNDSTRETCH_RING_FRAMES - 1 - (unsigned)(k + P)) % SNDSTRETCH_RING_FRAMES;
      double b0 = ring[2 * bi], b1 = ring[2 * bi + 1];
      cross += (double)ring[2 * ai] * b0 + (double)ring[2 * ai + 1] * b1;
      en += b0 * b0 + b1 * b1;
   }
   return (cross > 0.0) ? (cross * cross) / en : 0.0;
}

/* Debug dump of what the SCSP hands the sink (env-gated, both sinks use it, so
   the standalone and the libretro core can be compared on the same content). */
static void sndstretch_dump_in(const int32_t *l, const int32_t *r, unsigned n)
{
   static FILE *f = NULL;
   static int init = 0;
   unsigned i;
   if (!init)
   {
      const char *p = getenv("YAB_AUDIO_DUMP_IN");
      init = 1;
      if (p && *p) f = fopen(p, "wb");
   }
   if (!f) return;
   for (i = 0; i < n; i++)
   {
      int16_t a = (int16_t)(l[i] > 0x7FFF ? 0x7FFF : (l[i] < -0x8000 ? -0x8000 : l[i]));
      int16_t b = (int16_t)(r[i] > 0x7FFF ? 0x7FFF : (r[i] < -0x8000 ? -0x8000 : r[i]));
      fwrite(&a, sizeof(a), 1, f);
      fwrite(&b, sizeof(b), 1, f);
   }
}

void SndStretchWrite(const int32_t *left, const int32_t *right, unsigned num_samples)
{
   const double step = 1.0 / ratio;
   int last_i = -1;

   if (!left || !right || !num_samples) return;

   sndstretch_dump_in(left, right, num_samples);

   pthread_mutex_lock(&mtx);

   /* Resample the field into the ring through the fractional accumulator, so a
      ratio slightly below/above 1 stretches or squeezes it by <=1%. */
   while (frac < (double)num_samples)
   {
      int si = (int)frac;                 /* >= 0: truncates like floor */
      double f = frac - (double)si;
      int32_t al = (si == 0) ? lastl : left[si - 1];
      int32_t ar = (si == 0) ? lastr : right[si - 1];
      int32_t l = (int32_t)(al + (left[si] - al) * f);
      int32_t r = (int32_t)(ar + (right[si] - ar) * f);

      if (volume != 100)
      {
         l = (int32_t)(((int64_t)l * volume) / 100);
         r = (int32_t)(((int64_t)r * volume) / 100);
      }

      /* The SCSP's output is 32-bit and can exceed the s16 range: sndsdl.c's
       * PushFrame() scales and clamps for exactly this reason.  Without the
       * clamp the cast wraps around and that *is* audible distortion, not a
       * level question. */
      if (l > 0x7FFF) { l = 0x7FFF; clipped++; }
      else if (l < -0x8000) { l = -0x8000; clipped++; }
      if (r > 0x7FFF) { r = 0x7FFF; clipped++; }
      else if (r < -0x8000) { r = -0x8000; clipped++; }

      if (count >= SNDSTRETCH_RING_FRAMES)
         break;                           /* full: drop, like sndsdl */

      ring[wr * 2 + 0] = (int16_t)l;
      ring[wr * 2 + 1] = (int16_t)r;
      wr = (wr + 1) % SNDSTRETCH_RING_FRAMES;
      count++;

      last_i = si;
      frac += step;
   }

   if (frac >= (double)num_samples)
   {
      frac -= (double)num_samples;
      lastl = left[num_samples - 1];
      lastr = right[num_samples - 1];
   }
   else
   {
      frac = 0.0;
      if (last_i >= 0)
      {
         lastl = left[last_i];
         lastr = right[last_i];
      }
   }

   {
      const double target = (double)fill_target / (double)SNDSTRETCH_RING_FRAMES;
      const double fill = (double)count / (double)SNDSTRETCH_RING_FRAMES;
      const double err = fill - target;
      double want = 1.0 - SNDSTRETCH_RATIOGAIN * err;
      double rmin = SNDSTRETCH_RATIOMIN, rmax = SNDSTRETCH_RATIOMAX;

      if (rebuild || fill < target * SNDSTRETCH_LOWFILL)
         rmax = SNDSTRETCH_RATIOMAX_EMG;
      if (fill > SNDSTRETCH_HIGHFILL)
         rmin = SNDSTRETCH_RATIOMIN_EMG;
      if (rebuild && fill >= target)
         rebuild = 0;

      if (want > rmax) want = rmax;
      else if (want < rmin) want = rmin;

      ratio += (want - ratio) * SNDSTRETCH_RATIOSLEW;
   }

   pthread_mutex_unlock(&mtx);
}

unsigned SndStretchRead(int16_t *out, unsigned frames)
{
   unsigned done = 0;

   if (!out || !frames) return 0;

   pthread_mutex_lock(&mtx);

   while (done < frames && count > 0)
   {
      out[done * 2 + 0] = ring[rd * 2 + 0];
      out[done * 2 + 1] = ring[rd * 2 + 1];
      rd = (rd + 1) % SNDSTRETCH_RING_FRAMES;
      count--;
      done++;
   }

   if (done >= frames)
      plc_run = 0;
   else
   {
      unsigned gap = frames - done, nframes = gap, fi;
      int16_t *o = out + done * 2;
      int plc_done = 0;

      plc_run++;
      concealed += gap;
      rebuild = 1;

      /* Short gaps: repeat the last pitch period (PLC).  The period is found
         with a coarse grid plus a fine search around the winner, starting from
         the last known period, exactly like sndsdl.c. */
      if (!muted && plc_run <= PLC_RUNMAX)
      {
         unsigned pos_f = rd;
         double best = 0.0;
         int bestP = 0, P;

         if (plc_lastP >= PLC_PMIN && plc_lastP <= PLC_PMAX)
         {
            best = plc_score(pos_f, plc_lastP);
            if (best > 0.0) bestP = plc_lastP;
         }
         for (P = PLC_PMIN; P <= PLC_PMAX; P += 4)
         {
            double sc = plc_score(pos_f, P);
            if (sc > best) { best = sc; bestP = P; }
         }
         if (bestP > 0)
         {
            int lo = bestP - 3, hi = bestP + 3, q;
            if (lo < PLC_PMIN) lo = PLC_PMIN;
            if (hi > PLC_PMAX) hi = PLC_PMAX;
            for (q = lo; q <= hi; q++)
            {
               double sc;
               if (q == bestP) continue;
               sc = plc_score(pos_f, q);
               if (sc > best) { best = sc; bestP = q; }
            }
         }
         plc_lastP = bestP;

         if (bestP > 0)
         {
            for (fi = 0; fi < nframes; fi++)
            {
               unsigned si = (pos_f + SNDSTRETCH_RING_FRAMES - (unsigned)bestP +
                              (fi % (unsigned)bestP)) % SNDSTRETCH_RING_FRAMES;
               o[2 * fi + 0] = ring[2 * si + 0];
               o[2 * fi + 1] = ring[2 * si + 1];
            }
            if (nframes > 1764)      /* longer than ~40 ms: fade to 0 */
            {
               unsigned t;
               int32_t den = (int32_t)(nframes - 1764);
               if (den < 1) den = 1;
               for (t = 1764; t < nframes; t++)
               {
                  int32_t num = (int32_t)(nframes - 1 - t);
                  o[2 * t + 0] = (int16_t)((int32_t)o[2 * t + 0] * num / den);
                  o[2 * t + 1] = (int16_t)((int32_t)o[2 * t + 1] * num / den);
               }
            }
            plc_done = 1;
         }
      }

      /* No usable period: fade the last sample out over 64 frames, silence
         after that (a hard cut is what we are trying to avoid). */
      if (!plc_done)
      {
         const unsigned FADE = 64;
         int16_t fl = 0, fr = 0;
         unsigned fade_n = (nframes < FADE) ? nframes : FADE;
         if (done >= 1) { fl = out[(done - 1) * 2 + 0]; fr = out[(done - 1) * 2 + 1]; }
         for (fi = 0; fi < fade_n; fi++)
         {
            int32_t num = (int32_t)(FADE - 1 - fi);
            o[2 * fi + 0] = (int16_t)((int32_t)fl * num / (int32_t)(FADE - 1));
            o[2 * fi + 1] = (int16_t)((int32_t)fr * num / (int32_t)(FADE - 1));
         }
         for (fi = fade_n; fi < nframes; fi++)
         {
            o[2 * fi + 0] = 0;
            o[2 * fi + 1] = 0;
         }
      }
   }

   if (muted)
      memset(out, 0, frames * 2 * sizeof(int16_t));

   pthread_mutex_unlock(&mtx);
   return frames;
}
