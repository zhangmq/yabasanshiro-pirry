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
static int machine_started;

static void *machine_main(void *arg)
{
   (void)arg;
   while (machine_run) {
      struct timespec m0, m1;
      clock_gettime(CLOCK_MONOTONIC, &m0);
      YabauseExec();
      clock_gettime(CLOCK_MONOTONIC, &m1);
      pthread_mutex_lock(&st_mtx);
      acc_machine_ms += (m1.tv_sec - m0.tv_sec) * 1e3 + (m1.tv_nsec - m0.tv_nsec) / 1e6;
      acc_machine_n++;
      pthread_mutex_unlock(&st_mtx);
   }
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
      yinit.percoretype     = PERCORE_DUMMY;
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
      /* Audio: the SCSP thread produces at its own wall-clock rate, the consumer
         thread of sndlibretro.c drains it in 512-frame periods and calls this,
         so nothing in retro_run can block on the front end's audio ring. */
      SNDLibretroStart(libretro_audio_submit);
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
      machine_run = 0;
      pthread_join(machine_thread, NULL);
      machine_started = 0;
   }
   SNDLibretroStop();
   LOG_INFO("engine: YabauseDeInit");
   YabauseDeInit();
   engine_up = 0;
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
   (void)port;
   (void)device;
}

RETRO_API void retro_reset(void)
{
   /* frame_count is a diagnostic counter since retro_init(), not emulated time. */
   LOG_INFO("placeholder core reset");
}

RETRO_API void retro_run(void)
{
   if (input_poll_cb != NULL)
      input_poll_cb();

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

         }
         last = now; last_ready = frame_ready; last_run = frame_count;
         last_drain = drained_total; last_fields = Vdp2FieldSeq();
      }
   }

   if (frame_count % FRAME_LOG_INTERVAL == 0)
      LOG_INFO("run=%u ready=%u sent=%u drained=%u none=%u vin=%u",
               frame_count, frame_ready, frame_sent, drained_total, drained_none, engine_up);
}

RETRO_API size_t retro_serialize_size(void)
{
   return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
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
