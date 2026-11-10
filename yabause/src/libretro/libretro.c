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
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "log.h"
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#include <GLES3/gl3.h>

#include "yabause.h"
#include "vdp1.h"     /* VIDCore->GetNativeResolution / Resize */
#include "vidogl.h"
#include "sh2core.h"
#include "scsp.h"
#include "sndlibretro.h"
#include "m68kcore.h"
#include "cdbase.h"
#include "cs0.h"
#include "osdcore.h"
#include "peripheral.h"
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

/* engine-side caller mode: retro_run drains the VDP event queue (A+token) */
extern void Vdp2SetFrontendConsumer(int on);
extern void Vdp2SetRenderThreadHere(void);
extern void Vdp2SetEventPostedHook(void (*fn)(void));
extern int  Vdp2ConsumeFrontendEvent(void);
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
/* Temporary probe: how long the two joins actually block (see DESIGN note). */
extern void Vdp2TakeWaitStats(unsigned *vin_n, unsigned *vin_us,
                              unsigned *rnd_n, unsigned *rnd_us,
                              unsigned *drawend_machine_n, unsigned *drawend_side_n);

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
enum { YAB_CMD_NONE = 0, YAB_CMD_SAVE, YAB_CMD_LOAD, YAB_CMD_RESET };

static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cmd_type = YAB_CMD_NONE;
static int cmd_done = 1;
static int cmd_ok = 0;
static const void *cmd_load_buf;
static size_t cmd_load_size;
static void *cmd_save_buf;
static size_t cmd_save_size;
static size_t state_size;          /* stable retro_serialize_size() */

extern int YabSaveStateBuffer(void **buffer, size_t *size);
extern int YabLoadStateBuffer(const void *buffer, size_t size);

/* Engine work of one command.  Runs on the machine thread normally, and on the
   frontend thread when no machine thread exists (nothing can race then). */
static int yab_cmd_exec(int type, const void *load, size_t load_size)
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
   } else if (type == YAB_CMD_RESET) {
      YabauseReset();
      ok = 1;
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

   pthread_mutex_lock(&cmd_mtx);
   type = cmd_type;
   load = cmd_load_buf;
   n = cmd_load_size;
   pthread_mutex_unlock(&cmd_mtx);
   if (type == YAB_CMD_NONE)
      return;
   ok = yab_cmd_exec(type, load, n);
   pthread_mutex_lock(&cmd_mtx);
   cmd_ok = ok;
   cmd_type = YAB_CMD_NONE;
   cmd_done = 1;
   pthread_mutex_unlock(&cmd_mtx);
}

/* Frontend thread. */
static int yab_cmd_wait(int type, const void *load, size_t load_size)
{
   int ok;
   if (!machine_started)
      return yab_cmd_exec(type, load, load_size);
   pthread_mutex_lock(&cmd_mtx);
   cmd_load_buf = load;
   cmd_load_size = load_size;
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
   if (frame_width > 0 && nw == game_width && nh == game_height)
      return;

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
   if ((frame_ready % 120) == 1 && hw.get_current_framebuffer) {
      unsigned char px[4 * 16];
      GLint prev = 0;
      unsigned sum = 0, i;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)(uintptr_t)hw.get_current_framebuffer());
      glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (i = 0; i < sizeof(px); i++) sum += px[i];
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
      LOG_INFO("fbo readback #%u: sum=%u px0=%u,%u,%u,%u err=0x%x", frame_ready, sum,
               px[0], px[1], px[2], px[3], (unsigned)glGetError());
   }
}

static void log_gl_state(const char *tag)
{
   GLint fbo = -1, prog = -1, vao = -1;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
   glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
   glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
   LOG_INFO("gl[%s]: fbo=%d program=%d vao=%d err=0x%x", tag, fbo, prog, vao,
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

/* CD read trace (env YAB_CDTRACE=1): one line per sector the CD block delivers,
   so the movie/file reads can be compared with the standalone.  Called from
   cs2.c inside the libretro build only. */
void LibretroCdTrace(unsigned int fad, int ret, int isaudio,
                     unsigned status, unsigned ptiming, unsigned pcycles)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("YAB_CDTRACE");
      on = (e != NULL && e[0] == '1') ? 1 : 0;
   }
   if (!on) return;
   LOG_INFO("cd: fad=%u ret=%d audio=%d status=%02x pt=%u pc=%u",
            fad, ret, isaudio, status, ptiming, pcycles);
}

/* CD command trace (env YAB_CDCMD=1): one line when a command is executed and one
   with the result registers, so the command stream can be compared with the
   standalone.  Called from cs2.c inside the libretro build only. */
void LibretroCdCmdTrace(unsigned cmd, unsigned fad, unsigned a, unsigned b,
                        unsigned c, unsigned d, int done)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("YAB_CDCMD");
      on = (e != NULL && e[0] == '1') ? 1 : 0;
   }
   if (!on) return;
   if (!done)
      LOG_INFO("cdcmd: cmd=%02x fad=%u cr2=%04x cr3=%04x cr4=%04x status=%04x",
               cmd, fad, a, b, c, d);
   else
      LOG_INFO("cdcmd: done cmd=%02x cr1=%04x cr2=%04x cr3=%04x hirq=%04x",
               cmd, a, b, c, d);
}

/* CD data-delivery trace (env YAB_CDDATA=1), called from cs2.c. */
void LibretroCdDataTrace(const char *what, unsigned a, unsigned b, unsigned c, unsigned e)
{
   static int on = -1;
   if (on < 0) {
      const char *en = getenv("YAB_CDDATA");
      on = (en != NULL && en[0] == '1') ? 1 : 0;
   }
   if (!on) return;
   LOG_INFO("cddata: %s %u %u %u %u", what, a, b, c, e);
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
   int occ = __atomic_load_n(&aud_occ, __ATOMIC_RELAXED);
   int dry = __atomic_load_n(&aud_underrun_likely, __ATOMIC_RELAXED);

   if (dry || occ < 0 || occ < 25)
      return 4;              /* starving: refill the front end's buffer */
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

void LibretroEngineContextReset(void)
{
   if (engine_up) return;
   LOG_INFO("engine: YabauseInit (frontend consumer)");
   log_gl_state("before-init");
   Vdp2SetFrontendConsumer(1);
   {
      static yabauseinit_struct yinit;
      memset(&yinit, 0, sizeof(yinit));
      yinit.sh2coretype     = 3;
      yinit.vidcoretype     = VIDCORE_OGL;
      yinit.sndcoretype     = SNDCORE_LIBRETRO;   /* SCSP thread -> retro_run -> audio_batch_cb */
      yinit.m68kcoretype    = M68KCORE_MUSASHI;
      yinit.percoretype     = PERCORE_LIBRETRO;   /* pad on port 1, see PERLIBRETROJoy */
      yinit.carttype        = CART_DRAM32MBIT;
      yinit.clocksync       = 0;
      yinit.use_new_scsp    = 1;
      yinit.extend_backup   = 1;
      yinit.video_filter_type = 0;
      yinit.polygon_generation_mode = PERSPECTIVE_CORRECTION;
      yinit.frameskip       = 0;
      yinit.resolution_mode = 3;   /* match the standalone baseline (-r 3) */
      yinit.rbg_use_compute_shader = true;   /* standalone default ("Use compute shader") */
      yinit.cdcoretype      = CDCORE_ISO;
      /* The rest of the standalone's yinit (main.cpp), field by field: the engine
         branches on these, so leaving them at their zero value is a silent config
         divergence (e.g. scsp_main_mode 0 = CpuTime changes the SCSP thread and
         makes SyncCPUtoSCSP wait every field).  See CONFIG-ALIGN-20261109.md. */
      yinit.scsp_main_mode  = 1;              /* Realtime SCSP thread (non-Jetson) */
      yinit.scsp_sync_count_per_frame = 1;    /* g_scsp_sync */
      yinit.rotate_screen   = 0;
      yinit.rbg_resolution_mode = 0;
      yinit.basetime        = 0;
      yinit.cartpath        = NULL;
      yinit.mpegpath        = NULL;
      yinit.regionid        = 0;
      yinit.biospath        = libretro_bios_path[0] ? libretro_bios_path : NULL;
      yinit.buppath         = libretro_bup_path[0] ? libretro_bup_path : NULL;
      yinit.cdpath          = libretro_content_path;
      yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
      yinit.framelimit      = 0;
      yinit.usethreads      = 0;
      yinit.osdcoretype     = OSDCORE_DUMMY;
      yinit.skip_load       = 0;
      g_emulated_bios = (libretro_bios_path[0] != 0) ? 0 : 1;   /* standalone: 0 when -b */
      /* Standalone config defaults (main.cpp): "sound dsp" = 1 -> DSP on;
         "texcache persist" = true.  The DSP is emulated hardware state, so it
         stays on.  Texture-cache persistence is left OFF: measured on the
         Daytona race (same save state) it costs 5-10 ms of draw time here, and
         with this renderer (single front-end FBO, present after retro_run) it
         degrades over a long run; the standalone's own config keeps it on. */
      g_scsp_dsp_off = 0;
      g_texcache_persist = 0;
      if (YabauseInit(&yinit) != 0) { LOG_WARN("YabauseInit failed"); return; }
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
      extern void VDP2SetFrameLimit(int mode);
      /* Autoframeskip is available but OFF by default.  The engine's skip
         decision compares wall time against an ideal 60 Hz schedule and is
         coupled to frameSkipAndLimit()'s own sleep; in the core the schedule
         comes from the front end's vsync (the swap happens after retro_run
         returns), so that measure sees every field as "behind" and the engine
         skips continuously (measured: Golden Axe 34 fps, skip ~21/s, machine
         still 16.7 ms).  Opt in with YAB_FRAMESKIP=1 only to study it.
         Mode 3 = 60 Hz budget without the sleep (the front end paces). */
      {
         const char *fs = getenv("YAB_FRAMESKIP");
         if (fs != NULL && fs[0] == '1') {
            VDP2SetFrameLimit(3);
            EnableAutoFrameSkip();
            LOG_INFO("engine frame limit: 60 Hz budget, autoframeskip ON (YAB_FRAMESKIP=1)");
         } else {
            /* Standalone `-nf` parity: the limiter itself stays on (framelimit 0
               = the 60 Hz schedule with its sleep/busy-wait), only the skip
               decision is disabled.  Mode 1 would short-circuit the whole
               function and leave a different execution path. */
            VDP2SetFrameLimit(0);
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
   if (frontend_log != NULL)
      frontend_log((enum retro_log_level)level, "%s", line);
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
         snprintf(libretro_bup_path, sizeof(libretro_bup_path), "%s/yabasanshiro.bup", save_dir);
      else
         snprintf(libretro_bup_path, sizeof(libretro_bup_path), "yabasanshiro.bup");
      LOG_INFO("bup path (env=%d dir=%s): %s", (int)have_save,
               save_dir ? save_dir : "(null)", libretro_bup_path);

      {
         const char *sys_dir = NULL;
         static const char *names[] = { "saturn_bios.bin", "sega_101.bin",
                                        "mpr-17933.bin", "mpr-18811-mx.ic1", NULL };
         int i;
         if (cb != NULL && cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir) {
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
   info->valid_extensions = "cue|iso|chd";
   info->need_fullpath = false; /* placeholder needs no content */
   info->block_extract = false;
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
   info->timing.fps = yabsys.IsPal ? 50.0 : 60.0;
   info->timing.sample_rate = 44100.0;
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
   /* libretro contract: poll at least once per retro_run, on this thread.  The
      input snapshot is published from the engine's VIN handler (vdp2.cpp). */
   if (input_poll_cb != NULL)
      input_poll_cb();
   SNDLibretroHeartbeat();   /* the only pause signal libretro offers us */
   yab_in_run = 1;

   if (engine_up) {
      Vdp2SetRenderThreadHere();   /* this thread renders now, not VdpProc */
      /* Saturn mode changes are applied before anything of this field is drawn.
         The engine's Resize()/YglGLInit() rebuilds GL objects, so it has to stay
         outside the render stack (a change is then visible one field late, which
         is what the standalone's fixed window hides anyway). */
      core_apply_resolution();
      /* One field per call, and the call does not return before that field has
         been handled: consume the events, and when the queue is empty block on
         the machine's post notification (no timeout).  Draining the whole queue
         instead would pump the machine to 86-97 fps, because it posts its VIN
         event before running the field.

         The break is per FIELD, not per presented frame: with autoframeskip a
         field may present nothing (VIDDummy), and returning only on presentation
         would halve the front end's call rate and therefore the game's speed. */
      const unsigned seq0 = Vdp2FieldSeq();
      struct timespec t0, t1, t2, w0, d0, d1;
      double draw_ms = 0.0, wait_ms = 0.0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      for (;;) {
         /* Drawing and waiting for the machine are measured apart: a heavy game
            keeps this thread waiting for VOUT most of the field, and folding
            that into one "render" number hides which side is the bottleneck. */
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
      acc_draw_ms += draw_ms;
      acc_wait_ms += wait_ms;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      /* Every call ends with a picture.  A field that autoframeskip dropped ran
         VIDDummy, so the FBO still holds the previous picture and re-presenting it
         is exactly what the standalone shows when it does not swap.  No
         video_cb(NULL): the hw-render frame buffer is always valid. */
      frame_sent = frame_ready;
      video_cb(RETRO_HW_FRAME_BUFFER_VALID,
               (unsigned)(frame_width > 0 ? frame_width : TEST_W),
               (unsigned)(frame_height > 0 ? frame_height : TEST_H), 0);
      clock_gettime(CLOCK_MONOTONIC, &t2);
      acc_present_ms += (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
      acc_calls++;
   } else {
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
               /* Temporary probe: how long the machine actually blocks at the two
                  joins, and which DRAWEND path fired.  Remove with the probe. */
               unsigned vn, vu, rn, ru, dm, ds;
               Vdp2TakeWaitStats(&vn, &vu, &rn, &ru, &dm, &ds);
               LOG_INFO("join: vin_n=%u vin_ms=%.2f rnd_n=%u rnd_ms=%.2f drawend machine=%u render=%u",
                        vn, vu / 1000.0, rn, ru / 1000.0, dm, ds);
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
               lp = p; lc = c; ld = d;
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
      LOG_INFO("run=%u ready=%u sent=%u drained=%u none=%u vin=%u",
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

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   if (game != NULL) {
      libretro_content_path = game->path;
      LOG_INFO("load_game: %s (%u bytes)", game->path != NULL ? game->path : "(no path)",
               (unsigned)game->size);
   } else {
      LOG_INFO("load_game: no content (placeholder)");
   }

   /* libretro input protocol: ports, bitmask capability, descriptors. */
   yab_input_ports_init();
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
}

RETRO_API unsigned retro_get_region(void)
{
   return yabsys.IsPal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}
