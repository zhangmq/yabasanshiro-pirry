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

    Deliberately not here (phase 2, AUDIO-LIBRETRO-20261109.md §3a): prefill,
    +-1 % resampler with rate control, PLC/fade on underrun, pause/refill for
    save states, and moving the period source to the front end's audio thread
    (SET_AUDIO_CALLBACK) once a front end offers it.
*/

#include <errno.h>
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

static s16 *ring;                          /* SND_RING_FRAMES stereo frames */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 read_pos;                       /* frames */
static u32 write_pos;                      /* frames */
static u32 fill;                           /* frames queued */
static u32 frames_per_field;               /* 44100 / vertfreq, for the log */
static u8 volume = SND_MAXVOLUME;
static int muted;

/* Consumer thread. */
static pthread_t consumer_thread;
static volatile int consumer_run;
static int consumer_started;
static s16 period_buf[SND_PERIOD_FRAMES * 2];
static void (*submit_pcm)(const s16 *pcm, unsigned frames);
static unsigned submit_frames;

/* Per-second statistics, guarded by ring_lock. */
static u64 stat_ticks;
static u32 stat_produced;
static u32 stat_consumed;
static u32 stat_underrun;
static u32 stat_dropped;

static u64 snd_ticks(void)
{
   return YabauseGetTicks();
}

static u64 snd_tickfreq(void)
{
   return yabsys.tickfreq ? yabsys.tickfreq : 1;
}

static void SNDLibretroLogStats(void)
{
   const u64 freq = snd_tickfreq();
   u64 now;
   int due;
   u32 produced, consumed, underrun, dropped, fill_now, per_field;

   pthread_mutex_lock(&ring_lock);
   now = snd_ticks();
   if (stat_ticks == 0)
   {
      stat_ticks = now;
      pthread_mutex_unlock(&ring_lock);
      return;
   }
   due = (now - stat_ticks >= freq);
   produced = stat_produced;
   consumed = stat_consumed;
   underrun = stat_underrun;
   dropped = stat_dropped;
   fill_now = fill;
   per_field = frames_per_field;
   if (due)
   {
      stat_ticks = now;
      stat_produced = 0;
      stat_consumed = 0;
      stat_underrun = 0;
      stat_dropped = 0;
   }
   pthread_mutex_unlock(&ring_lock);

   if (due)
      LOG_DEBUG("audio: field=%u produced=%u consumed=%u underrun=%u dropped=%u fill=%u/%u",
                per_field, produced, consumed, underrun, dropped, fill_now,
                (u32)SND_RING_FRAMES);
}

/* One consumer period: exactly SND_PERIOD_FRAMES frames, silence for whatever
   the ring does not hold (sndsdl.c covers this with PLC/fade).  The device
   clock is never stretched to wait for the emulation. */
static void SNDLibretroPeriod(void)
{
   u32 first;

   pthread_mutex_lock(&ring_lock);

   first = fill;
   if (first > SND_PERIOD_FRAMES)
      first = SND_PERIOD_FRAMES;

   if (first > 0)
   {
      u32 head = SND_RING_FRAMES - read_pos;

      if (head > first)
         head = first;
      memcpy(period_buf, ring + 2 * read_pos, sizeof(s16) * 2 * head);
      if (first > head)
         memcpy(period_buf + 2 * head, ring, sizeof(s16) * 2 * (first - head));

      read_pos += first;
      if (read_pos >= SND_RING_FRAMES)
         read_pos -= SND_RING_FRAMES;
      fill -= first;
      stat_consumed += first;
   }
   if (first < SND_PERIOD_FRAMES)
   {
      memset(period_buf + 2 * first, 0,
             sizeof(s16) * 2 * (SND_PERIOD_FRAMES - first));
      stat_underrun += SND_PERIOD_FRAMES - first;
   }
   submit_frames = SND_PERIOD_FRAMES;

   pthread_mutex_unlock(&ring_lock);

   SNDLibretroLogStats();
}

/* Consumer thread: absolute schedule of SND_PERIOD_FRAMES at 44100 Hz, phase
   locked to CLOCK_MONOTONIC.  A wake-up more than three periods late is
   resynchronised instead of catching up in a burst (same clamp spirit as
   scsp.c's phase lock). */
static void *SNDLibretroConsumer(void *arg)
{
   const s64 period_ns =
      (s64)SND_PERIOD_FRAMES * 1000000000LL / 44100LL;
   struct timespec next;

   (void)arg;
   clock_gettime(CLOCK_MONOTONIC, &next);

   while (consumer_run)
   {
      struct timespec now;
      s64 late;

      next.tv_nsec += (long)(period_ns % 1000000000LL);
      next.tv_sec += (time_t)(period_ns / 1000000000LL);
      if (next.tv_nsec >= 1000000000L)
      {
         next.tv_sec++;
         next.tv_nsec -= 1000000000L;
      }

      while (consumer_run &&
             clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
         ;

      clock_gettime(CLOCK_MONOTONIC, &now);
      late = (s64)(now.tv_sec - next.tv_sec) * 1000000000LL +
             (s64)(now.tv_nsec - next.tv_nsec);
      if (late > 3 * period_ns)
         next = now;

      if (!consumer_run)
         break;

      SNDLibretroPeriod();

      /* Outside the ring lock: the front end may block here until its device
         drains, and the SCSP thread must never wait for that. */
      if (submit_pcm != NULL)
         submit_pcm(period_buf, submit_frames);
   }

   return NULL;
}

int SNDLibretroStart(void (*fn)(const s16 *pcm, unsigned frames))
{
   if (fn == NULL || consumer_started)
      return 0;

   submit_pcm = fn;
   consumer_run = 1;
   if (pthread_create(&consumer_thread, NULL, SNDLibretroConsumer, NULL) != 0)
   {
      LOG_WARN("audio: consumer thread failed to start");
      consumer_run = 0;
      submit_pcm = NULL;
      return -1;
   }
   consumer_started = 1;
   LOG_INFO("audio: consumer thread up, %u-frame periods (%.2f ms)",
            (unsigned)SND_PERIOD_FRAMES,
            (double)SND_PERIOD_FRAMES * 1000.0 / 44100.0);
   return 0;
}

void SNDLibretroStop(void)
{
   if (!consumer_started)
      return;
   consumer_run = 0;
   pthread_join(consumer_thread, NULL);
   consumer_started = 0;
   submit_pcm = NULL;
   LOG_INFO("audio: consumer thread stopped");
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
   pthread_mutex_unlock(&ring_lock);

   LOG_INFO("audio: SCSP adapter, ring %u frames (%u ms), %u-frame periods",
            (u32)SND_RING_FRAMES, (u32)(SND_RING_FRAMES / 44),
            (unsigned)SND_PERIOD_FRAMES);
   return 0;
}

static void SNDLibretroDeInit(void)
{
   SNDLibretroStop();

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
