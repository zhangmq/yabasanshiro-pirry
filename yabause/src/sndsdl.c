/*  Copyright 2005-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sndsdl.c
    \brief SDL sound interface.
*/

#ifdef HAVE_LIBSDL

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(__APPLE__) || defined(GEKKO)
 #ifdef HAVE_LIBSDL2
  #include <SDL2/SDL.h>
 #else
  #include <SDL/SDL.h>
 #endif
#else
 #include "SDL.h"
#endif
#include "error.h"
#include "scsp.h"
#include "sndsdl.h"
#include "sndstretch.h"
#include "debug.h"

static int SNDSDLInit(void);
static void SNDSDLDeInit(void);
static int SNDSDLReset(void);
static int SNDSDLChangeVideoFormat(int vertfreq);
static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace(void);
static void SNDSDLMuteAudio(void);
static void SNDSDLUnMuteAudio(void);
static void SNDSDLSetVolume(int volume);
#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport);
u8 SNDSDLMidiIn(int *isdata);
int SNDSDLMidiOut(u8 data);
#endif

// ---------------------------------------------------------------------------
// Decoupled sound path: a ring buffer with an explicit fill count, a small
// adaptive resampling trim (+-1%) against crystal and frame jitter, and
// packet-loss concealment (the last pitch period is repeated) instead of a
// hard cut when the ring runs dry. There is no time stretching, so tempo and
// pitch do not follow the emulation speed.

static int SNDSDLInit_Stretch(void);
static void SNDSDLDeInit_Stretch(void);
static int SNDSDLChangeVideoFormat_Stretch(int vertfreq);
static void SNDSDLUpdateAudio_Stretch(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace_Stretch(void);
static void MixAudio_Stretch(UNUSED void *userdata, Uint8 *stream, int len);
static void PushFrame(s32 l, s32 r);
static void ResetStream(void);
static int SNDSDLLoadSndPause(int action, unsigned int *fill_amount, unsigned int *filled_up, unsigned int *goal);
// ---------------------------------------------------------------------------

SoundInterface_struct SNDSDL = {
SNDCORE_SDL,
"SDL Sound Interface",
SNDSDLInit,
SNDSDLDeInit,
SNDSDLReset,
SNDSDLChangeVideoFormat,
SNDSDLUpdateAudio,
SNDSDLGetAudioSpace,
SNDSDLMuteAudio,
SNDSDLUnMuteAudio,
SNDSDLSetVolume,
#ifdef USE_SCSPMIDI
SNDSDLMidiChangePorts,
SNDSDLMidiIn,
SNDSDLMidiOut
#endif
};


// Ring parameters (used only in the decoupled path): ring about 160 ms
// deep (safety margin), operating point / fill target at soundprefill
// (~67 ms), fine trim clamped to +-1% (crystal/frame jitter, never audible as
// a pitch change).
#define YABA_STRETCH_NUMSOUNDBLOCKS 8
#define YABA_STRETCH_BLOCKSAMPLES   512
#define YABA_STRETCH_PREFILL        ((44100 / 60) * 4)
#define YABA_STRETCH_RATIOMIN       0.99
#define YABA_STRETCH_RATIOMAX       1.01
#define YABA_STRETCH_RATIOGAIN      0.10
#define YABA_STRETCH_RATIOSLEW      0.10

// Emergency band of the trim. At the +-1% clamp an empty ring needs about
// 8.6 s to refill (deficit = fill target, 3807 frames), and a full ring has
// to drop whole blocks in SNDSDLUpdateAudio_Stretch() (a jump of up to 16 ms).
// Outside the band below, the trim may go to +-1.5% (~26 cents) until the
// fill level is back at the target.
#define YABA_STRETCH_RATIOMIN_EMG   0.985
#define YABA_STRETCH_RATIOMAX_EMG   1.015
// Lower bound relative to the fill target, below which refilling is accelerated.
#define YABA_STRETCH_LOWFILL        0.60
// Upper bound relative to the ring size, above which draining is accelerated.
#define YABA_STRETCH_HIGHFILL       0.85

// Fill target in frames; SNDSDLInit_Stretch() raises it if SDL/ALSA grants a
// larger callback period than requested (otherwise the ring periodically
// drops to 0, causing crackling at a fixed cadence).
static u32 soundprefill = YABA_STRETCH_PREFILL;

static u16 *stereodata16;
static u32 soundoffset;
static volatile u32 soundpos;
static u32 soundlen;
static u32 soundbufsize;
static SDL_AudioSpec audiofmt;
static u8 soundvolume;
static int muted = 0;

// Used only in the decoupled path:
static u32 soundbuffered;   // bytes written but not yet played
static double soundratio;   // output frames per input frame
static double soundfrac;    // fractional read position in the input block
static s32 soundlastl;      // last input frame, for interpolation
static s32 soundlastr;

// Set by the audio callback on repeated underrun, read/cleared by the regulator
// in SNDSDLUpdateAudio_Stretch(). Both sides run under the SDL audio lock, so
// a plain int is sufficient.
static int soundrebuild;
// Touched only by the SDL audio callback thread.
// plc_run: consecutive underrun callbacks. Repeated PLC on the same material
//   turns into a static hum (same period every time), so from the 5th in a
//   row it fades out cleanly instead.
// plc_lastP: last detected pitch period, start candidate of the search.
static int plc_run = 0;
static int plc_lastP = 0;

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

// Scores one candidate period for the PLC, so the search below can run
// coarse/fine instead of exhaustively. Normalized cross-correlation over a
// window of W frames immediately before the read pointer, backwards with ring wrap.
/* PlcScore() lives in sndstretch.c now (single implementation). */


//////////////////////////////////////////////////////////////////////////////

// Callback of the decoupled path. Runs on the SDL audio thread, which holds the
// audio lock for its duration, so soundoffset/soundbuffered are stable here.
static void MixAudio_Stretch(UNUSED void *userdata, Uint8 *stream, int len) {
   /* Ring, PLC and the mute state live in sndstretch.c. */
   SndStretchRead((int16_t *)stream, (unsigned)(len / (int)(sizeof(int16_t) * 2)));
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLInit(void)
{
   return SNDSDLInit_Stretch();
}

// Pre-size the ring for the slowest video format (50 Hz) and never resize it,
// so ChangeVideoFormat_Stretch() does not have to free a buffer the audio
// thread is reading from.
static int SNDSDLInit_Stretch(void)
{
#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif
   SDL_InitSubSystem(SDL_INIT_AUDIO);

   audiofmt.freq = 44100;
   audiofmt.format = AUDIO_S16SYS;
   audiofmt.channels = 2;
   audiofmt.samples = YABA_STRETCH_BLOCKSAMPLES; // already a power of two
   audiofmt.callback = MixAudio_Stretch;
   audiofmt.userdata = NULL;

   {
      unsigned blocks = YABA_STRETCH_NUMSOUNDBLOCKS;
      soundbufsize = (audiofmt.freq / 50) * blocks * 2 * 2;
   }

   soundvolume = SDL_MIX_MAXVOLUME;

   {
      SDL_AudioSpec obtained;
      memset(&obtained, 0, sizeof(obtained));
      if (SDL_OpenAudio(&audiofmt, &obtained) != 0)
      {
         YabSetError(YAB_ERR_SDL, (void *)SDL_GetError());
         return -1;
      }
      // Adapt the fill target to the callback period actually granted: some
      // SDL/ALSA combinations grant larger periods than requested, and a too
      // small target then drops to 0 periodically (crackling at a fixed
      // cadence). 3 granted periods + 1 production frame of margin.
      if (obtained.samples > 0) {
         u32 want_prefill = (u32)obtained.samples * 3 + (44100 / 60);
         if (want_prefill > soundprefill) soundprefill = want_prefill;
      }

   }


   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   /* The decoupled path (ring + regulator + PLC) is shared with the libretro
    * sink; this sink owns only the SDL device. */
   SndStretchInit(44100, 60);
   SndStretchSetFillTarget(soundprefill);
   ResetStream();

   SDL_PauseAudio(0);

   g_loadsnd_pause = SNDSDLLoadSndPause;

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLDeInit(void)
{
   SNDSDLDeInit_Stretch();
}

static void SNDSDLDeInit_Stretch(void)
{
   g_loadsnd_pause = NULL;
   // Closes the device first, so the callback is no longer running by the
   // time the buffer it reads from goes away.
   SDL_CloseAudio();

   if (stereodata16)
   {
      free(stereodata16);
      stereodata16 = NULL;
   }
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLReset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLChangeVideoFormat(int vertfreq)
{
   return SNDSDLChangeVideoFormat_Stretch(vertfreq);
}

static int SNDSDLChangeVideoFormat_Stretch(UNUSED int vertfreq)
{
   // The ring is already sized for the slowest format, so there is nothing to
   // reallocate. Just re-silence it and reset the bookkeeping.
   SDL_LockAudio();

   if (stereodata16)
      memset(stereodata16, 0, soundbufsize);

   /* The decoupled path (ring + regulator + PLC) is shared with the libretro
    * sink; this sink owns only the SDL device. */
   SndStretchInit(44100, 60);
   SndStretchSetFillTarget(soundprefill);
   ResetStream();

   SDL_UnlockAudio();

   return 0;
}

/* Brings the ring to the target fill level WITHOUT discarding audio that has not been played yet.
   Call under SDL_LockAudio. Target = soundprefill frames, the same target ResetStream sets and the regulator
   (SNDSDLUpdateAudio_Stretch, target) controls to. If short, exactly that much silence is appended at the WRITE end
   (soundoffset/soundbuffered as in PushFrame); soundpos and the ring contents stay untouched.
   Afterwards the regulator/stretcher state is reset like ResetStream (soundratio=1.0, soundrebuild, plc_run,
   plc_lastP, fill statistics), so there is no multi-second catch-up at 1.015. The interpolation state (soundfrac,
   soundlastl/r) is zeroed only if silence was appended: the last written frame is then 0 and the join is seamless;
   without an append the interpolation continues from the old last frame.
   only_below_target=1: if the fill level is at or above the target, everything stays untouched (returns 0). */
static int LoadSndFillBlocked(unsigned int *fill_before, unsigned int *filled_up, unsigned int *goal, int only_below_target)
{
   /* The ring lives in sndstretch.c now: report its fill and top it up with
      silence there instead of poking the buffer from this sink. */
   unsigned now = SndStretchFill();
   unsigned want = soundprefill;
   *fill_before = now;
   *goal = want;
   *filled_up = 0;
   if (only_below_target && now >= want)
      return 0;
   if (now < want)
   {
      SndStretchPrefill();
      *filled_up = want - now;
      return 1;
   }
   return 0;
}

/* Pauses SDL output during a lock. SDL_PauseAudio(1) makes the audio thread output silence without calling the
   callback, so nothing is consumed and ring/regulator stay put; the callback never blocks (SDL only takes the device
   lock briefly, a running callback makes the CALLER wait). Never call under SDL_LockAudio.
   action=1 pause - action=0 resume + refill only if below target - action=-1 resume only (saving). */
static int SNDSDLLoadSndPause(int action, unsigned int *fill_amount, unsigned int *filled_up, unsigned int *goal)
{
   int r = 0;
   *fill_amount = 0; *filled_up = 0; *goal = 0;
   if (stereodata16 == NULL)
      return 0;
   if (action == 1)
   {
      SDL_PauseAudio(1);
      SDL_LockAudio();
      *fill_amount = soundbuffered / (sizeof(s16) * 2);
      SDL_UnlockAudio();
      return 1;
   }
   SDL_LockAudio();
   if (action == 0)
      r = LoadSndFillBlocked(fill_amount, filled_up, goal, 1);
   else
      *fill_amount = soundbuffered / (sizeof(s16) * 2);
   SDL_UnlockAudio();
   SDL_PauseAudio(0);
   return r;
}

//////////////////////////////////////////////////////////////////////////////

static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len) {
   u32 i;

   for (i = 0; i < len; i++)
   {
      // Left Channel
      *srcL = ( *srcL *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcL > 0x7FFF) *dst = 0x7FFF;
      else if (*srcL < -0x8000) *dst = -0x8000;
      else *dst = *srcL;
      srcL++;
      dst++;
      // Right Channel
	  *srcR = ( *srcR *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcR > 0x7FFF) *dst = 0x7FFF;
      else if (*srcR < -0x8000) *dst = -0x8000;
      else *dst = *srcR;
      srcR++;
      dst++;
   }
}

static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   SNDSDLUpdateAudio_Stretch(leftchanbuffer, rightchanbuffer, num_samples);
}

// Rewinds the ring to the primed state. The buffer is kept zeroed, so the
// primed region plays as silence. Callers must hold the audio lock, or be
// sure the device is not running yet.
static void ResetStream(void)
{
   SndStretchReset();
}

// Writes one output frame, already scaled and clipped, at the write pointer.
// The audio lock must be held.
/* PushFrame() lives in sndstretch.c now (single implementation). */


// Resampling with a small adaptive trim. The emulator produces 44100/60
// samples per emulated frame, the device consumes at its own 44100 Hz crystal
// rate, and the two never match exactly. The trim is clamped to +-1%
// (YABA_STRETCH_RATIOMIN/MAX), far below an audible pitch change. Larger gaps
// (emulation below full speed) are not stretched; MixAudio_Stretch() covers
// them with PLC or a fade.
static void SNDSDLUpdateAudio_Stretch(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   /* Ring + ratio regulator live in sndstretch.c (shared with libretro). */
   SndStretchWrite((const int32_t *)leftchanbuffer, (const int32_t *)rightchanbuffer, num_samples);
}

//////////////////////////////////////////////////////////////////////////////

static u32 SNDSDLGetAudioSpace(void)
{
   return SNDSDLGetAudioSpace_Stretch();
}

static u32 SNDSDLGetAudioSpace_Stretch(void)
{
   return SndStretchSpace();
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLMuteAudio(void)
{
   muted = 1;
   SndStretchSetMuted(1);
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLUnMuteAudio(void)
{
   muted = 0;
   SndStretchSetMuted(0);
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLSetVolume(int volume)
{
   soundvolume = ( (double)SDL_MIX_MAXVOLUME /(double)100 ) *volume;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport)
{
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

u8 SNDSDLMidiIn(int *isdata)
{
	*isdata = 0;
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

int SNDSDLMidiOut(u8 data)
{
	return 1;
}

//////////////////////////////////////////////////////////////////////////////
#endif
#endif
