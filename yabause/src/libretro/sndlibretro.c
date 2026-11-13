/*  YabaSanshiro — libretro port: SCSP output adapter.

    This file is part of YabaSanshiro.

    Phase 1 audio, structured like the standalone's SNDCORE_SDL (sndsdl.c),
    because the problem is the same one: two clocks (the emulation's and the
    audio device's) that must not drag each other around.

      producer — SCSP thread (scsp.c:6306 ScspExecAsync, wall clock, 44100 frames/s)
        SNDCore->GetAudioSpace()   free frames in the ring; 0 when full
        SNDCore->UpdateAudio()     s32 planar -> s16 interleaved, appended
        Full ring: the samples stay in the SCSP's own ring and its overrun
        logic drops them (scsp.c:6338).  The producer never waits.

      consumer — one thread of our own, paced at 512 frames per period
        (sndsdl.c:371 "audiofmt.samples = 512", i.e. 11.61 ms at 44100 Hz).
        Every period it takes exactly 512 frames, zero-filling a shortfall
        instead of waiting, and hands them over with the submit callback
        installed by libretro.c (which calls audio_batch_cb).  The period does
        not depend on the render frame rate and not on how full the ring is,
        so a slow renderer cannot slow the audio down or make it crackle, and a
        full front-end audio ring blocks this thread only.

      retro_run — no audio work at all.

    Consequences worth knowing: at 60 fps the emulation produces 735 frames per
    field and the consumer takes 512 per period (86.13 periods/s = 44100/s);
    the ring absorbs the difference, exactly as sndsdl.c's ring does against the
    sound card.  If the producer falls behind (renderer slower than the audio
    device, machine stalled), the consumer keeps its clock and outputs silence
    for the missing frames; phase 2 replaces that silence with sndsdl.c's
    PLC/fade, adds the +-1 % resampler and rate control, and the prefill.

    The ring is as deep as the standalone's (sndsdl.c:368: (44100/50)*8 = 7056
    frames = 160 ms) but is not primed: nothing is fed ahead of time here, so
    the depth is jitter/overflow headroom.

    Deliberately not here (phase 2, AUDIO-LIBRETRO-20261109.md §3a): moving the
    period source to the front end's audio thread (SET_AUDIO_CALLBACK) once a
    front end offers it.
*/

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scsp.h"
#include "yabause.h"
#include "log.h"
#include "sndlibretro.h"

/* sndsdl.c:368: (44100/50) * 8 frames = 160 ms. */
#define SND_RING_FRAMES   ((44100 / 50) * 8)

/* sndsdl.c:371: the period sndsdl.c asks SDL for, i.e. 11.61 ms at 44100 Hz. */
#define SND_PERIOD_FRAMES 512

#define SND_MAXVOLUME     128

/* Adaptive resampling trim, ported from the standalone (sndsdl.c:600-740).
   The SCSP thread produces by the wall clock, the front end's device consumes
   by its own crystal, and the two never match exactly (measured ~0.1% here).
   A small ratio trim holds the ring at a fill target instead of letting it
   drift into a hard underrun or a dropped block.  The trim stays inside +-1%
   (inaudible as pitch), with a slightly wider emergency band when the level is
   far off; the ratio is slewed so it never steps. */
#define SND_STRETCH_RATIOMIN      0.99
#define SND_STRETCH_RATIOMAX      1.01
#define SND_STRETCH_RATIOGAIN     0.10
#define SND_STRETCH_RATIOSLEW     0.10
#define SND_STRETCH_RATIOMIN_EMG  0.985
#define SND_STRETCH_RATIOMAX_EMG  1.015
#define SND_STRETCH_LOWFILL       0.60
#define SND_STRETCH_HIGHFILL      0.85
#define SND_STRETCH_TARGET_FIELDS 4      /* YABA_STRETCH_PREFILL: 4 fields ~67 ms */
#define SND_STRETCH_WIN_FRAMES    1024   /* one period needs ~522 + slack */

static s16 *ring;                          /* SND_RING_FRAMES stereo frames */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 read_pos;                       /* frames */
static u32 write_pos;                      /* frames */
static u32 fill;                           /* frames queued */
static u32 frames_per_field;               /* 44100 / vertfreq, for the log */
static u8 volume = SND_MAXVOLUME;
static int muted;

/* Per-second statistics, guarded by ring_lock. */
static u64 stat_ticks;
static u32 stat_produced;
static u32 stat_consumed;
static u32 stat_underrun;
static u32 stat_plc;              /* underrun frames filled by repeating a pitch period */
static u32 stat_fade;             /* underrun frames faded to silence                        */
static u32 stat_dropped;
static u32 stat_stale;             /* trimmed by the prefill target */

/* Async (env 22) path: the front end's audio thread pulls, so there is no
   consumer thread and no wall-clock schedule of our own. */
static s64 hb_us;                  /* last retro_run, CLOCK_MONOTONIC us */

/* Resampler state (ring_lock). */
static u32 stretch_target;         /* fill target, frames (four fields) */
static double soundratio = 1.0;    /* output frames per input frame */
static double fill_avg = -1.0;     /* low-passed ring level for the regulator */
static int snd_prefill_gate(void);  /* startup cushion, defined below */
static double soundfrac;           /* fractional read position in the window */
static s16 stretch_last[2];        /* last input frame, for interpolation */
static s16 stretch_win[SND_STRETCH_WIN_FRAMES * 2];

/* No pause handling: the SCSP runs on the machine's emulated clock
   (scsp.c, machine-token mode), so a front end that stops calling retro_run
   simply stops the machine and nothing is produced while it is away.  What the
   ring already holds is valid audio for the moment it was produced in, and the
   consumer may play it out; the next machine frame starts from exactly the same
   emulated time.  Nothing to detect, nothing to drop, nothing to refill.
   (The wall-clock SCSP thread needed a heartbeat to tell it that the front end
   had stopped, because it kept producing into a timeline nobody was watching.) */


/* Steer the ring towards the fill target (constants above).  Caller holds
   ring_lock.  Proportional only, like the standalone: a small standing error
   just settles the ring a few ms off target, whereas an integral term would
   wind the ratio up against a rate difference that never goes away. */
static void snd_regulate(void)
{
   /* The producer publishes a whole field at a time (735/882 frames every 16.7 or
      20 ms) while this runs once per 512-frame device period, so the raw level
      carries a strong ripple at the field rate - and sampling it at 86 Hz aliases
      that ripple into a slow beat.  Regulating the raw value made the ratio swing
      by ~0.5% at a few Hz, audible as a warble/oscillation (reported on PAL,
      where the field rate is 50 Hz).  Regulate the low-passed level instead:
      tau ~ 0.6 s kills the field ripple and leaves real drift. */
   if (fill_avg < 0.0)
      fill_avg = (double)fill;
   else
      fill_avg += ((double)fill - fill_avg) * 0.02;

   double fillf = fill_avg / (double)SND_RING_FRAMES;
   double targetf = (double)stretch_target / (double)SND_RING_FRAMES;
   double err = fillf - targetf;
   double want = 1.0 - SND_STRETCH_RATIOGAIN * err;
   double rmin = SND_STRETCH_RATIOMIN;
   double rmax = SND_STRETCH_RATIOMAX;

   if (fillf < targetf * SND_STRETCH_LOWFILL)
      rmax = SND_STRETCH_RATIOMAX_EMG;
   if (fillf > SND_STRETCH_HIGHFILL)
      rmin = SND_STRETCH_RATIOMIN_EMG;
   if (want > rmax)
      want = rmax;
   else if (want < rmin)
      want = rmin;

   /* Deadband: the SCSP thread and the audio device run off the same system
      clock here, so a level inside this band is jitter, not drift.  Regulating
      it would keep the resampler permanently off 1.0, which is audible as a
      faint hiss over music (linear interpolation, measured: ratio never
      settled, 45% of the delivered samples were interpolated).  Inside the band
      the ratio goes back to exactly 1.0 and snd_render_period() copies. */
   if (fabs(err) < 0.10) {
      want = 1.0;
      if (soundratio > 1.0 ? (soundratio - 1.0 < 2e-4) : (1.0 - soundratio < 2e-4))
         soundratio = 1.0;
   }
   if (want != 1.0 || soundratio != 1.0)
      soundratio += (want - soundratio) * SND_STRETCH_RATIOSLEW;
}

/* Packet-loss concealment for a short ring, ported from the standalone
   (sndsdl.c MixAudio_Stretch + PlcScore): repeat the last pitch period found in
   the ring history so a tonal signal continues instead of clicking.  After four
   consecutive gaps, or when no period is found, fade 64 frames from the last
   sample and leave the rest silent.  Called with ring_lock held. */
static double snd_plc_score(u32 pos_f, int P)
{
   const int W = 240;
   double cross = 0.0, en = 1e-9;
   int k;

   for (k = 0; k < W; k += 2)
   {
      u32 ai = (pos_f + SND_RING_FRAMES - 1 - (u32)k) % SND_RING_FRAMES;
      u32 bi = (pos_f + SND_RING_FRAMES - 1 - (u32)(k + P)) % SND_RING_FRAMES;
      double b0 = ring[2 * bi], b1 = ring[2 * bi + 1];
      cross += (double)ring[2 * ai] * b0 + (double)ring[2 * ai + 1] * b1;
      en    += b0 * b0 + b1 * b1;
   }
   return (cross > 0.0) ? (cross * cross) / en : 0.0;
}

static int snd_plc_run;           /* consecutive periods with an underrun (statistics only) */
static int snd_plc_lastP;

/* Upstream caps the concealment at four consecutive periods (sndsdl.c:238
   `plc_run <= 4`) because it was written for an isolated dropout: repeating a
   pitch period forever after one lost packet would turn a stall into a buzzing
   loop.

   The case this port runs into is the opposite one.  When the machine cannot
   emulate a field in 1/60 s the SCSP produces less than 44.1 kHz and *every*
   period is short - by 15-25 frames at astrass' title screen.  With the
   counter cap the first four periods get a pitch-period repetition and all the
   rest take the fade path: a 0.3-0.5 ms fade to silence every 11.6 ms, audible
   as a periodic rough edge.

   So the criterion is the gap length, not the number of consecutive events: a
   short gap is bridged by repeating the last pitch period (cheap, keeps the
   waveform going, no pitch change), a long one still fades so a real stall
   cannot become a loop.  220 frames = 5 ms covers the gaps the SCSP leaves
   when it delivers a quarter of a field at a time; a whole empty period (512)
   still fades. */
#define SND_PLC_MAX_GAP_FRAMES 220

static void snd_conceal(s16 *dst, u32 from)
{
   u32 nframes;
   int plc_done = 0;

   if (from >= SND_PERIOD_FRAMES)
      return;
   nframes = SND_PERIOD_FRAMES - from;
   snd_plc_run++;

   if (from > 0 && nframes <= SND_PLC_MAX_GAP_FRAMES)
   {
      const int P_MIN = 32, P_MAX = 480;
      double best = 0.0;
      int bestP = 0, P;

      if (snd_plc_lastP >= P_MIN && snd_plc_lastP <= P_MAX)
      {
         best = snd_plc_score(read_pos, snd_plc_lastP);
         if (best > 0.0)
            bestP = snd_plc_lastP;
      }
      for (P = P_MIN; P <= P_MAX; P += 4)
      {
         double sc = snd_plc_score(read_pos, P);
         if (sc > best) { best = sc; bestP = P; }
      }
      if (bestP > 0)
      {
         int lo = bestP - 3, hi = bestP + 3, q;
         if (lo < P_MIN) lo = P_MIN;
         if (hi > P_MAX) hi = P_MAX;
         for (q = lo; q <= hi; q++)
         {
            double sc;
            if (q == bestP) continue;
            sc = snd_plc_score(read_pos, q);
            if (sc > best) { best = sc; bestP = q; }
         }
      }
      snd_plc_lastP = bestP;
      if (bestP > 0)
      {
         u32 fi;
         for (fi = 0; fi < nframes; fi++)
         {
            u32 si = (read_pos + SND_RING_FRAMES - (u32)bestP +
                      (fi % (u32)bestP)) % SND_RING_FRAMES;
            dst[2 * (from + fi)]     = ring[2 * si];
            dst[2 * (from + fi) + 1] = ring[2 * si + 1];
         }
         if (nframes > 1764)
         {
            u32 t;
            s32 den = (s32)(nframes - 1764);
            if (den < 1) den = 1;
            for (t = 1764; t < nframes; t++)
            {
               s32 num = (s32)(nframes - 1 - t);
               dst[2 * (from + t)]     = (s16)((s32)dst[2 * (from + t)] * num / den);
               dst[2 * (from + t) + 1] = (s16)((s32)dst[2 * (from + t) + 1] * num / den);
            }
         }
         plc_done = 1;
         stat_plc += nframes;
      }
   }

   if (!plc_done)
   {
      stat_fade += nframes;
      const u32 FADE = 64;
      s16 fl = 0, fr = 0;
      u32 fade_n = (nframes < FADE) ? nframes : FADE;
      u32 fi;

      if (from > 0)
      {
         fl = dst[2 * (from - 1)];
         fr = dst[2 * (from - 1) + 1];
      }
      for (fi = 0; fi < fade_n; fi++)
      {
         s32 num = (s32)(FADE - 1 - fi);
         dst[2 * (from + fi)]     = (s16)((s32)fl * num / (s32)(FADE - 1));
         dst[2 * (from + fi) + 1] = (s16)((s32)fr * num / (s32)(FADE - 1));
      }
      for (fi = fade_n; fi < nframes; fi++)
      {
         dst[2 * (from + fi)]     = 0;
         dst[2 * (from + fi) + 1] = 0;
      }
   }
}

/* One output period, resampled by soundratio from the ring.  Caller holds
   ring_lock.  Always writes SND_PERIOD_FRAMES frames; a ring that cannot feed
   them is zero-filled (the standalone conceals that with PLC/fade, phase 2).
   The phase (soundfrac) and the last input frame carry across calls, so the
   interpolation is continuous instead of restarting at every period. */
static void snd_render_period(s16 *dst)
{
   const double step = 1.0 / soundratio;
   u32 need, avail, i, k, used;
   int clamped = 0;
   double pos = soundfrac;

   /* Ratio 1.0 and an aligned phase: hand the ring out untouched.  No
      interpolation, no rounding (16-bit samples stay bit-exact), which is what
      the standalone does when its device rate equals the SCSP rate. */
   if (soundratio == 1.0 && soundfrac == 0.0 && fill >= SND_PERIOD_FRAMES)
   {
      u32 head = SND_RING_FRAMES - read_pos;
      if (head > SND_PERIOD_FRAMES)
         head = SND_PERIOD_FRAMES;
      memcpy(dst, ring + 2 * read_pos, sizeof(s16) * 2 * head);
      if (head < SND_PERIOD_FRAMES)
         memcpy(dst + 2 * head, ring, sizeof(s16) * 2 * (SND_PERIOD_FRAMES - head));
      stretch_last[0] = dst[2 * (SND_PERIOD_FRAMES - 1)];
      stretch_last[1] = dst[2 * (SND_PERIOD_FRAMES - 1) + 1];
      read_pos += SND_PERIOD_FRAMES;
      if (read_pos >= SND_RING_FRAMES)
         read_pos -= SND_RING_FRAMES;
      fill -= SND_PERIOD_FRAMES;
      stat_consumed += SND_PERIOD_FRAMES;
      soundfrac = 0.0;
      snd_regulate();
      return;
   }

   need = (u32)((double)SND_PERIOD_FRAMES * step) + 3;
   if (need > SND_STRETCH_WIN_FRAMES)
      need = SND_STRETCH_WIN_FRAMES;

   avail = fill;
   if (avail > need)
      avail = need;

   if (avail > 0)
   {
      u32 head = SND_RING_FRAMES - read_pos;
      if (head > avail)
         head = avail;
      memcpy(stretch_win, ring + 2 * read_pos, sizeof(s16) * 2 * head);
      if (avail > head)
         memcpy(stretch_win + 2 * head, ring, sizeof(s16) * 2 * (avail - head));
   }
   if (avail < need)
      memset(stretch_win + 2 * avail, 0, sizeof(s16) * 2 * (need - avail));
   if (avail < SND_PERIOD_FRAMES)
      stat_underrun += SND_PERIOD_FRAMES - avail;

   for (k = 0; k < SND_PERIOD_FRAMES; k++)
   {
      u32 idx = (u32)pos;
      double f = pos - (double)idx;
      double al, ar;

      if (idx >= need)
         idx = need - 1;
      al = (idx == 0) ? (double)stretch_last[0] : (double)stretch_win[2 * (idx - 1)];
      ar = (idx == 0) ? (double)stretch_last[1] : (double)stretch_win[2 * (idx - 1) + 1];
      dst[2 * k]     = (s16)(al + ((double)stretch_win[2 * idx] - al) * f);
      dst[2 * k + 1] = (s16)(ar + ((double)stretch_win[2 * idx + 1] - ar) * f);
      pos += step;
   }

   if (avail < need)
   {
      u32 from = (u32)((double)avail * soundratio);
      if (from > SND_PERIOD_FRAMES)
         from = SND_PERIOD_FRAMES;
      snd_conceal(dst, from);
   }
   else
      snd_plc_run = 0;

   used = (u32)pos;
   if (used > avail)
   {
      used = avail;
      clamped = 1;
   }
   if (used > 0)
   {
      stretch_last[0] = stretch_win[2 * (used - 1)];
      stretch_last[1] = stretch_win[2 * (used - 1) + 1];
      read_pos += used;
      if (read_pos >= SND_RING_FRAMES)
         read_pos -= SND_RING_FRAMES;
      fill -= used;
      stat_consumed += used;
   }
   if (clamped)
      pos = 0.0;                    /* short ring: the phase is meaningless */
   else
      pos -= (double)used;
   if (pos < 0.0)
      pos = 0.0;
   soundfrac = pos;

   snd_regulate();
}

/* Startup prefill: the ring is regulated towards stretch_target (four fields),
   but the regulator can only stretch audio that is there - starting from an
   empty ring, the consumer takes every frame the producer writes and the ring
   never fills (measured with a PAL game: fill 0-378 of 3528, soundratio pinned
   at its emergency clamp, PLC filling the gaps).  The standalone does the same
   thing with YABA_STRETCH_PREFILL (sndsdl.c): stay silent until the cushion
   exists.  A two-second safety releases the gate so a silent producer can never
   mute the core for good. */
static int prefill_done;
static u64 prefill_deadline_us;
static u64 snd_now_us(void);

/* Caller holds ring_lock.  1 = still filling, output silence. */
static int snd_prefill_gate(void)
{
   if (prefill_done)
      return 0;
   if (fill >= stretch_target)
   {
      /* Entering playback with more than the cushion is latency we would then
         have to regulate away (and while regulating the resampler runs off 1.0,
         which is audible as a faint hiss).  Trim to the target instead: the
         ring starts at the level the regulator wants. */
      u32 trim = fill - stretch_target;
      prefill_done = 1;
      read_pos += trim;
      if (read_pos >= SND_RING_FRAMES)
         read_pos -= SND_RING_FRAMES;
      fill = stretch_target;
      stat_stale += trim;
      LOG_INFO("audio: prefill done, %u frames (trimmed %u)", (u32)fill, (u32)trim);
      return 0;
   }
   if (prefill_deadline_us != 0 && snd_now_us() > prefill_deadline_us)
   {
      prefill_done = 1;
      LOG_WARN("audio: prefill timeout, starting with %u of %u frames",
               (u32)fill, (u32)stretch_target);
      return 0;
   }
   return 1;
}

/* Async path: one device period per call, the same size the standalone asks
   SDL for (sndsdl.c:371).  The caller submits it from inside the front end's
   audio callback, outside ring_lock (that call may block). */
unsigned SNDLibretroPump(s16 *dst)
{
   pthread_mutex_lock(&ring_lock);
   if (snd_prefill_gate())
   {
      memset(dst, 0, sizeof(s16) * 2 * SND_PERIOD_FRAMES);
      pthread_mutex_unlock(&ring_lock);
      return SND_PERIOD_FRAMES;
   }
   snd_render_period(dst);
   pthread_mutex_unlock(&ring_lock);
   return SND_PERIOD_FRAMES;
}

/* How many whole periods the ring can feed right now; zero while the startup
   cushion is still filling (the pump would only hand out silence).  The
   synchronous path in retro_run uses it: when the front end refuses env 22 it
   renders audio by calling retro_run, and whatever the SCSP produced since the
   last call is handed over there. */
unsigned SNDLibretroReadyPeriods(void)
{
   unsigned n;

   pthread_mutex_lock(&ring_lock);
   n = snd_prefill_gate() ? 0 : (fill / SND_PERIOD_FRAMES);
   pthread_mutex_unlock(&ring_lock);
   return n;
}

void SNDLibretroDropBacklog(void)
{
   pthread_mutex_lock(&ring_lock);
   read_pos = 0;
   write_pos = 0;
   fill = 0;
   prefill_done = 0;
   prefill_deadline_us = snd_now_us() + 2000000ULL;
   pthread_mutex_unlock(&ring_lock);
}

static u64 snd_now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (u64)t.tv_sec * 1000000u + (u64)t.tv_nsec / 1000u;
}

double SNDLibretroRatio(void)
{
   double r;

   pthread_mutex_lock(&ring_lock);
   r = soundratio;
   pthread_mutex_unlock(&ring_lock);
   return r;
}

void SNDLibretroSnapshot(u32 *produced, u32 *consumed, u32 *dropped,
                         u32 *underrun, unsigned *fill_frames,
                         u32 *plc_frames, u32 *fade_frames)
{
   pthread_mutex_lock(&ring_lock);
   if (produced)
      *produced = stat_produced;
   if (consumed)
      *consumed = stat_consumed;
   if (dropped)
      *dropped = stat_dropped;
   if (underrun)
      *underrun = stat_underrun;
   if (fill_frames)
      *fill_frames = fill;
   if (plc_frames)
      *plc_frames = stat_plc;
   if (fade_frames)
      *fade_frames = stat_fade;
   pthread_mutex_unlock(&ring_lock);
}

//////////////////////////////////////////////////////////////////////////////

static int SNDLibretroInit(void)
{
   pthread_mutex_lock(&ring_lock);
   if (ring == NULL)
      ring = (s16 *)malloc(sizeof(s16) * 2 * SND_RING_FRAMES);
   if (ring == NULL)
   {
      pthread_mutex_unlock(&ring_lock);
      return -1;
   }
   memset(ring, 0, sizeof(s16) * 2 * SND_RING_FRAMES);
   read_pos = 0;
   write_pos = 0;
   fill = 0;
   stat_ticks = 0;
   stat_produced = stat_consumed = stat_underrun = stat_dropped = 0;
   stat_stale = 0;
   soundratio = 1.0;
   soundfrac = 0.0;
   stretch_last[0] = stretch_last[1] = 0;
   stretch_target = 735 * SND_STRETCH_TARGET_FIELDS;
   prefill_done = 0;
   prefill_deadline_us = snd_now_us() + 2000000ULL;   /* safety: 2 s */
   pthread_mutex_unlock(&ring_lock);

   LOG_INFO("audio: SCSP adapter, ring %u frames (%u ms), %u-frame periods",
            (u32)SND_RING_FRAMES, (u32)(SND_RING_FRAMES / 44),
            (unsigned)SND_PERIOD_FRAMES);
   return 0;
}

static void SNDLibretroDeInit(void)
{
   pthread_mutex_lock(&ring_lock);
   free(ring);
   ring = NULL;
   fill = 0;
   pthread_mutex_unlock(&ring_lock);
}

static int SNDLibretroReset(void)
{
   pthread_mutex_lock(&ring_lock);
   read_pos = 0;
   write_pos = 0;
   fill = 0;
   pthread_mutex_unlock(&ring_lock);
   return 0;
}

static int SNDLibretroChangeVideoFormat(int vertfreq)
{
   /* The engine reports 50 or 60 (scsp.c:5656, from
      scspsoundlen = 44100/(type ? 50 : 60)).  It only feeds the debug line:
      the consumer period is the device's, not the video format's. */
   pthread_mutex_lock(&ring_lock);
   frames_per_field = (vertfreq > 0) ? (u32)(44100 / vertfreq) : 735;
   stretch_target = frames_per_field * SND_STRETCH_TARGET_FIELDS;
   read_pos = 0;
   write_pos = 0;
   fill = 0;
   pthread_mutex_unlock(&ring_lock);
   LOG_INFO("audio: %d Hz, %u frames per field", vertfreq,
            (unsigned)frames_per_field);
   return 0;
}

/* Called from the SCSP thread only (scsp.c:6364).  Never calls into the front
   end and never waits for it. */
static void SNDLibretroUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer,
                                   u32 num_samples)
{
   const s32 *srcl = (const s32 *)leftchanbuffer;
   const s32 *srcr = (const s32 *)rightchanbuffer;
   u32 i, space;

   pthread_mutex_lock(&ring_lock);

   if (ring == NULL || num_samples == 0)
   {
      pthread_mutex_unlock(&ring_lock);
      return;
   }

   space = SND_RING_FRAMES - fill;
   if (num_samples > space)
   {
      stat_dropped += num_samples - space;
      num_samples = space;
   }

   for (i = 0; i < num_samples; i++)
   {
      s32 l, r;

      if (muted)
      {
         l = 0;
         r = 0;
      }
      else
      {
         /* sndsdl.c:591-596 scales through a wider type and leaves the source
            alone: it is the SCSP's own output ring, which other readers use. */
         l = (s32)(((s64)srcl[i] * volume) / SND_MAXVOLUME);
         r = (s32)(((s64)srcr[i] * volume) / SND_MAXVOLUME);
         if (l > 0x7FFF) l = 0x7FFF;
         else if (l < -0x8000) l = -0x8000;
         if (r > 0x7FFF) r = 0x7FFF;
         else if (r < -0x8000) r = -0x8000;
      }

      ring[2 * write_pos] = (s16)l;
      ring[2 * write_pos + 1] = (s16)r;
      write_pos++;
      if (write_pos >= SND_RING_FRAMES)
         write_pos = 0;
      fill++;
   }

   stat_produced += num_samples;

   pthread_mutex_unlock(&ring_lock);
}

static u32 SNDLibretroGetAudioSpace(void)
{
   u32 space;

   pthread_mutex_lock(&ring_lock);
   space = (ring == NULL) ? 0 : (SND_RING_FRAMES - fill);
   pthread_mutex_unlock(&ring_lock);

   return space;
}

static void SNDLibretroMuteAudio(void)
{
   muted = 1;
}

static void SNDLibretroUnMuteAudio(void)
{
   muted = 0;
}

static void SNDLibretroSetVolume(int volume_in)
{
   volume = (u8)(((double)SND_MAXVOLUME / 100.0) * volume_in);
}

//////////////////////////////////////////////////////////////////////////////

SoundInterface_struct SNDLibretro = {
   SNDCORE_LIBRETRO,
   "Libretro SCSP adapter",
   SNDLibretroInit,
   SNDLibretroDeInit,
   SNDLibretroReset,
   SNDLibretroChangeVideoFormat,
   SNDLibretroUpdateAudio,
   SNDLibretroGetAudioSpace,
   SNDLibretroMuteAudio,
   SNDLibretroUnMuteAudio,
   SNDLibretroSetVolume
};
