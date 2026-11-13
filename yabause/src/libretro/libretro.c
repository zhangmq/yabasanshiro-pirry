/*  YabaSanshiro — libretro port: core entry point.

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
 * PLACEHOLDER CORE — no emulation yet.
 *
 * This exists to make the libretro side of the build real before any emulator
 * code is pulled in:
 *   - proves the .so builds, exports the retro_* ABI and loads in a front end
 *   - proves the video/audio callbacks and the LOG_ENABLED logging layer work
 *     from inside a core
 *   - gives a visible test card instead of a black screen
 *
 * It draws a 320x240 RGB565 colour bar pattern with a moving marker and emits
 * silence. Replace render_test_card()/the audio path with the engine once the
 * "standalone engine + one frame present" step starts; the API plumbing here
 * (callbacks, log sink, pixel format, geometry) is meant to survive that step.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "libretro_core_options.h"   /* core options v2 table */
#include "log.h"
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <GLES3/gl3.h>

#include "yabause.h"
#include "vdp1.h"     /* VIDCore->GetNativeResolution / Resize */
#include "vidogl.h"
#include "sh2core.h"
#include "sh2int.h"   /* SH2CORE_INTERPRETER (core option) */
#include "smpc.h"     /* SmpcSetSystemLanguage (core option) */
#include "scsp.h"
#include "sndlibretro.h"
#include "m68kcore.h"
#include "cdbase.h"
#include "cs2.h"      /* Cs2ForceOpenTray / Cs2ForceCloseTray (disk control) */
#include "memory.h"   /* BupRam / FormatBackupRam: the save-RAM card */
#include "cs0.h"
#include "osdcore.h"
#include "peripheral.h"
#include "stv.h"      /* ST-V romset detection/board bring-up (yabause/src/stv/) */
#define PERCORE_LIBRETRO 2   /* peripheral core id implemented below */
#include "ygl.h"

static struct retro_hw_render_callback hw;

/* One frame per retro_run, and retro_run blocks until that frame exists.
   The machine thread notifies after every VDP event it posts; there is no
   timeout -- a call returns only after a field has been presented. */
static pthread_mutex_t ev_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ev_cv  = PTHREAD_COND_INITIALIZER;
static unsigned ev_posted, ev_seen;

/* Statistics shared by the machine thread (writer) and retro_run (reader). */
static pthread_mutex_t st_mtx = PTHREAD_MUTEX_INITIALIZER;
static double acc_draw_ms, acc_present_ms, acc_wait_ms;
static double acc_machine_ms;
static unsigned acc_machine_n;
static unsigned acc_calls;

static void on_event_posted(void)
{
   pthread_mutex_lock(&ev_mtx);
   ev_posted++;
   pthread_cond_signal(&ev_cv);
   pthread_mutex_unlock(&ev_mtx);
}

static int engine_up;
static const char *libretro_content_path;
static char libretro_bup_path[512];
static char libretro_bios_path[512];
static char libretro_save_dir[512];
static char libretro_system_dir[512];

/* ST-V (Sega Titan Video): the content is a MAME romset zip, not a disc.  The
   engine side lives in yabause/src/stv/ and only the libretro build compiles
   it; everything below is inert until a romset is detected. */
static int   stv_mode;
static char *stv_game;              /* romset name; owned (STVGetSingle strdup) */
static int   stv_rotated;           /* per-title rotation, latched at load time */
static char  stv_bios_path[512];    /* <system>/stvbios.zip */
static char  stv_eeprom_dir[512];   /* <save>/stv/  (per-game .nv) */
static char  stv_bup_path[512];     /* <save>/stv/<game>.ram */
static char  stv_content_path[512]; /* our copy: game->path is call-scoped */

/* The frontend's save-RAM slot (RETRO_MEMORY_SAVE_RAM) is the engine's backup
   card (ported from the reference line's libretro.c). */
#define YABA_INTERNAL_BUP_SIZE 0x10000
static u8  sram_shadow[YABA_INTERNAL_BUP_SIZE];
static int sram_apply_pending = 0; /* push shadow -> BupRam once BupRam exists */
static void set_memory_maps(void);
static void sram_seed_from_backup(const char *path);
static void sram_sync(void);

/* engine-side caller mode: retro_run drains the VDP event queue (A+token) */
extern void Vdp2SetFrontendConsumer(int on);
extern void Vdp2SetRenderThreadHere(void);
extern void Vdp2SetEventPostedHook(void (*fn)(void));
extern int  Vdp2ConsumeFrontendEvent(void);
/* Second half of the engine's VIN handler: limiter + Sync + VIN token, run after the
   present so the limiter measures the swap (see vdp2VBlankIN's comment). */
extern void Vdp2FinishField(void);
extern void Vdp2AbortJoins(int on);
extern void Vdp2DrainQueueNoRender(void);
extern unsigned Vdp2FieldSeq(void);
extern void EnableAutoFrameSkip(void);
extern void DisableAutoFrameSkip(void);
extern int  g_perline_requested;
/* Config-driven globals of the standalone (main.cpp:346/364) that the engine
   reads directly and whose engine defaults differ from the standalone's. */
extern int  g_scsp_dsp_off;      /* scsp.c:163 defaults to 1 = SCSP DSP off   */
extern int  g_texcache_persist;  /* yglcache.c:49 defaults to 0               */
extern int  g_emulated_bios;   /* port_stubs.c: 0 when a real BIOS is used */
extern void YabauseSetDecilineMode(int on);

static pthread_t machine_thread;
static volatile int machine_run;
static volatile int machine_done = 1;
static int machine_started;

/* ---- machine-thread commands (save state / load state / reset) ------------
 * The engine's state belongs to the machine thread, while libretro's
 * serialize/unserialize/reset arrive on the frontend thread.  The command is
 * queued and executed by the machine at its next field boundary; while waiting,
 * the frontend thread keeps pumping the VDP event queue, because the machine
 * may be parked in a join that only that event handling releases. */
enum { YAB_CMD_NONE = 0, YAB_CMD_SAVE, YAB_CMD_LOAD, YAB_CMD_RESET, YAB_CMD_DISC };

#define YAB_PATH_MAX 512

static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cmd_type = YAB_CMD_NONE;
static int cmd_done = 1;
static int cmd_ok = 0;
static const void *cmd_load_buf;
static size_t cmd_load_size;
static void *cmd_save_buf;
static size_t cmd_save_size;
static size_t state_size;          /* stable retro_serialize_size() */
/* YAB_CMD_DISC payload: open the tray (path unused), or close it on this image. */
static int cmd_disc_eject;
static char cmd_disc_path[YAB_PATH_MAX];

extern int YabSaveStateBuffer(void **buffer, size_t *size);
extern int YabLoadStateBuffer(const void *buffer, size_t size);
/* Engine-side repair after a state load/reset: the state restores Vdp1Regs/
   Vdp2Regs/RAM but not the renderer's field state machine (queues, snapshots,
   VDP1 status), so the machine can end up parked.  Same hook the reference
   line calls from its state command. */
extern void Vdp2AfterLoadState(void);
extern int stv_eeprom_persist;   /* stv.c: gates EEPROM read and write */
static volatile int g_mode_pending;   /* defined further down; used by the state command */

/* Engine work of one command.  Runs on the machine thread normally, and on the
   frontend thread when no machine thread exists (nothing can race then). */
static int yab_cmd_exec(int type, const void *load, size_t load_size,
                        int disc_eject, const char *disc_path)
{
   int ok = 0;
   if (type == YAB_CMD_SAVE) {
      void *buf = NULL;
      size_t sz = 0;
      if (YabSaveStateBuffer(&buf, &sz) == 0) {
         cmd_save_buf = buf;
         cmd_save_size = sz;
         ok = 1;
      } else {
         free(buf);
      }
   } else if (type == YAB_CMD_LOAD) {
      ok = (YabLoadStateBuffer(load, load_size) == 0) ? 1 : 0;
      if (ok) {
         Vdp2AfterLoadState();
         SNDLibretroDropBacklog();
         /* A state can carry a different video mode: let the render thread
            rebuild the geometry/GL objects before it draws the next field. */
         g_mode_pending = 1;
      }
   } else if (type == YAB_CMD_RESET) {
      YabauseReset();
      Vdp2AfterLoadState();
      g_mode_pending = 1;
      ok = 1;
   } else if (type == YAB_CMD_DISC) {
      /* The CD block belongs to the machine thread (Cs2Exec runs there), so the
         tray moves from here; closing it re-opens the ISO core on that image. */
      if (disc_eject) {
         Cs2ForceOpenTray();
         ok = 1;
      } else if (disc_path != NULL && disc_path[0] != '\0') {
         ok = (Cs2ForceCloseTray(CDCORE_ISO, disc_path) == 0) ? 1 : 0;
      }
   }
   return ok;
}

/* Machine thread, at the field boundary. */
static void machine_cmd_step(void)
{
   int type;
   const void *load;
   size_t n;
   int ok;
   int eject;
   char path[YAB_PATH_MAX];

   pthread_mutex_lock(&cmd_mtx);
   type = cmd_type;
   load = cmd_load_buf;
   n = cmd_load_size;
   eject = cmd_disc_eject;
   snprintf(path, sizeof(path), "%s", cmd_disc_path);
   pthread_mutex_unlock(&cmd_mtx);
   if (type == YAB_CMD_NONE)
      return;
   ok = yab_cmd_exec(type, load, n, eject, path);
   pthread_mutex_lock(&cmd_mtx);
   cmd_ok = ok;
   cmd_type = YAB_CMD_NONE;
   cmd_done = 1;
   pthread_mutex_unlock(&cmd_mtx);
}

/* Frontend thread. */
static int yab_cmd_wait_ex(int type, const void *load, size_t load_size,
                           int disc_eject, const char *disc_path)
{
   int ok;
   if (!machine_started)
      return yab_cmd_exec(type, load, load_size, disc_eject, disc_path);
   pthread_mutex_lock(&cmd_mtx);
   cmd_load_buf = load;
   cmd_load_size = load_size;
   cmd_disc_eject = disc_eject;
   if (disc_path != NULL)
      snprintf(cmd_disc_path, sizeof(cmd_disc_path), "%s", disc_path);
   else
      cmd_disc_path[0] = '\0';
   cmd_ok = 0;
   cmd_done = 0;
   cmd_type = type;
   pthread_mutex_unlock(&cmd_mtx);
   for (;;) {
      int done;
      pthread_mutex_lock(&cmd_mtx);
      done = cmd_done;
      pthread_mutex_unlock(&cmd_mtx);
      if (done)
         break;
      /* Keep the renderer alive: the machine may be blocked in a join waiting
         for this very thread to consume a VDP event. */
      while (Vdp2ConsumeFrontendEvent()) { }
      usleep(200);
   }
   pthread_mutex_lock(&cmd_mtx);
   ok = cmd_ok;
   pthread_mutex_unlock(&cmd_mtx);
   return ok;
}

/* Save / load / reset carry no payload of their own. */
static int yab_cmd_wait(int type, const void *load, size_t load_size)
{
   return yab_cmd_wait_ex(type, load, load_size, 0, NULL);
}

/* Disk control, called from the frontend thread (its menu, between frames). */
static int yab_disc_cmd(int eject, const char *path)
{
   return yab_cmd_wait_ex(YAB_CMD_DISC, NULL, 0, eject, path);
}

static void *machine_main(void *arg)
{
   (void)arg;
   machine_done = 0;
   while (machine_run) {
      struct timespec m0, m1;
      machine_cmd_step();   /* save/load/reset run at the field boundary */
      if (!machine_run)
         break;
      clock_gettime(CLOCK_MONOTONIC, &m0);
      /* One machine frame through the peripheral core: PERDummy just runs
         YabauseExec(), PERLIBRETRO applies the published pad state first. */
      if (PERCore != NULL)
         PERCore->HandleEvents();
      else
         YabauseExec();
      sram_sync();
      clock_gettime(CLOCK_MONOTONIC, &m1);
      pthread_mutex_lock(&st_mtx);
      acc_machine_ms += (m1.tv_sec - m0.tv_sec) * 1e3 + (m1.tv_nsec - m0.tv_nsec) / 1e6;
      acc_machine_n++;
      pthread_mutex_unlock(&st_mtx);
   }
   machine_done = 1;
   return NULL;
}

#ifdef LOG_ENABLED
#define CORE_VERSION "1.6.2 (placeholder core)"
#else
#define CORE_VERSION "1.6.2 (placeholder core, logging off)"
#endif

#define TEST_W 320
#define TEST_H 240
#define AUDIO_FRAMES 735 /* 44100 Hz / 60 fps */
#define FRAME_LOG_INTERVAL 60

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_sample_cb;
static retro_audio_sample_batch_t audio_batch_cb;

/* Async audio (RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, env 22).  When the front
   end accepts it, the front end's audio thread calls yab_audio_callback() when
   it can take audio, and retro_run does no audio work at all; otherwise the
   consumer thread of sndlibretro.c submits in 512-frame periods as before.
   Neither path ever calls the audio callbacks from retro_run(). */
static struct retro_audio_callback audio_cb_cfg;
static int audio_async_ok;       /* env 22 accepted by this front end */
static int audio_async_on;       /* engine is running on the async path */
static s16 async_pcm[512 * 2];   /* one period, filled by SNDLibretroPump() */
static unsigned aud_calls, aud_short;

/* RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK (env 62): the front end
   tells us how full ITS audio buffer is and whether it is about to run dry.
   That is the only protocol-level view a core gets of the front end's side, and
   it is what decides how much audio to hand over per notification: one period
   in steady state, a burst while the front end is starving (its buffer empty
   after a resume or a load).  Called from the front end's thread. */
static volatile int aud_occ = -1;          /* -1 = no report yet */
static volatile int aud_underrun_likely;
static volatile int aud_buf_active;
/* Frames handed over per callback: the number that decides whether a front end
   can drain the core's buffer in one pull (minarch asked for it). */
static unsigned aud_wmax, aud_wmin = 0xFFFFFFFFu;
static double aud_us_sum, aud_us_max;
/* Delivery budget: the front end may call the env 22 callback far more often
   than its device consumes audio (minarch does when its ring is starved:
   measured 40k calls/s), and every call would otherwise hand over a period,
   so 99% of what the front end played was concealment and the spin starved
   the SCSP thread.  A token bucket against the device rate fixes the average:
   hand a period over only when the device has consumed what we already sent.
   Frames are 44100/s whatever the video standard is. */
static unsigned long long aud_rate_start_us;   /* token bucket epoch */
static unsigned long long aud_rate_frames;     /* frames handed over since then */
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static retro_log_printf_t frontend_log;

/* ------------------------------------------------------------------------- *
 * Resolution / geometry (libretro protocol)
 *
 * The engine renders the Saturn mode itself (yinit.resolution_mode =
 * RES_ORIGINAL, the standalone's -r 3), so the frame the front end gets is the
 * native mode: 320x224 / 352x224 / 352x240 / 640x480 / 704x448 (double-height
 * interlace).  The mode changes while a game runs, so this is polled once per
 * field and published with RETRO_ENVIRONMENT_SET_GEOMETRY (allowed only inside
 * retro_run) before that field is drawn.
 * ------------------------------------------------------------------------- */
#define GEOMETRY_MAX_W 704            /* Saturn native maximum */
#define GEOMETRY_MAX_H 512

void *LibretroFrontFbo(void);

static int game_width  = 320;         /* current native Saturn mode */
static int game_height = 224;
static int frame_width  = 0;          /* size last reported to the front end */
static int frame_height = 0;

/* Called by VIDOGLVdp2SetResolution() (vidogl.c) on the machine thread, at the
   guest's TVMD write.  It only marks the change: the GL-side resize, the FBO pin
   and SET_GEOMETRY must run on the frontend thread, in core_apply_resolution(). */
static volatile int g_mode_pending;
void Vdp2ModeChanged(int native_w, int native_h)
{
   if (native_w <= 0 || native_h <= 0)
      return;
   if (native_w == game_width && native_h == game_height && !g_mode_pending)
      return;
   g_mode_pending = 1;
}

static void core_apply_resolution(void)
{
   int nw = 0, nh = 0, interlace = 0;
   GLuint fb;
   struct retro_system_av_info info;

   if (VIDCore == NULL || VIDCore->GetNativeResolution == NULL)
      return;
   VIDCore->GetNativeResolution(&nw, &nh, &interlace);
   if (nw <= 0 || nh <= 0)
      return;
   if (!g_mode_pending && frame_width > 0 && nw == game_width && nh == game_height)
      return;
   g_mode_pending = 0;

   game_width = nw;
   game_height = nh;
   frame_width = nw;
   frame_height = nh;

   /* on = 1, aspect = FULL: the engine must not letterbox its own frame (the
      front end scales), and screen_width/height must follow the frame so that
      SetSaturnResolution() keeps GlWidth/GlHeight at the frame size. */
   fb = (GLuint)(uintptr_t)LibretroFrontFbo();
   if (fb == 0)
      return;
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   VIDCore->Resize(0, 0, (unsigned)frame_width, (unsigned)frame_height, 1, FULL);
   /* YglGLInit() inside Resize re-reads the current framebuffer binding; pin the
      front end's FBO again so the engine does not fall back to 0. */
   _Ygl->default_fbo = fb;
   /* With RES_ORIGINAL, _Ygl->width/height are only recomputed here; without it
      the engine keeps rendering the first mode's size (hi-res modes would stay
      at 320x224 internally even though the TVMD mode changed). */
   YglRebuildGramebuffer();

   if (environ_cb != NULL) {
      retro_get_system_av_info(&info);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
   }
   LOG_INFO("resolution: %dx%d (interlace=%d)", frame_width, frame_height, interlace);
}


static uint16_t *frame_buf;
static int16_t *audio_buf;
static unsigned frame_count;
static unsigned frame_ready;
static unsigned frame_sent;
static unsigned drained_total, drained_none;

/* Forward everything the logging layer produces to the front end, keeping the
   line and the severity (LOG_LVL_* values are RETRO_LOG_* values). */
/* Called by the platform layer (yui_libretro.c) at the engine's present point. */
void LibretroPresentFrame(void)
{
   /* Called from YuiSwapBuffers(), i.e. inside the engine's YglRender stack.
      Only record it here: video_cb must run outside the engine stack, where the
      frontend can safely draw its own present/HUD/swap on this context. */
   frame_ready++;
}

static void log_gl_state(const char *tag)
{
   GLint fbo = -1, prog = -1, vao = -1;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
   glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   LOG_DEBUG("gl[%s]: fbo=%d program=%d vao=%d err=0x%x", tag, fbo, prog, vao,
            (unsigned)glGetError());
}

/* Leave the context in a state the frontend can initialise its own pipeline in. */
static void reset_gl_state_for_frontend(const char *tag)
{
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glUseProgram(0);
   glBindVertexArray(0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_STENCIL_TEST);
   glActiveTexture(GL_TEXTURE0);
   log_gl_state(tag);
}

void *LibretroFrontFbo(void)
{
   return hw.get_current_framebuffer ? (void *)(uintptr_t)hw.get_current_framebuffer() : NULL;
}

int LibretroHasVideo(void)
{
   return video_cb != NULL;
}

/* Audio submit hook: called from the consumer thread in sndlibretro.c. */
static void libretro_audio_submit(const int16_t *pcm, unsigned frames)
{
   if (audio_batch_cb != NULL)
      audio_batch_cb(pcm, frames);
}

static unsigned yab_audio_want_periods(void);   /* env 62 driven, defined below */

/* Async path: called by the front end's audio thread, on its schedule.  The
   protocol requires the normal audio callback to run from inside this callback,
   which is why the submit happens here and not in retro_run(). */
static void yab_audio_callback(void)
{
   struct timespec a, b;
   unsigned n, got;
   double us;

   if (!audio_async_on || audio_batch_cb == NULL)
      return;

   /* Hand over one period in steady state; when env 62 says the front end's
      buffer is empty (resume, load, slow frame) hand several, so its device
      never starves.  Each period is still one audio callback call. */
   unsigned want = yab_audio_want_periods();

   /* Never hand over more audio than the device can play.  A front end that
      reports a healthy buffer and asks again before the device consumed what we
      already sent gets nothing (it asks again later).  Exception: it says it is
      starving, then refill immediately. */
   {
      struct timespec now_ts;
      unsigned long long now_us, due, limit;

      clock_gettime(CLOCK_MONOTONIC, &now_ts);
      now_us = (unsigned long long)now_ts.tv_sec * 1000000ULL +
               (unsigned long long)now_ts.tv_nsec / 1000ULL;
      if (aud_rate_start_us == 0)
         aud_rate_start_us = now_us;
      due = (now_us - aud_rate_start_us) * 44100ULL / 1000000ULL;
      /* How much audio we may be ahead of what the device has played: one
         period.  The front end asks again as soon as its device consumed the
         previous one, so this is what keeps a spin (minarch calls this callback
         3k times/s while starved) from draining our ring while handing the front
         end audio it cannot take (measured: short 15-21/s with two periods of
         lookahead).  env 62's occupancy is NOT usable here: minarch only reports
         it every 5 s, so gating on it starved the front end instead. */
      limit = 512ULL;
      if (aud_rate_frames > due + limit)
         return;
      /* A long gap (pause, load) must not become a catch-up burst. */
      if (due > aud_rate_frames + 44100ULL * 2ULL)
         aud_rate_start_us = now_us - aud_rate_frames * 1000000ULL / 44100ULL;
   }
   unsigned sent = 0, total = 0;

   while (sent < want)
   {
      n = SNDLibretroPump(async_pcm);
      if (n == 0)
         break;               /* paused, or nothing left to hand over */
      clock_gettime(CLOCK_MONOTONIC, &a);
      got = (unsigned)audio_batch_cb(async_pcm, n);
      clock_gettime(CLOCK_MONOTONIC, &b);
      us = (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;

      aud_calls++;
      aud_us_sum += us;
      if (us > aud_us_max)
         aud_us_max = us;
      if (got < n)
         aud_short++;
      sent++;
      total += n;
   }
   aud_rate_frames += total;
   /* wmax/wmin are frames per NOTIFICATION (one front end callback may now
      carry several periods), which is what a front end can observe. */
   if (total > aud_wmax) aud_wmax = total;
   if (total && total < aud_wmin) aud_wmin = total;
}

static void yab_audio_buffer_status(bool active, unsigned occupancy,
                                   bool underrun_likely)
{
   __atomic_store_n(&aud_buf_active, active ? 1 : 0, __ATOMIC_RELAXED);
   __atomic_store_n(&aud_occ, (int)occupancy, __ATOMIC_RELAXED);
   __atomic_store_n(&aud_underrun_likely, underrun_likely ? 1 : 0, __ATOMIC_RELAXED);
}

/* How many periods to hand over this time.  "The amount of audio data to write
   is up to the core" (libretro.h), and env 62 is where the front end tells us
   its side is empty: refill it then, one period otherwise.  The bound is the
   front end's own buffer (a few periods), not a size we invent. */
static unsigned yab_audio_want_periods(void)
{
   /* Always exactly one period.  The front end's ring is small (minarch:
       2048 frames = two periods), so handing four periods at once overflows it
       and the front end drops the excess: measured with a PAL game, "short"
       (the front end accepted less than we sent) was 22-25 periods/s, audible
       as clicks/hiss on top of the music.  Refilling a starving ring is the
       front end's own business; it asks again when it has room. */
   return 1;
}

/* env 22's set_state is about the front end's audio driver (active/inactive),
   not about a game pause, and libretro has no pause environment call.  Audio
   writes are already gated by the front end calling us or not, so this only
   gets logged; pause detection uses the retro_run heartbeat (sndlibretro.h). */
static void yab_audio_set_state(bool enabled)
{
   LOG_DEBUG("audio: front end audio driver %s", enabled ? "active" : "inactive");
}

/* ------------------------------------------------------------------------- *
 * Core options (libretro protocol)
 *
 * The table is in libretro_core_options.h; this block reads the values and
 * applies them.  Two kinds:
 *   - restart-type (the name carries "(restart)"): latched in
 *     LibretroEngineContextReset(), i.e. read when the engine is built.  The
 *     front end cannot restart a core by itself, so the name is the only signal
 *     it can show; see REQ-CORE-OPTIONS-20261111.md.
 *   - live: applied from retro_run, on the render thread with the front end's
 *     context current.  Values that rebuild GL objects are only touched there,
 *     before the field's VDP events are consumed.
 * ------------------------------------------------------------------------- */
enum { YAB_VF_AUTO = 0, YAB_VF_NTSC = 1, YAB_VF_PAL = 2 };

static int  opt_videoformat = YAB_VF_AUTO;
static int  opt_videoformat_latched = VIDEOFORMATTYPE_NTSC;  /* for av_info/region */
static int  opt_resolution_mode = 3;            /* RES_ORIGINAL */
static int  opt_rbg_resolution_mode = 0;        /* RBG_RES_ORIGINAL */
static int  opt_rbg_use_compute_shader = 1;
static int  opt_texcache_persist = 0;   /* off by default: helps few games, hurts some */
static int  opt_interlace_half = 0;
static int  opt_frameskip = 0;
static int  opt_sh2coretype = 3;                /* dynarec (DYNAREC_DEVMIYAX) */
static int  opt_addon_cart = CART_DRAM32MBIT;
static int  opt_force_hle_bios = 0;
static int  opt_system_language = 0;
static int  opt_multitap[2];
static int  opt_scsp_dsp = 1;
static int  opt_cd_volume = 100;
static int  opt_mute_cdda;                     /* ScspSetCddaMute, machine */
static int  opt_sfx_volume = 100;
static int  opt_stv_region = STV_REGION_EU;   /* ST-V BIOS region preference */
static int  opt_stv_eeprom_save;                /* write <save>/stv/<game>.nv on exit? */
static int  opt_stv_service;                  /* Test/Service/Pause keys */

/* Engine-side hooks without a header of their own. */
extern int  g_interlace_half_menu;      /* vidogl.c:3619, read per draw          */
extern void ScspSetCddaVolume(int v);   /* scsp.c:1962, read per mix             */
extern void ScspSetCddaMute(int on_val); /* scsp.c:5707, read per sample          */
extern void ScspSetSfxVolume(int v);    /* scsp.c:1963, read per mix             */

/* Applied-state cache: the engine already has these after an init, so the first
   retro_run does not re-apply (a resolution change rebuilds GL objects). */
static int  applied_resolution = -1, applied_rbg_res = -1, applied_rbg_cs = -1;
static int  applied_frameskip = -1, applied_dsp = -1, applied_cdvol = -1;
static int  applied_mute_cdda = -1;
static int  applied_sfxvol = -1, applied_interlace = -1, applied_texcache = -1;
static int  applied_multitap[2] = { -1, -1 };

static const char *opt_str(const char *key)
{
   struct retro_variable var;

   if (environ_cb == NULL)
      return NULL;
   var.key = key;
   var.value = NULL;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      return NULL;
   return var.value;
}

static int opt_is(const char *key, const char *value)
{
   const char *v = opt_str(key);
   return v != NULL && strcmp(v, value) == 0;
}

/* Read every option into the state above (no engine work). */
static void yab_options_read(void)
{
   const char *v;

   if (opt_is("yabasanshiro_videoformattype", "PAL"))       opt_videoformat = YAB_VF_PAL;
   else if (opt_is("yabasanshiro_videoformattype", "NTSC")) opt_videoformat = YAB_VF_NTSC;
   else                                                     opt_videoformat = YAB_VF_AUTO;

   if (opt_is("yabasanshiro_resolution_mode", "2x"))        opt_resolution_mode = 2;
   else if (opt_is("yabasanshiro_resolution_mode", "4x"))   opt_resolution_mode = 1;
   else                                                     opt_resolution_mode = 3;

   if (opt_is("yabasanshiro_rbg_resolution_mode", "2x"))         opt_rbg_resolution_mode = 1;
   else if (opt_is("yabasanshiro_rbg_resolution_mode", "720p"))  opt_rbg_resolution_mode = 2;
   else if (opt_is("yabasanshiro_rbg_resolution_mode", "1080p")) opt_rbg_resolution_mode = 3;
   else                                                          opt_rbg_resolution_mode = 0;

   opt_rbg_use_compute_shader = !opt_is("yabasanshiro_rbg_use_compute_shader", "disabled");
   opt_texcache_persist       =  opt_is("yabasanshiro_texcache_persist", "enabled");
   opt_interlace_half         =  opt_is("yabasanshiro_interlace_half", "enabled");
   opt_frameskip              =  opt_is("yabasanshiro_frameskip", "auto");

   opt_sh2coretype = opt_is("yabasanshiro_sh2coretype", "interpreter")
                     ? SH2CORE_INTERPRETER : 3;

   if (opt_is("yabasanshiro_addon_cart", "none"))                 opt_addon_cart = CART_NONE;
   else if (opt_is("yabasanshiro_addon_cart", "1M_extended_ram")) opt_addon_cart = CART_DRAM8MBIT;
   else                                                           opt_addon_cart = CART_DRAM32MBIT;

   opt_force_hle_bios = opt_is("yabasanshiro_force_hle_bios", "enabled");

   if (opt_is("yabasanshiro_system_language", "german"))        opt_system_language = 1;
   else if (opt_is("yabasanshiro_system_language", "french"))   opt_system_language = 2;
   else if (opt_is("yabasanshiro_system_language", "spanish"))  opt_system_language = 3;
   else if (opt_is("yabasanshiro_system_language", "italian"))  opt_system_language = 4;
   else if (opt_is("yabasanshiro_system_language", "japanese")) opt_system_language = 5;
   else                                                         opt_system_language = 0;

   opt_multitap[0] = opt_is("yabasanshiro_multitap_port1", "enabled");
   opt_multitap[1] = opt_is("yabasanshiro_multitap_port2", "enabled");

   opt_scsp_dsp = !opt_is("yabasanshiro_scsp_dsp", "disabled");

   v = opt_str("yabasanshiro_cd_volume");
   opt_cd_volume = v != NULL ? atoi(v) : 100;
   opt_mute_cdda = opt_is("yabasanshiro_mute_cdda", "enabled");
   v = opt_str("yabasanshiro_sfx_volume");
   opt_sfx_volume = v != NULL ? atoi(v) : 100;

   /* ST-V only.  The BIOS region is a preference among the BIOSes a cart's
      header allows (stv.c picks one of those); some Japanese carts only come
      up on the JP one. */
   if (opt_is("yabasanshiro_stv_region", "US"))      opt_stv_region = STV_REGION_US;
   else if (opt_is("yabasanshiro_stv_region", "JP")) opt_stv_region = STV_REGION_JP;
   else if (opt_is("yabasanshiro_stv_region", "TW")) opt_stv_region = STV_REGION_TW;
   else                                              opt_stv_region = STV_REGION_EU;
   opt_stv_service = opt_is("yabasanshiro_service_enabled", "enabled");
   /* Off by default: the ST-V EEPROM is machine NVRAM (region, coin mode,
      credits, bookkeeping) and writing it changes the user's save directory
      without anything asking for it. */
   opt_stv_eeprom_save = opt_is("yabasanshiro_stv_eeprom_save", "enabled");
}

/* Latch the video standard for this content.  "auto" reads the disc IP.BIN
   region (E/A/L = PAL, J/U/T/K/B = NTSC), the same rule the engine's SMPC
   autodetect uses; unreadable images stay NTSC.  Runs in retro_load_game: the
   front end asks for retro_get_system_av_info() before the engine exists. */
static void yab_videoformat_latch(void)
{
   int pal = (opt_videoformat == YAB_VF_PAL);

   if (stv_mode) {
      /* ST-V hardware has no PAL mode, and there is no disc header to read. */
      opt_videoformat_latched = VIDEOFORMATTYPE_NTSC;
      LOG_INFO("option: video format ST-V -> NTSC 60Hz%s",
               opt_videoformat == YAB_VF_PAL ? " (PAL option ignored)" : "");
      return;
   }

   if (opt_videoformat == YAB_VF_AUTO) {
      char region[17];
      if (libretro_content_path != NULL &&
          ISOCDReadRegionCode(libretro_content_path, region, sizeof(region)) == 0) {
         switch (region[0]) {
            case 'E': case 'A': case 'L': pal = 1; break;
            case 'J': case 'U': case 'T': case 'K': case 'B': pal = 0; break;
            default: pal = 0; break;
         }
         LOG_INFO("option: video format auto -> region '%s' = %s",
                  region, pal ? "PAL 50Hz" : "NTSC 60Hz");
      } else {
         LOG_INFO("option: video format auto, disc region unreadable -> NTSC 60Hz");
      }
   } else {
      LOG_INFO("option: video format forced %s", pal ? "PAL 50Hz" : "NTSC 60Hz");
   }
   opt_videoformat_latched = pal ? VIDEOFORMATTYPE_PAL : VIDEOFORMATTYPE_NTSC;
}

/* The engine has just been built with the option values: remember them, so the
   live applier only writes what really changed. */
static void yab_options_seed(void)
{
   applied_resolution = opt_resolution_mode;
   applied_rbg_res    = opt_rbg_resolution_mode;
   applied_rbg_cs     = opt_rbg_use_compute_shader;
   applied_frameskip  = opt_frameskip;
   applied_dsp        = opt_scsp_dsp;
   applied_cdvol      = opt_cd_volume;
   applied_mute_cdda  = opt_mute_cdda;
   applied_sfxvol     = opt_sfx_volume;
   applied_interlace  = opt_interlace_half;
   applied_texcache   = opt_texcache_persist;
   applied_multitap[0] = opt_multitap[0];
   applied_multitap[1] = opt_multitap[1];
}

/* Push changed live options into the engine.  retro_run only (render thread,
   front end context current); the resolution change rebuilds GL objects. */
static void yab_options_apply(void)
{
   if (opt_texcache_persist != applied_texcache) {
      applied_texcache = opt_texcache_persist;
      g_texcache_persist = opt_texcache_persist;
      LOG_INFO("option: texture cache persist %s", opt_texcache_persist ? "on" : "off");
   }
   if (opt_interlace_half != applied_interlace) {
      applied_interlace = opt_interlace_half;
      g_interlace_half_menu = opt_interlace_half;
      LOG_INFO("option: interlace backgrounds %s", opt_interlace_half ? "on" : "off");
   }
   if (opt_scsp_dsp != applied_dsp) {
      applied_dsp = opt_scsp_dsp;
      g_scsp_dsp_off = opt_scsp_dsp ? 0 : 1;
      LOG_INFO("option: SCSP DSP %s", opt_scsp_dsp ? "on" : "off");
   }
   if (opt_mute_cdda != applied_mute_cdda) {
      applied_mute_cdda = opt_mute_cdda;
      ScspSetCddaMute(opt_mute_cdda);
      LOG_INFO("option: mute CDDA %s", opt_mute_cdda ? "on" : "off");
   }
   if (opt_cd_volume != applied_cdvol) {
      applied_cdvol = opt_cd_volume;
      ScspSetCddaVolume(opt_cd_volume);
      LOG_INFO("option: CD volume %d", opt_cd_volume);
   }
   if (opt_sfx_volume != applied_sfxvol) {
      applied_sfxvol = opt_sfx_volume;
      ScspSetSfxVolume(opt_sfx_volume);
      LOG_INFO("option: SFX volume %d", opt_sfx_volume);
   }
   if (opt_multitap[0] != applied_multitap[0] || opt_multitap[1] != applied_multitap[1]) {
      applied_multitap[0] = opt_multitap[0];
      applied_multitap[1] = opt_multitap[1];
      /* No multitap support in the peripheral core yet: record the choice so it
         is not silently ignored, see REQ-CORE-OPTIONS-20261111.md. */
      LOG_INFO("option: 6Player adaptor port1=%d port2=%d (not implemented yet)",
               opt_multitap[0], opt_multitap[1]);
   }

   if (!engine_up)
      return;

   if (opt_rbg_resolution_mode != applied_rbg_res) {
      applied_rbg_res = opt_rbg_resolution_mode;
      VideoSetSetting(VDP_SETTING_RBG_RESOLUTION_MODE, opt_rbg_resolution_mode);
      LOG_INFO("option: RBG resolution mode %d", opt_rbg_resolution_mode);
   }
   if (opt_rbg_use_compute_shader != applied_rbg_cs) {
      applied_rbg_cs = opt_rbg_use_compute_shader;
      VideoSetSetting(VDP_SETTING_RBG_USE_COMPUTESHADER, opt_rbg_use_compute_shader);
      LOG_INFO("option: RBG compute shader %s", opt_rbg_use_compute_shader ? "on" : "off");
   }
   if (opt_frameskip != applied_frameskip) {
      applied_frameskip = opt_frameskip;
      /* Only the skip decision is toggled, like the standalone's
         EnableAutoFrameSkip()/DisableAutoFrameSkip(): the frame limit stays what YabauseInit()
         set (yinit.framelimit = 0, the 60/50 Hz schedule with its sleep).  VDP2SetFrameLimit()
         re-anchors the limiter (framecount/onesecondticks/lastticks = 0), so calling it on a
         skip toggle would wipe the lateness the decision accumulates. */
      if (opt_frameskip)
         EnableAutoFrameSkip();
      else
         DisableAutoFrameSkip();
      LOG_INFO("option: autoframeskip %s", opt_frameskip ? "on" : "off");
   }
   if (opt_resolution_mode != applied_resolution) {
      applied_resolution = opt_resolution_mode;
      /* Rebuilds the engine's framebuffers (vidogl.c VIDOGLSetSettingValueMode);
         safe here: this thread owns the GL context and the machine thread does
         not touch GL. */
      VideoSetSetting(VDP_SETTING_RESOLUTION_MODE, opt_resolution_mode);
      LOG_INFO("option: resolution mode %d", opt_resolution_mode);
   }
}

void LibretroEngineContextReset(void)
{
   if (engine_up) return;
   LOG_INFO("engine: YabauseInit (frontend consumer)");
   log_gl_state("before-init");
   Vdp2SetFrontendConsumer(1);
   {
      static yabauseinit_struct yinit;
      memset(&yinit, 0, sizeof(yinit));
      yinit.sh2coretype     = opt_sh2coretype;   /* core option "SH2 Core" */
      yinit.vidcoretype     = VIDCORE_OGL;
      yinit.sndcoretype     = SNDCORE_LIBRETRO;   /* SCSP thread -> retro_run -> audio_batch_cb */
      yinit.m68kcoretype    = M68KCORE_MUSASHI;
      yinit.percoretype     = PERCORE_LIBRETRO;   /* pad on port 1, see PERLIBRETROJoy */
      yinit.carttype        = opt_addon_cart;    /* core option "Addon Cartridge" */
      yinit.clocksync       = 0;
      yinit.use_new_scsp    = 1;
      yinit.extend_backup   = 1;
      yinit.video_filter_type = 0;
      yinit.polygon_generation_mode = PERSPECTIVE_CORRECTION;
      yinit.frameskip       = 0;
      yinit.resolution_mode = opt_resolution_mode;   /* core option, default RES_ORIGINAL */
      yinit.rbg_use_compute_shader = opt_rbg_use_compute_shader ? true : false;
      yinit.cdcoretype      = CDCORE_ISO;
      /* The rest of the standalone's yinit (main.cpp), field by field: the engine
         branches on these, so leaving them at their zero value is a silent config
         divergence (e.g. scsp_main_mode 0 = CpuTime changes the SCSP thread and
         makes SyncCPUtoSCSP wait every field).  See CONFIG-ALIGN-20261109.md. */
      yinit.scsp_main_mode  = SCSP_MAIN_MODE_MACHINE_TOKEN; /* SCSP follows the
                              machine's emulated time, one-way: the 68k cannot
                              run ahead of the SH-2 while this thread sits in a
                              renderer join, and the machine never waits for the
                              SCSP.  See the thread comment in scsp.c. */
      yinit.scsp_sync_count_per_frame = 1;    /* g_scsp_sync */
      yinit.rotate_screen   = 0;
      yinit.rbg_resolution_mode = opt_rbg_resolution_mode;
      yinit.basetime        = 0;
      yinit.cartpath        = NULL;
      yinit.mpegpath        = NULL;
      yinit.regionid        = 0;
      yinit.biospath        = (opt_force_hle_bios || !libretro_bios_path[0])
                              ? NULL : libretro_bios_path;   /* "Force HLE BIOS" */
      yinit.buppath         = libretro_bup_path[0] ? libretro_bup_path : NULL;
      yinit.cdpath          = libretro_content_path;
      yinit.videoformattype = opt_videoformat_latched;   /* "Video Format", latched at load */
      yinit.framelimit      = 0;
      yinit.usethreads      = 0;
      yinit.osdcoretype     = OSDCORE_DUMMY;
      yinit.skip_load       = 0;
      if (stv_mode) {
         /* ST-V board: no disc and no Saturn BIOS; stv.c assembles the romset
            into the CART_ROMSTV image and installs the ST-V BIOS in BiosRom.
            The backup RAM is the board's own 64 KB card, per game (Kronos). */
         yinit.carttype        = CART_ROMSTV;
         yinit.cdcoretype      = CDCORE_DUMMY;
         yinit.cdpath          = NULL;
         yinit.biospath        = NULL;
         yinit.extend_backup   = 0;
         yinit.buppath         = stv_bup_path[0] ? stv_bup_path : NULL;
         yinit.stvgamepath     = libretro_content_path;
         yinit.stvgame         = stv_game;
         yinit.stvbiospath     = stv_bios_path;
         yinit.eepromdir       = stv_eeprom_dir;
         yinit.stv_favorite_region = opt_stv_region;
         LOG_INFO("engine: ST-V mode romset=%s region=%s eeprom=%s",
                  stv_game ? stv_game : "(none)",
                  opt_stv_region == STV_REGION_JP ? "JP" :
                  opt_stv_region == STV_REGION_US ? "US" :
                  opt_stv_region == STV_REGION_TW ? "TW" : "EU",
                  stv_eeprom_dir);
      }
      g_emulated_bios = (yinit.biospath != NULL) ? 0 : 1;   /* standalone: 0 when -b */
      if (stv_mode)
         g_emulated_bios = 0;   /* the ST-V BIOS is real, installed by stv.c */
      /* Standalone config defaults (main.cpp): "sound dsp" = 1 -> DSP on.  The
         DSP is emulated hardware state, so it stays on.
         Texture-cache persistence is a core option and defaults OFF: the fixed
         savestate measurement that once made it the default (DoA in-game, 39 ->
         55 fps) is the exception - most titles gain little, and some regress
         because the atlas grows and the dirty-band uploads get bigger.  The
         option ("Texture Cache Persist", enabled) turns it back on,
         YAB_TEXCACHE_PERSIST=0/1 still overrides it for A/B runs. */
      g_scsp_dsp_off = opt_scsp_dsp ? 0 : 1;   /* core option "SCSP DSP" */
      {
         /* The env override exists for A/B runs only; say so loudly when it
            wins, otherwise an option seems to be ignored. */
         const char *tp = getenv("YAB_TEXCACHE_PERSIST");
         g_texcache_persist = (tp != NULL) ? atoi(tp) : opt_texcache_persist;
         if (tp != NULL)
            LOG_WARN("option: texture cache persist = %d OVERRIDDEN by "
                     "YAB_TEXCACHE_PERSIST=%s (option wanted %d)",
                     g_texcache_persist, tp, opt_texcache_persist);
      }
      if (YabauseInit(&yinit) != 0) { LOG_WARN("YabauseInit failed"); return; }
      set_memory_maps();   /* HighWram/LowWram exist only after YabauseInit() */
      if (stv_mode) {
         /* The front end may already have asked for av_info before this
            (retro_get_system_av_info has the same report); send it again now
            that the game list flag is live, so both orders end up rotated. */
         unsigned rotation = (unsigned)yabsys.isRotated;
         if (environ_cb != NULL)
            environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);
         LOG_INFO("ST-V: rotation=%u (title flag %d)", rotation, stv_rotated);
      }
      /* Options that are plain engine state rather than init-time fields: set
         them once the engine exists (SCSP/SMPC objects are alive now). */
      g_interlace_half_menu = opt_interlace_half;
      ScspSetCddaVolume(opt_cd_volume);
      ScspSetSfxVolume(opt_sfx_volume);
      SmpcSetSystemLanguage(opt_system_language);
      yab_options_seed();
      LOG_INFO("option: initial state dsp=%d cd=%d sfx=%d language=%d interlace=%d texcache=%d",
               opt_scsp_dsp, opt_cd_volume, opt_sfx_volume, opt_system_language,
               opt_interlace_half, g_texcache_persist);
      /* Audio: the SCSP thread produces at its own wall-clock rate.  With env 22
         the front end's audio thread pulls 512-frame periods from the ring and
         submits them from inside its own callback; without it, the consumer
         thread of sndlibretro.c does the same on a wall-clock schedule.  Either
         way nothing here runs on the front end's render thread. */
      if (audio_async_ok) {
         audio_async_on = 1;
         LOG_INFO("audio: async mode (front end pulls through env 22)");
      } else {
         SNDLibretroStart(libretro_audio_submit);
      }
      /* Per-line CPU sync, exactly as the standalone does it (main.cpp, right after
         YabauseInit, which resets the mode): request it here, and vdp2's FPSDisplay
         switches to DecilineMode 0 three seconds after the game starts drawing.
         YAB_PERLINE=0 keeps the historical core path (deciline throughout). */
      {
         const char *pl = getenv("YAB_PERLINE");
         if (pl != NULL && pl[0] == '0') {
            LOG_INFO("per-line: disabled by YAB_PERLINE=0 (deciline throughout)");
         } else {
            g_perline_requested = 1;
            YabauseSetDecilineMode(1);
            LOG_INFO("per-line: requested (standalone parity; switches after 3 s of VDP1 draws)");
         }
      }
   {
      /* Autoframeskip is available but OFF by default.  The decision and its 60/50 Hz schedule
         are the engine's own (frameSkipAndLimit), exactly as in the standalone; the frame limit
         itself is fixed at mode 0 by YabauseInit() (yinit.framelimit = 0), so nothing here
         touches VDP2SetFrameLimit() -- see the option handler above for why. */
      {
         const char *fs = getenv("YAB_FRAMESKIP");
         if ((fs != NULL && fs[0] == '1') || opt_frameskip) {
            EnableAutoFrameSkip();
            if (fs != NULL && fs[0] == '1' && !opt_frameskip)
               LOG_WARN("option: autoframeskip OFF but forced by YAB_FRAMESKIP=1");
            LOG_INFO("engine frame limit: 60 Hz schedule, autoframeskip ON (option or YAB_FRAMESKIP=1)");
         } else {
            /* The limiter itself stays on (mode 0 = the schedule with its sleep/busy-wait),
               only the skip decision is disabled -- the standalone's `-nf` path. */
            DisableAutoFrameSkip();
            LOG_INFO("engine frame limit: 60 Hz schedule, frameskip off (standalone -nf path)");
         }
      }
      Vdp2SetEventPostedHook(on_event_posted);
   }
   /* Render target: the FBO the frontend handed us.  ygles.c sets
      _Ygl->default_fbo = 0 when it initialises, which in a libretro hw core is
      not the surface the frontend presents. */
   {
      extern void *LibretroFrontFbo(void);
      _Ygl->default_fbo = (GLuint)(uintptr_t)LibretroFrontFbo();
      LOG_INFO("engine render target: default_fbo=%u", _Ygl->default_fbo);
   }
   reset_gl_state_for_frontend("after-init");
   }
   engine_up = 1;
   machine_run = 1;
   if (pthread_create(&machine_thread, NULL, machine_main, NULL) == 0)
      machine_started = 1;
   else
      LOG_WARN("machine thread failed");
}

void LibretroEngineContextDestroy(void)
{
   if (!engine_up) return;
   if (machine_started) {
      /* The machine can be parked in a join or in a full event queue whose only
         consumer is this thread: release both before joining, otherwise the
         quit from the frontend's menu hangs in pthread_join. */
      Vdp2AbortJoins(1);
      machine_run = 0;
      {
         int spins = 0;
         while (!machine_done && spins++ < 5000) {
            Vdp2DrainQueueNoRender();
            usleep(1000);
         }
      }
      pthread_join(machine_thread, NULL);
      machine_started = 0;
      machine_done = 1;
      Vdp2AbortJoins(0);
   }
   /* Stop the async path first: the front end may call the audio callback from
      its audio thread at any time, and that thread must not outlive the ring. */
   audio_async_on = 0;
   if (audio_async_ok && environ_cb != NULL) {
      struct retro_audio_callback none;
      memset(&none, 0, sizeof(none));
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &none);
      audio_async_ok = 0;
   }
   SNDLibretroStop();
   if (stv_mode) {
      /* STVDeInit() is the only path that writes <save>/stv/<game>.nv
         (eeprom_deinit() -> T123Save()).  It must run before YabauseDeInit()
         frees the cartridge whose board owns the EEPROM, and only when the
         option asks for it: the EEPROM is machine NVRAM, not game data. */
      LOG_INFO("engine: STVDeInit (EEPROM %s, persist=%s)", stv_eeprom_dir,
               stv_eeprom_persist ? "read+write" : "off");
      STVDeInit();
   }
   LOG_INFO("engine: YabauseDeInit");
   YabauseDeInit();
   engine_up = 0;
   state_size = 0;
   free(cmd_save_buf);
   cmd_save_buf = NULL;
   cmd_save_size = 0;
   cmd_type = YAB_CMD_NONE;
   cmd_done = 1;
}

static void core_log_sink(int level, const char *line)
{
   if (frontend_log != NULL) {
      /* The log layer already decided this line passes LOG_LEVEL, and the text
         carries its own severity ("[debug] ...").  Forward debug lines as INFO
         because front ends may drop the DEBUG level outright: minarch's
         LOG_note() prints LOG_DEBUG only in a DEBUG build of minarch itself, so
         a release minarch would swallow every debug line even though
         LOG_LEVEL=debug says we want them. */
      if ((int)level == (int)RETRO_LOG_DEBUG)
         level = (int)RETRO_LOG_INFO;
      frontend_log((enum retro_log_level)level, "%s", line);
   }
}

static void render_test_card(void)
{
   static const uint16_t colors[8] = {
       0xFFFF, /* white   */
       0xFFE0, /* yellow  */
       0x07FF, /* cyan    */
       0x07E0, /* green   */
       0xF81F, /* magenta */
       0xF800, /* red     */
       0x001F, /* blue    */
       0x0000  /* black   */
   };
   const unsigned marker_w = 40;
   const unsigned marker_x = (frame_count * 4) % (TEST_W - marker_w);
   const unsigned marker_y = TEST_H * 3 / 4;
   unsigned x, y;

   for (y = 0; y < TEST_H; y++)
   {
      uint16_t *row = frame_buf + (size_t)y * TEST_W;

      for (x = 0; x < TEST_W; x++)
      {
         uint16_t c = colors[(x * 8) / TEST_W];

         if (y >= marker_y && y < marker_y + 16 && x >= marker_x && x < marker_x + marker_w)
            c = 0xFFFF;
         row[x] = c;
      }
   }
}

static struct retro_hw_render_callback hw;


/* Crash evidence: a segfault here otherwise leaves nothing but a dead process.
   The backtrace goes to stderr, which the frontend redirects into its log. */
static void crash_handler(int sig)
{
   void *bt[32];
   int n = backtrace(bt, 32);
   const char *m = "\n[YAB-CRASH] signal caught, backtrace:\n";
   if (write(2, m, strlen(m)) < 0) { /* best effort */ }
   backtrace_symbols_fd(bt, n, 2);
   _exit(128 + sig);
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   environ_cb = cb;

   {
      static int installed;
      if (!installed) {
         struct sigaction sa;
         installed = 1;
         memset(&sa, 0, sizeof(sa));
         sa.sa_handler = crash_handler;
         sa.sa_flags = SA_RESETHAND;
         sigaction(SIGSEGV, &sa, NULL);
         sigaction(SIGABRT, &sa, NULL);
         sigaction(SIGBUS, &sa, NULL);
      }
   }

   memset(&hw, 0, sizeof(hw));
   hw.context_type       = RETRO_HW_CONTEXT_OPENGLES3;
   hw.version_major      = 3;
   hw.version_minor      = 0;
   hw.depth              = true;
   hw.stencil            = true;
   hw.bottom_left_origin = true;
   hw.context_reset      = LibretroEngineContextReset;
   hw.context_destroy    = LibretroEngineContextDestroy;

   /* Only valid from retro_set_environment() or retro_init(). */
   if (cb != NULL && cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) && log.log != NULL)
   {
      frontend_log = log.log;
      LogSetSink(core_log_sink);
   }

   {
      const char *save_dir = NULL;
      bool have_save = cb != NULL && cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
      if (have_save && save_dir != NULL)
         snprintf(libretro_save_dir, sizeof(libretro_save_dir), "%s", save_dir);
      /* Same layout as the reference line: the Saturn card is
         <save>/yabasanshiro/backup.bin and the ST-V EEPROM/card live in
         <save>/stv/.  Both directories are created here because the engine
         just fopen()s those paths; without the directory every write fails
         silently and the card/EEPROM contents are lost on exit. */
      if (have_save && save_dir != NULL) {
         char dir[512];
         snprintf(dir, sizeof(dir), "%s/yabasanshiro", save_dir);
         (void)mkdir(dir, 0777);
         snprintf(dir, sizeof(dir), "%s/stv", save_dir);
         (void)mkdir(dir, 0777);
         snprintf(libretro_bup_path, sizeof(libretro_bup_path),
                  "%s/yabasanshiro/backup.bin", save_dir);
      } else {
         snprintf(libretro_bup_path, sizeof(libretro_bup_path), "yabasanshiro.bup");
      }
      LOG_INFO("bup path (env=%d dir=%s): %s", (int)have_save,
               save_dir ? save_dir : "(null)", libretro_bup_path);

      {
         const char *sys_dir = NULL;
         static const char *names[] = { "saturn_bios.bin", "sega_101.bin",
                                        "mpr-17933.bin", "mpr-18811-mx.ic1", NULL };
         int i;
         if (cb != NULL && cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
            snprintf(libretro_system_dir, sizeof(libretro_system_dir), "%s", sys_dir);
            for (i = 0; names[i] != NULL; i++) {
               snprintf(libretro_bios_path, sizeof(libretro_bios_path), "%s/%s", sys_dir, names[i]);
               if (access(libretro_bios_path, R_OK) == 0) break;
               libretro_bios_path[0] = '\0';
            }
            LOG_INFO("bios: dir=%s -> %s", sys_dir,
                     libretro_bios_path[0] ? libretro_bios_path : "(not found, HLE)");
         }
      }
   }

   if (cb != NULL && !cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw))
      LOG_WARN("front end refused SET_HW_RENDER(GLES3)");

   if (cb != NULL && !cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      LOG_WARN("front end rejected RGB565; it keeps its default pixel format");

   /* Core options: the protocol asks for registration as early as possible
      (retro_set_environment).  The table is v2, with the fallback chain down to
      SET_VARIABLES for old front ends. */
   {
      bool cats = false;
      libretro_set_core_options(cb, &cats);
      LOG_INFO("options: registered (categories %s)", cats ? "supported" : "unsupported");
   }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_sample_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

/* ---------------------------------------------------------------------------
 * Libretro peripheral core: the Saturn pad side of the libretro input API.
 *
 * PERDummy never adds a pad, so PORTDATA stays empty and the SMPC hands the
 * guest a port status that is neither F0 (not connected) nor F1 (directly
 * connected).  The standalone builds this from its SDL layer, but this core
 * must not depend on one frontend, so the state comes from the protocol:
 *
 *   retro_set_controller_port_device()      -> pad_type[port]
 *   RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS   -> how many ports to expose
 *   RETRO_ENVIRONMENT_GET_INPUT_BITMASKS    -> MASK query or per-button loop
 *   RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS -> 13 Saturn buttons, all ports
 *
 * Input is polled exactly once per retro_run (frontend thread, as the libretro
 * contract requires) and published as a per-port snapshot; the machine thread
 * only reads that snapshot and applies it to the engine.  No libretro callback
 * runs off the retro_run thread, and the engine's peripheral state is only
 * touched by the machine thread at the top of its frame, so a controller change
 * cannot race the SMPC's INTBACK.
 * ------------------------------------------------------------------------- */
#define YAB_MAX_PORTS 12
static int pad_type[YAB_MAX_PORTS];          /* what the machine applies */
static int pad_type_pending[YAB_MAX_PORTS];  /* what the frontend asked for */
static volatile int pad_cfg_dirty = 1;
static unsigned players = 2;
static int libretro_supports_bitmasks = 0;

typedef struct {
   u16 joypad;           /* RETRO_DEVICE_ID_JOYPAD_* bits */
   int analog[4];
   int trig_l2, trig_r2;
} yab_input_port_t;
static yab_input_port_t yab_input_buf[2][YAB_MAX_PORTS];
static volatile int yab_input_cur = -1;   /* index of the last COMPLETE snapshot */
static int yab_in_run = 0;                /* set while retro_run is on the stack */

static const struct { unsigned id; u8 key; } kPadKeys[] = {
   { RETRO_DEVICE_ID_JOYPAD_UP,    PERPAD_UP },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,  PERPAD_DOWN },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,  PERPAD_LEFT },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, PERPAD_RIGHT },
   { RETRO_DEVICE_ID_JOYPAD_Y,     PERPAD_X },
   { RETRO_DEVICE_ID_JOYPAD_B,     PERPAD_A },
   { RETRO_DEVICE_ID_JOYPAD_A,     PERPAD_B },
   { RETRO_DEVICE_ID_JOYPAD_X,     PERPAD_Y },
   { RETRO_DEVICE_ID_JOYPAD_L,     PERPAD_LEFT_TRIGGER },
   { RETRO_DEVICE_ID_JOYPAD_R,     PERPAD_RIGHT_TRIGGER },
   { RETRO_DEVICE_ID_JOYPAD_START, PERPAD_START },
   { RETRO_DEVICE_ID_JOYPAD_L2,    PERPAD_Z },
   { RETRO_DEVICE_ID_JOYPAD_R2,    PERPAD_C },
};
#define PADKEY_NB ((unsigned)(sizeof(kPadKeys) / sizeof(kPadKeys[0])))

/* ST-V cabinet (JAMMA/IOGA).  Port 0 is the player-1 panel, port 1 the
   player-2 panel; Test/Service/Pause are cabinet keys and only exist when the
   "Service Mode (ST-V)" option is on.  Same mapping as the reference ST-V
   port, so an existing front-end profile keeps working. */
static const struct { unsigned id; u8 key; unsigned player; const char *name; }
kStvSystemKeys[] = {
   { RETRO_DEVICE_ID_JOYPAD_L2, PERJAMMA_TEST,    0, "Test"    },
   { RETRO_DEVICE_ID_JOYPAD_R2, PERJAMMA_SERVICE, 0, "Service" },
   { RETRO_DEVICE_ID_JOYPAD_L3, PERJAMMA_PAUSE,   0, "Pause"   },
};
#define STV_SYSKEY_NB ((unsigned)(sizeof(kStvSystemKeys) / sizeof(kStvSystemKeys[0])))

static const struct { unsigned id; u8 key; unsigned player; const char *name; }
kStvGameKeys[] = {
   { RETRO_DEVICE_ID_JOYPAD_SELECT, PERJAMMA_COIN1,      0, "Coin"    },
   { RETRO_DEVICE_ID_JOYPAD_START,  PERJAMMA_START1,     0, "Start"   },
   { RETRO_DEVICE_ID_JOYPAD_UP,     PERPAD_UP,           0, "Up"      },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT,  PERPAD_RIGHT,        0, "Right"   },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,   PERPAD_DOWN,         0, "Down"    },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,   PERPAD_LEFT,         0, "Left"    },
   { RETRO_DEVICE_ID_JOYPAD_B,      PERPAD_A,            0, "Button1" },
   { RETRO_DEVICE_ID_JOYPAD_A,      PERPAD_B,            0, "Button2" },
   { RETRO_DEVICE_ID_JOYPAD_Y,      PERPAD_C,            0, "Button3" },
   { RETRO_DEVICE_ID_JOYPAD_X,      PERPAD_X,            0, "Button4" },
   { RETRO_DEVICE_ID_JOYPAD_SELECT, PERJAMMA_COIN2,      1, "Coin"    },
   { RETRO_DEVICE_ID_JOYPAD_START,  PERJAMMA_START2,     1, "Start"   },
   { RETRO_DEVICE_ID_JOYPAD_UP,     PERJAMMA_P2_UP,      1, "Up"      },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT,  PERJAMMA_P2_RIGHT,   1, "Right"   },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,   PERJAMMA_P2_DOWN,    1, "Down"    },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,   PERJAMMA_P2_LEFT,    1, "Left"    },
   { RETRO_DEVICE_ID_JOYPAD_B,      PERJAMMA_P2_BUTTON1, 1, "Button1" },
   { RETRO_DEVICE_ID_JOYPAD_A,      PERJAMMA_P2_BUTTON2, 1, "Button2" },
   { RETRO_DEVICE_ID_JOYPAD_Y,      PERJAMMA_P2_BUTTON3, 1, "Button3" },
   { RETRO_DEVICE_ID_JOYPAD_X,      PERJAMMA_P2_BUTTON4, 1, "Button4" },
};
#define STV_GAMEKEY_NB ((unsigned)(sizeof(kStvGameKeys) / sizeof(kStvGameKeys[0])))

/* Frontend thread, at load time: negotiate the protocol, reset the port map. */
static void yab_input_ports_init(void)
{
   unsigned i;
   unsigned max_users = 0;
   bool bitmasks = false;

   for (i = 0; i < YAB_MAX_PORTS; i++) {
      pad_type[i] = RETRO_DEVICE_JOYPAD;
      pad_type_pending[i] = RETRO_DEVICE_JOYPAD;
   }
   if (environ_cb != NULL &&
       environ_cb(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS, &max_users) && max_users > 0)
      players = (max_users > YAB_MAX_PORTS) ? YAB_MAX_PORTS : max_users;
   else
      players = 2;   /* not reported: assume the engine's two ports */
   if (environ_cb != NULL && environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &bitmasks))
      libretro_supports_bitmasks = bitmasks ? 1 : 0;
   pad_cfg_dirty = 1;
}

/* Frontend thread, at load time: descriptors so any frontend can map the pad. */
static void yab_input_descriptors(void)
{
   static const struct { unsigned id; const char *name; } d[] = {
      { RETRO_DEVICE_ID_JOYPAD_UP,    "Up"    },
      { RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down"  },
      { RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left"  },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { RETRO_DEVICE_ID_JOYPAD_Y,     "X"     },
      { RETRO_DEVICE_ID_JOYPAD_B,     "A"     },
      { RETRO_DEVICE_ID_JOYPAD_A,     "B"     },
      { RETRO_DEVICE_ID_JOYPAD_X,     "Y"     },
      { RETRO_DEVICE_ID_JOYPAD_L,     "L"     },
      { RETRO_DEVICE_ID_JOYPAD_R,     "R"     },
      { RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { RETRO_DEVICE_ID_JOYPAD_L2,    "Z"     },
      { RETRO_DEVICE_ID_JOYPAD_R2,    "C"     },
   };
   static struct retro_input_descriptor desc[2 * (sizeof(d) / sizeof(d[0])) + 1];
   const unsigned nb = (unsigned)(sizeof(d) / sizeof(d[0]));
   unsigned p, i;

   if (stv_mode) {
      /* The cabinet table has no Saturn-only ids (and JOYPAD_SELECT, which
         minarch needs to see for the coin key). */
      static struct retro_input_descriptor sdesc[STV_SYSKEY_NB + STV_GAMEKEY_NB + 1];
      unsigned n = 0;
      memset(sdesc, 0, sizeof(sdesc));
      if (opt_stv_service)
         for (i = 0; i < STV_SYSKEY_NB; i++, n++) {
            sdesc[n].port        = kStvSystemKeys[i].player;
            sdesc[n].device      = RETRO_DEVICE_JOYPAD;
            sdesc[n].id          = kStvSystemKeys[i].id;
            sdesc[n].description = kStvSystemKeys[i].name;
         }
      for (i = 0; i < STV_GAMEKEY_NB; i++, n++) {
         sdesc[n].port        = kStvGameKeys[i].player;
         sdesc[n].device      = RETRO_DEVICE_JOYPAD;
         sdesc[n].id          = kStvGameKeys[i].id;
         sdesc[n].description = kStvGameKeys[i].name;
      }
      if (environ_cb != NULL)
         environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, sdesc);
      return;
   }

   memset(desc, 0, sizeof(desc));
   for (p = 0; p < 2; p++)
      for (i = 0; i < nb; i++) {
         desc[p * nb + i].port = p;
         desc[p * nb + i].device = RETRO_DEVICE_JOYPAD;
         desc[p * nb + i].index = 0;
         desc[p * nb + i].id = d[i].id;
         desc[p * nb + i].description = d[i].name;
      }
   if (environ_cb != NULL)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

/* Frontend thread: read the callback once per frame and publish the snapshot.
   The poll itself happens in retro_run, before this. */
void yab_input_publish(void)
{
   int cur, next;
   unsigned i, id;

   if (!yab_in_run)
      return;
   cur = yab_input_cur;
   next = (cur == 0) ? 1 : 0;
   for (i = 0; i < players && i < YAB_MAX_PORTS; i++) {
      yab_input_port_t *p = &yab_input_buf[next][i];
      p->joypad = 0;
      p->analog[0] = p->analog[1] = p->analog[2] = p->analog[3] = 0;
      p->trig_l2 = p->trig_r2 = 0;
      if (input_state_cb == NULL)
         continue;
      if (libretro_supports_bitmasks) {
         p->joypad = (u16)input_state_cb(i, RETRO_DEVICE_JOYPAD, 0,
                                         RETRO_DEVICE_ID_JOYPAD_MASK);
      } else {
         for (id = 0; id < 16; id++)
            if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, id))
               p->joypad |= (u16)(1u << id);
      }
      if (pad_type_pending[i] == RETRO_DEVICE_ANALOG) {
         int l2, r2;
         p->analog[0] = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
         p->analog[1] = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
         p->analog[2] = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
         p->analog[3] = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
         l2 = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
         r2 = input_state_cb(i, RETRO_DEVICE_ANALOG,
               RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);
         if (l2 == 0) l2 = (p->joypad & (1u << RETRO_DEVICE_ID_JOYPAD_L2)) ? 0x7FFF : 0;
         if (r2 == 0) r2 = (p->joypad & (1u << RETRO_DEVICE_ID_JOYPAD_R2)) ? 0x7FFF : 0;
         p->trig_l2 = l2;
         p->trig_r2 = r2;
      }
   }
   __atomic_store_n(&yab_input_cur, next, __ATOMIC_RELEASE);
}

/* Machine thread: read the last complete snapshot, never a libretro callback. */
static int input_state_cb_wrapper(unsigned port, unsigned device, unsigned index, unsigned id)
{
   const yab_input_port_t *p;
   int cur = __atomic_load_n(&yab_input_cur, __ATOMIC_ACQUIRE);

   if (cur < 0 || port >= YAB_MAX_PORTS)
      return 0;
   p = &yab_input_buf[cur][port];
   if (device == RETRO_DEVICE_JOYPAD)
      return (id < 16) ? ((p->joypad >> id) & 1) : 0;
   if (device == RETRO_DEVICE_ANALOG) {
      if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
         if (id == RETRO_DEVICE_ID_ANALOG_X) return p->analog[0];
         if (id == RETRO_DEVICE_ID_ANALOG_Y) return p->analog[1];
      } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
         if (id == RETRO_DEVICE_ID_ANALOG_X) return p->analog[2];
         if (id == RETRO_DEVICE_ID_ANALOG_Y) return p->analog[3];
      } else if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
         if (id == RETRO_DEVICE_ID_JOYPAD_L2) return p->trig_l2;
         if (id == RETRO_DEVICE_ID_JOYPAD_R2) return p->trig_r2;
      }
   }
   return 0;
}

/* Machine thread: rebuild the engine's port layout from pad_type_pending. */
static void PERLIBRETROApplyConfig(void)
{
   unsigned i, j;
   PortData_struct *portdata;
   void *controller;

   if (stv_mode) {
      /* One JAMMA cabinet controller; the keys are the cabinet's own ids, so
         PerSetKey(key, key, cab) wires each one to its Press/Release handler. */
      void *cab;
      PerPortReset();
      cab = (void *)PerCabAdd(NULL);
      if (opt_stv_service)
         for (i = 0; i < STV_SYSKEY_NB; i++)
            PerSetKey(kStvSystemKeys[i].key, kStvSystemKeys[i].key, cab);
      for (i = 0; i < STV_GAMEKEY_NB; i++)
         PerSetKey(kStvGameKeys[i].key, kStvGameKeys[i].key, cab);
      players = 2;
      pad_cfg_dirty = 0;
      return;
   }

   for (i = 0; i < YAB_MAX_PORTS; i++)
      pad_type[i] = pad_type_pending[i];
   PerPortReset();
   for (i = 0; i < players && i < YAB_MAX_PORTS; i++) {
      /* Two physical ports; more than two peripherals need the multitap slot.
         Same split as the standalone's key map setup. */
      portdata = ((players > 2 && i < 6) || i == 0) ? &PORTDATA1 : &PORTDATA2;
      switch (pad_type[i]) {
         case RETRO_DEVICE_NONE:
            continue;
         case RETRO_DEVICE_ANALOG:
            controller = (void *)Per3DPadAdd(portdata);
            for (j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, (u8)j, controller);
            for (j = PERANALOG_AXIS1; j <= PERANALOG_AXIS7; j++)
               PerSetKey((i << 8) + j, (u8)j, controller);
            break;
         default:
            controller = (void *)PerPadAdd(portdata);
            for (j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, (u8)j, controller);
            break;
      }
   }
   pad_cfg_dirty = 0;
}

static int PERLIBRETROInit(void)
{
   pad_cfg_dirty = 1;   /* the machine thread builds the pads on its first frame */
   return 0;
}

static int PERLIBRETROHandleEvents(void)
{
   unsigned i, k;

   if (pad_cfg_dirty)
      PERLIBRETROApplyConfig();
   if (stv_mode) {
      /* Cabinet inputs only: the Saturn pad loop below would push PERPAD_*
         through the shared key table and clear what this loop set. */
      if (opt_stv_service)
         for (i = 0; i < STV_SYSKEY_NB; i++) {
            if (input_state_cb_wrapper(kStvSystemKeys[i].player,
                                       RETRO_DEVICE_JOYPAD, 0, kStvSystemKeys[i].id))
               PerKeyDown(kStvSystemKeys[i].key);
            else
               PerKeyUp(kStvSystemKeys[i].key);
         }
      for (i = 0; i < STV_GAMEKEY_NB; i++) {
         if (input_state_cb_wrapper(kStvGameKeys[i].player,
                                    RETRO_DEVICE_JOYPAD, 0, kStvGameKeys[i].id))
            PerKeyDown(kStvGameKeys[i].key);
         else
            PerKeyUp(kStvGameKeys[i].key);
      }
      if (YabauseExec() != 0)
         return -1;
      return 0;
   }
   for (i = 0; i < players && i < YAB_MAX_PORTS; i++) {
      if (pad_type[i] == RETRO_DEVICE_NONE)
         continue;
      for (k = 0; k < PADKEY_NB; k++) {
         if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, kPadKeys[k].id))
            PerKeyDown((i << 8) + kPadKeys[k].key);
         else
            PerKeyUp((i << 8) + kPadKeys[k].key);
      }
      if (pad_type[i] == RETRO_DEVICE_ANALOG) {
         int lx = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
         int ly = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
         int rx = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
         int ry = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
         PerAxisValue((i << 8) + PERANALOG_AXIS1, (u8)((lx + 0x8000) >> 8));
         PerAxisValue((i << 8) + PERANALOG_AXIS2, (u8)((ly + 0x8000) >> 8));
         PerAxisValue((i << 8) + PERANALOG_AXIS3, (u8)((rx + 0x8000) >> 8));
         PerAxisValue((i << 8) + PERANALOG_AXIS4, (u8)((ry + 0x8000) >> 8));
      }
   }
   if (YabauseExec() != 0)
      return -1;
   return 0;
}

void PERLIBRETRODeInit(void) {}
void PERLIBRETRONothing(void) {}
u32  PERLIBRETROScan(u32 flags) { (void)flags; return 0; }
void PERLIBRETROKeyName(u32 key, char *name, int size) { (void)key; if (size > 0) name[0] = '\0'; }

PerInterface_struct PERLIBRETROJoy = {
   PERCORE_LIBRETRO,
   "Libretro Input Interface",
   PERLIBRETROInit,
   PERLIBRETRODeInit,
   PERLIBRETROHandleEvents,
   PERLIBRETROScan,
   0,
   PERLIBRETRONothing,
   PERLIBRETROKeyName
};

RETRO_API void retro_init(void)
{
   frame_count = 0;
   frame_buf = (uint16_t*)calloc((size_t)TEST_W * TEST_H, sizeof(uint16_t));
   audio_buf = (int16_t*)calloc((size_t)AUDIO_FRAMES * 2, sizeof(int16_t));

   LOG_INFO("placeholder core init: %dx%d RGB565, %d Hz stereo, %.2f fps",
            TEST_W, TEST_H, 44100, 60.0);
}

RETRO_API void retro_deinit(void)
{
   LOG_INFO("placeholder core deinit after %u frames", frame_count);
   LibretroEngineContextDestroy();
   yab_in_run = 0;
   yab_input_cur = -1;
   pad_cfg_dirty = 1;
   free(frame_buf);
   free(audio_buf);
   frame_buf = NULL;
   audio_buf = NULL;
}

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "YabaSanshiro";
   info->library_version = CORE_VERSION;
   /* zip: an ST-V game is a MAME romset and stv.c matches its entries by name
      and CRC32, so the archive must reach the core untouched. */
   info->valid_extensions = "cue|iso|chd|m3u|zip";
   /* The engine opens the image itself (yinit.cdpath), and a Saturn image is
      300-600 MB: with `false` the front end reads the whole file into RAM before
      retro_load_game and hands over a buffer this core never looks at.  Measured
      on the 421 MB Castlevania ISO: RSS 44 -> 434 MB while the front end read it,
      first emulated second at 24 s, peak RSS 607 MB.  `true` matches the earlier
      h700-libretro core (need_fullpath = true). */
   info->need_fullpath = true;
   /* true: the front end hands the archive over as-is.  With `false` it would
      extract a .zip first, and an ST-V romset is exactly the case that must not
      be unpacked (the engine has no zip reader for CD images either). */
   info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width = (unsigned)(frame_width > 0 ? frame_width : game_width);
   info->geometry.base_height = (unsigned)(frame_height > 0 ? frame_height : game_height);
   /* The front end allocates its hw-render FBO from these; understating them
      (the old 320x240) left no room for the hi-res modes. */
   info->geometry.max_width = GEOMETRY_MAX_W;
   info->geometry.max_height = GEOMETRY_MAX_H;
   /* Saturn pixels are not square: every TV mode (320/352/704 wide) is 4:3. */
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps = (opt_videoformat_latched == VIDEOFORMATTYPE_PAL) ? 50.0 : 60.0;
   info->timing.sample_rate = 44100.0;

   /* Vertical ST-V boards render sideways and the front end is asked to turn
      the picture.  The command is sent unconditionally (0 included): a front
      end keeps the last value it was handed (minarch's ma_gl_rotation is a
      plain static), so silence would leave the previous title's rotation on.
      The aspect ratio is inverted only while rotated, so the front end's own
      width/height swap lands on the right box.  The latch from retro_load_game
      covers the front ends that ask for av_info before the engine is built;
      yabsys.isRotated (set by YabauseInit) is the authoritative value later. */
   {
      unsigned rotation = 0;
      if (stv_mode)
         rotation = (unsigned)(yabsys.isRotated != 0 || stv_rotated);
      if (environ_cb != NULL)
         environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);
      if (rotation != 0)
         info->geometry.aspect_ratio = 1.0f / info->geometry.aspect_ratio;
   }
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   /* Frontend thread.  Only record the request; the machine thread applies it
      at the top of its next frame (the engine's peripheral tables belong to
      that thread).  Unsupported device types fall back to the RetroPad. */
   if (port >= YAB_MAX_PORTS)
      return;
   if (device != RETRO_DEVICE_NONE && device != RETRO_DEVICE_JOYPAD &&
       device != RETRO_DEVICE_ANALOG)
      device = RETRO_DEVICE_JOYPAD;
   pad_type_pending[port] = (int)device;
   pad_cfg_dirty = 1;
}

RETRO_API void retro_reset(void)
{
   /* The engine reset runs on the machine thread at its field boundary. */
   if (engine_up)
      (void)yab_cmd_wait(YAB_CMD_RESET, NULL, 0);
}

RETRO_API void retro_run(void)
{
   /* libretro contract: poll at least once per retro_run, on this thread
      (libretro.h:9358).  A call can cover more than one field -- a skipped field
      presents nothing, so the call continues into the next one -- and the
      standalone samples the pad once per field, so the poll sits at the top of
      the field loop below (and once here for the no-engine path).
      SNDLibretroHeartbeat() is the only pause signal libretro offers us. */
   SNDLibretroHeartbeat();
   yab_in_run = 1;

   if (engine_up) {
      Vdp2SetRenderThreadHere();   /* this thread renders now, not VdpProc */
      /* Core options: the front end flags a change, re-read and apply it here
         (render thread, GL context current, before this field's events). */
      {
         bool updated = false;
         if (environ_cb != NULL &&
             environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
            yab_options_read();
            yab_options_apply();
         }
      }
      /* Saturn mode changes are applied before anything of this field is drawn.
         The engine's Resize()/YglGLInit() rebuilds GL objects, so it has to stay
         outside the render stack (a change is then visible one field late, which
         is what the standalone's fixed window hides anyway). */
      core_apply_resolution();
      /* One call = one video frame (libretro.h:9357).  A field that autoframeskip drops
         presents nothing -- the standalone does not swap on a skipped field either -- so the
         call keeps running fields until one is actually drawn, and only that field's
         present ends the call.  Inside a field the order is the standalone's:
         Vdp2DrawEnd (the engine renders into the front end's FBO) -> video_cb (the swap,
         which is where the standalone's YuiSwapBuffers sits) -> Vdp2FinishField (the
         limiter reads the clock only now, then sleeps, then Sync).  Presenting after the
         limiter instead would charge the swap's wait to the next field, on top of a sleep
         that was already spent.
         Draining the whole queue without a field boundary would pump the machine to 86-97
         fps, because it posts its VIN event before running the field. */
      unsigned ready0 = frame_ready;
      struct timespec t1, t2, w0, d0, d1;
      double draw_ms = 0.0, wait_ms = 0.0;
      for (;;) {
         const unsigned seq0 = Vdp2FieldSeq();
         int drew;
         /* Input once per field (standalone parity): the snapshot is published from the
            engine's VIN handler, before the machine runs this field's VBlank routine. */
         if (input_poll_cb != NULL)
            input_poll_cb();
         /* Drawing and waiting for the machine are measured apart: a heavy game keeps this
            thread waiting for VOUT most of the field, and folding that into one "render"
            number hides which side is the bottleneck.  The limiter's sleep is outside this
            number now that Vdp2FinishField() runs it after the present. */
         for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &d0);
            while (Vdp2ConsumeFrontendEvent()) {
               if (Vdp2FieldSeq() != seq0) break;
            }
            clock_gettime(CLOCK_MONOTONIC, &d1);
            draw_ms += (d1.tv_sec - d0.tv_sec) * 1e3 + (d1.tv_nsec - d0.tv_nsec) / 1e6;
            if (Vdp2FieldSeq() != seq0) break;
            clock_gettime(CLOCK_MONOTONIC, &w0);
            pthread_mutex_lock(&ev_mtx);
            if (ev_posted == ev_seen)
               pthread_cond_wait(&ev_cv, &ev_mtx);
            ev_seen = ev_posted;
            pthread_mutex_unlock(&ev_mtx);
            {
               struct timespec w1;
               clock_gettime(CLOCK_MONOTONIC, &w1);
               wait_ms += (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6;
            }
         }
         /* This field's present: frame_ready advances inside YuiSwapBuffers, i.e. only when
            VIDCore->Vdp2DrawEnd() really ran (a skipped field ran VIDDummy's no-op). */
         drew = (frame_ready != ready0);
         if (drew) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            frame_sent = frame_ready;
            video_cb(RETRO_HW_FRAME_BUFFER_VALID,
                     (unsigned)(frame_width > 0 ? frame_width : TEST_W),
                     (unsigned)(frame_height > 0 ? frame_height : TEST_H), 0);
            clock_gettime(CLOCK_MONOTONIC, &t2);
            acc_present_ms += (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
         }
         Vdp2FinishField();   /* limiter (clock + skip decision + sleep), Sync, VIN token */
         if (drew)
            break;
      }
      acc_draw_ms += draw_ms;
      acc_wait_ms += wait_ms;
      acc_calls++;
   } else {
      if (input_poll_cb != NULL)
         input_poll_cb();
      render_test_card();
   }

   if (!engine_up && audio_batch_cb != NULL)
      audio_batch_cb(audio_buf, AUDIO_FRAMES); /* silence */

   frame_count++;
   {
      static struct timespec last;
      static unsigned last_ready, last_run, last_drain, last_fields;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (last.tv_sec == 0) { last = now; last_ready = frame_ready; last_run = frame_count;
                              last_drain = drained_total; last_fields = Vdp2FieldSeq(); }
      else if (now.tv_sec != last.tv_sec) {
         double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
         {
                                                /* Same three numbers the standalone OSD prints (PERF-BASELINE.md):
               fps / skip / vdp1, sampled once per second. */
            extern int show_vdp1_frame;
            extern unsigned int show_skipped_frame;
            /* Same line, same fields, same sources as the standalone probe:
               PERFLOG fps=<rendered fields/s> skip=<show_skipped_frame> vdp1=<show_vdp1_frame> */
            /* fps = presented fields/s (the standalone OSD's number is the FIELD
               rate, so it also gets logged as fields=); skip = autoframeskip drops. */
            LOG_INFO("PERFLOG fps=%d fields=%d skip=%u vdp1=%d",
                     (int)((frame_ready - last_ready) / dt + 0.5),
                     (int)((Vdp2FieldSeq() - last_fields) / dt + 0.5),
                     show_skipped_frame, show_vdp1_frame);
            {
               double c = acc_calls ? (double)acc_calls : 1.0;
               double mc = acc_machine_n ? (double)acc_machine_n : 1.0;
               /* draw/wait/swap are per retro_run call; machine is per field.
                  swap = time inside video_cb, i.e. the front end's present
                  including its vsync wait (not core work). */
               LOG_INFO("segment: calls/s=%.0f draw_ms=%.2f wait_ms=%.2f machine_ms=%.2f (n=%u) swap_ms=%.2f",
                        acc_calls / dt, acc_draw_ms / c, acc_wait_ms / c, acc_machine_ms / mc,
                        acc_machine_n, acc_present_ms / c);
               acc_draw_ms = acc_present_ms = acc_wait_ms = acc_machine_ms = 0.0;
               acc_calls = acc_machine_n = 0;
            }
            {
               /* Audio, once per second: which path is live, what the submit
                  costs on the thread that owns it, and the ring balance.  In
                  async mode calls/frames come from the front end's audio
                  thread; in thread mode they come from the consumer thread and
                  only the frame counters move. */
               static u32 lp, lc, ld, lu;
               u32 p, c, d, u;
               unsigned fill_frames;
               SNDLibretroSnapshot(&p, &c, &d, &u, &fill_frames);
               static unsigned lpe, lre;
               unsigned pe = SNDLibretroPauseEvents(), re = SNDLibretroResumeEvents();
               unsigned wmax = aud_wmax, wmin = aud_wmin;
               if (wmin == 0xFFFFFFFFu) wmin = 0;
               LOG_INFO("audio: mode=%s paused=%d occ=%d dry=%d ratio=%.4f calls=%u frames=%u wmax=%u wmin=%u pump_us=%.2f/%.2f under=%u short=%u fill=%u prod=%u cons=%u drop=%u pause=%u resume=%u",
                        audio_async_on ? "async" : "thread", SNDLibretroPaused(),
                        __atomic_load_n(&aud_occ, __ATOMIC_RELAXED),
                        __atomic_load_n(&aud_underrun_likely, __ATOMIC_RELAXED),
                        SNDLibretroRatio(), aud_calls, c - lc, wmax, wmin,
                        aud_calls ? aud_us_sum / aud_calls : 0.0, aud_us_max,
                        u - lu, aud_short, fill_frames,
                        p - lp, c - lc, d - ld, pe - lpe, re - lre);
               lp = p; lc = c; ld = d; lu = u;
               lpe = pe; lre = re;
               aud_calls = aud_short = 0;
               aud_wmax = 0;
               aud_wmin = 0xFFFFFFFFu;
               aud_us_sum = aud_us_max = 0.0;
            }

         }
         last = now; last_ready = frame_ready; last_run = frame_count;
         last_drain = drained_total; last_fields = Vdp2FieldSeq();
      }
   }

   if (frame_count % FRAME_LOG_INTERVAL == 0)
      LOG_DEBUG("run=%u ready=%u sent=%u drained=%u none=%u vin=%u",
                frame_count, frame_ready, frame_sent, drained_total, drained_none, engine_up);
   yab_in_run = 0;
}

/* retro_serialize_size() must stay constant for the lifetime of a loaded game:
   probe once, then report that size (plus slack, so a later field cannot need
   more than the frontend allocated). */
RETRO_API size_t retro_serialize_size(void)
{
   if (!engine_up)
      return 0;
   if (state_size == 0) {
      if (yab_cmd_wait(YAB_CMD_SAVE, NULL, 0) && cmd_save_buf != NULL)
         state_size = cmd_save_size + (2u << 20);
      free(cmd_save_buf);
      cmd_save_buf = NULL;
      cmd_save_size = 0;
   }
   return state_size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   if (!engine_up || data == NULL)
      return false;
   if (state_size == 0)
      (void)retro_serialize_size();
   if (!yab_cmd_wait(YAB_CMD_SAVE, NULL, 0) || cmd_save_buf == NULL) {
      free(cmd_save_buf);
      cmd_save_buf = NULL;
      cmd_save_size = 0;
      return false;
   }
   if (cmd_save_size < 0x14 || cmd_save_size > size) {
      free(cmd_save_buf);
      cmd_save_buf = NULL;
      cmd_save_size = 0;
      return false;
   }
   memcpy(data, cmd_save_buf, cmd_save_size);
   if (size > cmd_save_size)
      memset((uint8_t *)data + cmd_save_size, 0, size - cmd_save_size);
   /* YabLoadStateStream() requires header.size == (bytes - 0x14); the frontend
      allocated exactly the size we reported, so rewrite that field for the
      padded buffer (the engine's own value is the unpadded payload size). */
   {
      uint32_t payload = (uint32_t)(size - 0x14);
      memcpy((uint8_t *)data + 8, &payload, sizeof(payload));
   }
   free(cmd_save_buf);
   cmd_save_buf = NULL;
   cmd_save_size = 0;
   return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   if (!engine_up || data == NULL || size == 0)
      return false;
   return yab_cmd_wait(YAB_CMD_LOAD, data, size) ? true : false;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

/* ------------------------------------------------------------------------- *
 * Disk control (libretro protocol): multi-disc games and .m3u playlists
 *
 * Ported from the h700-libretro branch, which had this working: a Saturn
 * multi-disc title ships as several images, the front end hands the playlist
 * (.m3u) to retro_load_game, and the core resolves it here, boots its first
 * entry and publishes the whole list through the disk-control interface.
 * Swapping is a real tray open/close on the emulated CD block, so it runs on
 * the machine thread through the same command channel as save/load/reset
 * (Cs2Exec lives there).
 *
 * Playlist entries are paths relative to the playlist's directory or absolute;
 * '#' comments and blank lines are skipped.  That is the layout in use:
 *   Roms/Sega Saturn (SATURN)/D之食卓.m3u
 *   Roms/Sega Saturn (SATURN)/D之食卓/D_CN3_CD1.chd
 * ------------------------------------------------------------------------- */
#define YAB_MAX_DISKS 16

static char     disk_paths[YAB_MAX_DISKS][YAB_PATH_MAX];
static char     disk_labels[YAB_MAX_DISKS][YAB_PATH_MAX];
static unsigned disk_count;
static unsigned disk_index;
static unsigned disk_initial_idx;
static char     disk_initial_path[YAB_PATH_MAX];
static int      disk_tray_open;

/* The image the engine boots: the playlist's first entry, or the single image.
   libretro_content_path points here, so a later YabauseInit opens it too. */
static char     content_path_buf[YAB_PATH_MAX];

static const char *disk_basename(const char *path)
{
   const char *slash = strrchr(path, '/');
   return (slash != NULL) ? slash + 1 : path;
}

static int disk_ext_is_m3u(const char *path)
{
   const char *ext = strrchr(disk_basename(path), '.');
   if (ext == NULL)
      return 0;
   ext++;
   return (ext[0] == 'm' || ext[0] == 'M') && ext[1] == '3' &&
          (ext[2] == 'u' || ext[2] == 'U') && ext[3] == '\0';
}

/* Directory part including the trailing slash ("" when there is none). */
static void disk_dirname(const char *path, char *out, size_t size)
{
   const char *slash = strrchr(path, '/');
   size_t n = (slash != NULL) ? (size_t)(slash - path) + 1 : 0;
   if (n >= size)
      n = size - 1;
   memcpy(out, path, n);
   out[n] = '\0';
}

/* "D_CN3_CD1.chd" -> "D_CN3_CD1": what a front end shows in its disc list. */
static void disk_set_label_from_path(unsigned idx)
{
   char base[YAB_PATH_MAX];
   char *dot;
   if (idx >= YAB_MAX_DISKS || disk_paths[idx][0] == '\0')
      return;
   snprintf(base, sizeof(base), "%s", disk_basename(disk_paths[idx]));
   dot = strrchr(base, '.');
   if (dot != NULL && dot != base)
      *dot = '\0';
   snprintf(disk_labels[idx], sizeof(disk_labels[idx]), "%s", base);
}

static bool disk_set_eject_state(bool ejected)
{
   if (!ejected && (disk_index >= disk_count || disk_paths[disk_index][0] == '\0'))
      return false;
   /* Before the engine exists there is no CD block to move: the front end's
      request is still the initial state, so recording it is all it needs. */
   if (engine_up && !yab_disc_cmd(ejected ? 1 : 0,
                                  ejected ? NULL : disk_paths[disk_index]))
      return false;
   if (!ejected)
      snprintf(content_path_buf, sizeof(content_path_buf), "%s",
               disk_paths[disk_index]);
   disk_tray_open = ejected ? 1 : 0;
   return true;
}

static bool     disk_get_eject_state(void) { return disk_tray_open ? true : false; }
static unsigned disk_get_image_index(void) { return disk_index; }
static unsigned disk_get_num_images(void)  { return disk_count; }

static bool disk_set_image_index(unsigned index)
{
   /* The media changes when the tray closes (disk_set_eject_state(false)); here
      only the image to insert is recorded.  index == disk_count is the front
      end's "no disc" sentinel. */
   if (index == disk_count) {
      disk_index = index;
      return true;
   }
   if (index >= disk_count)
      return false;
   disk_index = index;
   return true;
}

static bool disk_replace_image_index(unsigned index,
      const struct retro_game_info *info)
{
   if (index >= YAB_MAX_DISKS)
      return false;

   if (info == NULL || info->path == NULL) {   /* remove this image */
      unsigned i;
      if (index >= disk_count)
         return false;
      for (i = index; i + 1 < disk_count; i++)
         snprintf(disk_paths[i], YAB_PATH_MAX, "%s", disk_paths[i + 1]);
      if (disk_count > 0)
         disk_count--;
      if (disk_count > 0 && disk_index >= disk_count)
         disk_index = disk_count - 1;
      return true;
   }

   snprintf(disk_paths[index], YAB_PATH_MAX, "%s", info->path);
   disk_set_label_from_path(index);
   if (index >= disk_count)
      disk_count = index + 1;
   return true;
}

static bool disk_add_image_index(void)
{
   if (disk_count >= YAB_MAX_DISKS)
      return false;
   disk_paths[disk_count][0]  = '\0';
   disk_labels[disk_count][0] = '\0';
   disk_count++;
   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
   disk_initial_idx = index;
   if (path != NULL)
      snprintf(disk_initial_path, sizeof(disk_initial_path), "%s", path);
   else
      disk_initial_path[0] = '\0';
   return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
   if (index >= disk_count || path == NULL || len == 0 || disk_paths[index][0] == '\0')
      return false;
   snprintf(path, len, "%s", disk_paths[index]);
   return true;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
   if (index >= disk_count || label == NULL || len == 0 || disk_labels[index][0] == '\0')
      return false;
   snprintf(label, len, "%s", disk_labels[index]);
   return true;
}

static struct retro_disk_control_ext_callback disk_ext_cb = {
   disk_set_eject_state,
   disk_get_eject_state,
   disk_get_image_index,
   disk_set_image_index,
   disk_get_num_images,
   disk_replace_image_index,
   disk_add_image_index,
   disk_set_initial_image,
   disk_get_image_path,
   disk_get_image_label,
};

static struct retro_disk_control_callback disk_cb = {
   disk_set_eject_state,
   disk_get_eject_state,
   disk_get_image_index,
   disk_set_image_index,
   disk_get_num_images,
   disk_replace_image_index,
   disk_add_image_index,
};

/* Build the disc list from the content path and publish the boot image.  Runs
   in retro_load_game, before the engine (and its CD block) exists. */
static void disk_init_from_content(const char *content_path)
{
   disk_count     = 0;
   disk_index     = 0;
   disk_tray_open = 0;

   if (content_path != NULL && content_path[0] != '\0' &&
       disk_ext_is_m3u(content_path)) {
      char basedir[YAB_PATH_MAX];
      char line[YAB_PATH_MAX];
      FILE *fp;
      disk_dirname(content_path, basedir, sizeof(basedir));
      fp = fopen(content_path, "r");
      if (fp != NULL) {
         while (fgets(line, sizeof(line), fp) != NULL && disk_count < YAB_MAX_DISKS) {
            char *p = line;
            char *end;
            /* a UTF-8 BOM in front of the first entry would hide it */
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF)
               p += 3;
            while (*p == ' ' || *p == '\t')
               p++;
            if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
               continue;
            end = p + strlen(p);
            while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                               end[-1] == ' ' || end[-1] == '\t'))
               *--end = '\0';
            if (*p == '\0')
               continue;
            if (*p == '/')
               snprintf(disk_paths[disk_count], YAB_PATH_MAX, "%s", p);
            else
               snprintf(disk_paths[disk_count], YAB_PATH_MAX, "%s%s", basedir, p);
            disk_set_label_from_path(disk_count);
            disk_count++;
         }
         fclose(fp);
      }
      LOG_INFO("disk: playlist %s -> %u image(s)", content_path, disk_count);
   }

   if (disk_count == 0) {   /* single image, or an empty/unreadable playlist */
      if (content_path != NULL && content_path[0] != '\0') {
         snprintf(disk_paths[0], YAB_PATH_MAX, "%s", content_path);
         disk_set_label_from_path(0);
         disk_count = 1;
      }
   }

   /* Honour a frontend-requested initial image (last used disc restore). */
   if (disk_initial_idx < disk_count &&
       (disk_initial_path[0] == '\0' ||
        strcmp(disk_initial_path, disk_paths[disk_initial_idx]) == 0))
      disk_index = disk_initial_idx;
   disk_initial_idx = 0;
   disk_initial_path[0] = '\0';

   if (disk_count > 0)
      snprintf(content_path_buf, sizeof(content_path_buf), "%s",
               disk_paths[disk_index]);
   else
      content_path_buf[0] = '\0';
}

/* env 57 says which disk-control struct the front end understands; the ext form
   (env 58) supersedes the original one (env 13). */
static void disk_register(void)
{
   unsigned version = 0;
   if (environ_cb == NULL)
      return;
   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) &&
       version >= 1)
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_ext_cb);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_cb);
   LOG_INFO("disk: control interface registered, %u image(s), boot=%s",
            disk_count, content_path_buf[0] ? content_path_buf : "(none)");
}

/* "x.zip" (case-insensitive) */
static int path_ext_is_zip(const char *path)
{
   const char *ext = strrchr(path, '.');
   if (ext == NULL)
      return 0;
   ext++;
   return (ext[0] == 'z' || ext[0] == 'Z') && (ext[1] == 'i' || ext[1] == 'I') &&
          (ext[2] == 'p' || ext[2] == 'P') && ext[3] == '\0';
}

/* <save>/stv, per-game <game>.nv (EEPROM) and <game>.ram (backup card).  The
   directory is created here because eeprom_deinit()/the backup card save only
   work when it exists. */
static void stv_setup_save_dirs(void)
{
   const char *save = libretro_save_dir[0] ? libretro_save_dir : ".";

   snprintf(stv_eeprom_dir, sizeof(stv_eeprom_dir), "%s/stv/", save);
   if (mkdir(stv_eeprom_dir, 0777) != 0) {
      struct stat st;
      if (stat(stv_eeprom_dir, &st) != 0 || !S_ISDIR(st.st_mode))
         LOG_WARN("ST-V: cannot create %s (EEPROM/backup card will not persist)",
                  stv_eeprom_dir);
   }
   snprintf(stv_bup_path, sizeof(stv_bup_path), "%s%s.ram", stv_eeprom_dir,
            stv_game != NULL ? stv_game : "stv");
}

/* Publish the Saturn work-RAM layout so the frontend can address it (cheat
   search, RetroAchievements).  Called once the engine has allocated the
   buffers: same descriptors as the reference line. */
static void set_memory_maps(void)
{
   static struct retro_memory_descriptor descs[2];
   struct retro_memory_map mmap;

   if (!HighWram || !LowWram)
      return;

   memset(descs, 0, sizeof(descs));

   /* High Work RAM -- HWRAM @ 0x06000000, 1 MB. */
   descs[0].flags     = RETRO_MEMDESC_SYSTEM_RAM;
   descs[0].ptr       = HighWram;
   descs[0].start     = 0x06000000;
   descs[0].len       = 0x100000;
   descs[0].addrspace = "HWRAM";

   /* Low Work RAM -- LWRAM @ 0x00200000, 1 MB. */
   descs[1].flags     = RETRO_MEMDESC_SYSTEM_RAM;
   descs[1].ptr       = LowWram;
   descs[1].start     = 0x00200000;
   descs[1].len       = 0x100000;
   descs[1].addrspace = "LWRAM";

   mmap.descriptors     = descs;
   mmap.num_descriptors = 2;
   if (environ_cb != NULL)
      environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
}

static void sram_seed_from_backup(const char *path)
{
   FILE *bf = (path != NULL && path[0] != '\0') ? fopen(path, "rb") : NULL;
   size_t n = 0;
   if (bf)
   {
      n = fread(sram_shadow, 1, sizeof(sram_shadow), bf);
      fclose(bf);
      if (n < sizeof(sram_shadow))
         FormatBackupRam(sram_shadow, sizeof(sram_shadow));
   }
   else
      FormatBackupRam(sram_shadow, sizeof(sram_shadow));
   LOG_INFO("backup RAM: %s (%s)", (path != NULL && path[0]) ? path : "(none)",
            (bf && n == sizeof(sram_shadow)) ? "loaded" : "fresh card");
   sram_apply_pending = 1;
}

static void sram_sync(void)
{
   if (!BupRam)
      return;
   if (sram_apply_pending)
   {
      memcpy(BupRam, sram_shadow, sizeof(sram_shadow));
      sram_apply_pending = 0;
   }
   else
      memcpy(sram_shadow, BupRam, sizeof(sram_shadow));
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   /* A new load starts from scratch: drop the previous title's ST-V state. */
   free(stv_game);
   stv_game = NULL;
   stv_mode = 0;
   stv_rotated = 0;
   stv_content_path[0] = '\0';
   stv_bios_path[0]  = '\0';
   stv_eeprom_dir[0] = '\0';
   stv_bup_path[0]   = '\0';

   if (game != NULL) {
      const char *content = game->path;
      if (content != NULL && path_ext_is_zip(content)) {
         /* ST-V: a MAME romset.  Detection needs the board BIOS (the game list
            entries are matched together with it), so look for it next to the
            front end's system files and fail loudly when it is missing. */
         const char *sysdir = libretro_system_dir[0] ? libretro_system_dir : ".";
         char *romset = NULL;

         snprintf(stv_bios_path, sizeof(stv_bios_path), "%s/stvbios.zip", sysdir);
         if (access(stv_bios_path, R_OK) != 0) {
            LOG_WARN("ST-V: %s is missing; a romset cannot be identified "
                      "without the ST-V BIOS", stv_bios_path);
            return false;
         }
         STVGetSingle(content, stv_bios_path, &romset);
         if (romset == NULL) {
            LOG_WARN("ST-V: %s has no romset in the game list; this core does "
                      "not read zipped Saturn images", content);
            return false;
         }
         stv_mode = 1;
         stv_game = romset;                 /* owned from here on */
         stv_rotated = STVGetRotated(romset);
         snprintf(stv_content_path, sizeof(stv_content_path), "%s", content);
         libretro_content_path = stv_content_path;
         stv_setup_save_dirs();
         LOG_INFO("load_game: ST-V romset '%s' from %s (rotation=%d, eeprom=%s, "
                  "card=%s)", stv_game, content, stv_rotated,
                  stv_eeprom_dir, stv_bup_path);
      } else {
         /* Content is a disc image or an .m3u playlist of them: build the disc
            list and let the engine boot its first entry.  A vertical ST-V title
            leaves yabsys.isRotated set; a Saturn game reports 0. */
         yabsys.isRotated = 0;
         disk_init_from_content(content);
         libretro_content_path = (content_path_buf[0] != '\0') ? content_path_buf
                                                               : content;
         LOG_INFO("load_game: %s (%u bytes) -> boot image %s",
                  content != NULL ? content : "(no path)",
                  (unsigned)game->size,
                  libretro_content_path != NULL ? libretro_content_path : "(none)");
      }
   } else {
      disk_init_from_content(NULL);
      libretro_content_path = NULL;
      LOG_INFO("load_game: no content (placeholder)");
   }

   /* Prime the SAVE_RAM shadow from the core's own backup card; the frontend
      may then overlay its .srm on top before the first machine frame.  This has
      to happen synchronously here: the frontend reads retro_get_memory_data()
      right after load_game returns, and the engine is built later (context_reset
      on the machine thread). */
   sram_seed_from_backup(stv_mode ? stv_bup_path : libretro_bup_path);

   /* ST-V EEPROM: with the option off (default) the machine starts from the
      factory table in memory and nothing is written on exit, so a test run
      always sees the same fresh NVRAM. */
   if (stv_mode)
      stv_eeprom_persist = opt_stv_eeprom_save;

   /* Core options: restart-type values are latched here, before the engine is
      built.  The video standard has to be known now because the front end asks
      for retro_get_system_av_info() right after load_game returns. */
   yab_options_read();
   yab_videoformat_latch();

   /* libretro input protocol: ports, bitmask capability, descriptors. */
   yab_input_ports_init();
   if (stv_mode)
      players = 2;   /* two cabinet panels, no multitap */
   yab_input_descriptors();

   /* Async audio: ask for the front end's audio callback.  Registering it makes
      the audio callbacks run on the front end's audio thread; the engine is
      started later (hw.context_reset), so record the answer here. */
   memset(&audio_cb_cfg, 0, sizeof(audio_cb_cfg));
   audio_cb_cfg.callback = yab_audio_callback;
   audio_cb_cfg.set_state = yab_audio_set_state;
   audio_async_on = 0;
   audio_async_ok = (environ_cb != NULL &&
                     environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK,
                                &audio_cb_cfg)) ? 1 : 0;
   LOG_INFO("audio: env22 async %s", audio_async_ok
            ? "accepted (front end drives audio)" : "refused (consumer thread)");
   {
      struct retro_audio_buffer_status_callback bs;
      memset(&bs, 0, sizeof(bs));
      bs.callback = yab_audio_buffer_status;
      if (environ_cb != NULL &&
          environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &bs))
         LOG_INFO("audio: env62 buffer status accepted");
      else
         LOG_INFO("audio: env62 buffer status refused");
   }

   /* Disk control: the front end's disc list for this content.  An ST-V romset
      has no disc to swap. */
   if (!stv_mode)
      disk_register();

   return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info,
                                       size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   LOG_INFO("unload_game");
   /* The frontend usually ran hw.context_destroy first; this call is idempotent
      and also covers frontends that only call unload_game. */
   LibretroEngineContextDestroy();
   free(stv_game);
   stv_game = NULL;
   stv_mode = 0;
   stv_rotated = 0;
   stv_content_path[0] = '\0';
   stv_bios_path[0]  = '\0';
   stv_eeprom_dir[0] = '\0';
   stv_bup_path[0]   = '\0';
   players = 2;
}

RETRO_API unsigned retro_get_region(void)
{
   return (opt_videoformat_latched == VIDEOFORMATTYPE_PAL) ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
   switch (id) {
      case RETRO_MEMORY_SAVE_RAM:   return sram_shadow;
      case RETRO_MEMORY_SYSTEM_RAM: return HighWram;
      default:                      return NULL;
   }
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   switch (id) {
      case RETRO_MEMORY_SAVE_RAM:   return sizeof(sram_shadow);
      case RETRO_MEMORY_SYSTEM_RAM: return 0x100000;
      default:                      return 0;
   }
}
