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
#include "vidogl.h"
#include "sh2core.h"
#include "scsp.h"
#include "m68kcore.h"
#include "cdbase.h"
#include "cs0.h"
#include "osdcore.h"
#include "peripheral.h"
#include "ygl.h"

static struct retro_hw_render_callback hw;
static int engine_up;
static const char *libretro_content_path;
static char libretro_bup_path[512];
static char libretro_bios_path[512];

/* engine-side caller mode: retro_run drains the VDP event queue (A+token) */
extern void Vdp2SetFrontendConsumer(int on);
extern void Vdp2SetRenderThreadHere(void);
extern int  Vdp2ConsumeFrontendEvent(void);

static pthread_t machine_thread;
static volatile int machine_run;
static int machine_started;

static void *machine_main(void *arg)
{
   (void)arg;
   while (machine_run)
      YabauseExec();
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

static uint16_t *frame_buf;
static int16_t *audio_buf;
static unsigned frame_count;
static unsigned frame_ready;
static unsigned frame_sent;
static unsigned drained_total, drained_none;
static double engine_ms_sum, present_ms_sum;
static unsigned timed_frames;

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

void *LibretroFrontFbo(void)
{
   return hw.get_current_framebuffer ? (void *)(uintptr_t)hw.get_current_framebuffer() : NULL;
}

int LibretroHasVideo(void)
{
   return video_cb != NULL;
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
      yinit.sndcoretype     = SNDCORE_DUMMY;
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
      yinit.rbg_use_compute_shader = false;
      yinit.cdcoretype      = CDCORE_ISO;
      yinit.carttype        = CART_NONE;
      yinit.regionid        = 0;
      yinit.biospath        = libretro_bios_path[0] ? libretro_bios_path : NULL;
      yinit.buppath         = libretro_bup_path[0] ? libretro_bup_path : NULL;
      yinit.cdpath          = libretro_content_path;
      yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
      yinit.framelimit      = 0;
      yinit.usethreads      = 0;
      yinit.osdcoretype     = OSDCORE_DUMMY;
      yinit.skip_load       = 0;
      if (YabauseInit(&yinit) != 0) { LOG_WARN("YabauseInit failed"); return; }
   {
      extern void VDP2SetFrameLimit(int mode);
      VDP2SetFrameLimit(1);            /* no limit: the frontend paces */
      LOG_INFO("engine frame limit: off (frontend paces)");
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
   info->geometry.base_width = TEST_W;
   info->geometry.base_height = TEST_H;
   info->geometry.max_width = TEST_W;
   info->geometry.max_height = TEST_H;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps = 60.0;
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
      /* A+token: drain the events the machine queued; rendering happens here,
         on the front end's thread, and YuiSwapBuffers issues video_cb. */
      int n = 0;
      struct timespec ta, tb, tc;
      clock_gettime(CLOCK_MONOTONIC, &ta);
      while (n < 16 && Vdp2ConsumeFrontendEvent())
         n++;
      clock_gettime(CLOCK_MONOTONIC, &tb);
      drained_total += (unsigned)n;
      if (n == 0) drained_none++;
      /* The engine's YuiSwapBuffers() only bumped frame_ready; present now, with
         the engine stack unwound and its drawing complete. */
      if (frame_ready != frame_sent) {
         frame_sent = frame_ready;
         video_cb(RETRO_HW_FRAME_BUFFER_VALID, TEST_W, TEST_H, 0);
      } else {
         video_cb(NULL, TEST_W, TEST_H, 0);   /* dupe: no new field this call */
      clock_gettime(CLOCK_MONOTONIC, &tc);
      if (n > 0) {
         engine_ms_sum += (tb.tv_sec - ta.tv_sec) * 1e3 + (tb.tv_nsec - ta.tv_nsec) / 1e6;
         present_ms_sum += (tc.tv_sec - tb.tv_sec) * 1e3 + (tc.tv_nsec - tb.tv_nsec) / 1e6;
         timed_frames++;
      }
      }
   } else {
      render_test_card();
   }

   if (!engine_up && audio_batch_cb != NULL)
      audio_batch_cb(audio_buf, AUDIO_FRAMES); /* silence */

   frame_count++;
   {
      static struct timespec last;
      static unsigned last_ready, last_run, last_drain;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (last.tv_sec == 0) { last = now; last_ready = frame_ready; last_run = frame_count; last_drain = drained_total; }
      else if (now.tv_sec != last.tv_sec) {
         double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
         {
                                                /* Same three numbers the standalone OSD prints (PERF-BASELINE.md):
               fps / skip / vdp1, sampled once per second. */
            extern int show_vdp1_frame;
            extern unsigned int show_skipped_frame;
            double n = timed_frames > 0 ? (double)timed_frames : 1.0;
            /* Same line, same fields, same sources as the standalone probe:
               PERFLOG fps=<rendered fields/s> skip=<show_skipped_frame> vdp1=<show_vdp1_frame> */
            LOG_INFO("PERFLOG fps=%d skip=%u vdp1=%d",
                     (int)((frame_ready - last_ready) / dt + 0.5), show_skipped_frame,
                     show_vdp1_frame);

            engine_ms_sum = present_ms_sum = 0.0; timed_frames = 0;
         }
         last = now; last_ready = frame_ready; last_run = frame_count; last_drain = drained_total;
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
   return RETRO_REGION_NTSC;
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
