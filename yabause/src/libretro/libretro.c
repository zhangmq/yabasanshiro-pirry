#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include <sys/stat.h>
#include <strings.h>

#include <libretro.h>

#include <file/file_path.h>

#include <unistd.h>
#include <time.h>
#include <pthread.h>
#ifdef YAB_HANGDBG
#include <sys/syscall.h>
#endif
#include "vdp1.h"
#include "vdp2.h"
#include "smpc.h"
#include "scu.h"
#include "peripheral.h"
#include "cdbase.h"
#include "yabause.h"
#include "yui.h"
#include "cheat.h"

#include "cs0.h"
#include "cs2.h"
#include "stv.h"

#include "m68kcore.h"
#include "vidogl.h"
#include "vidsoft.h"

#include "ygl.h"
#include "yab_trace.h"   /* YAB_TRACE_LEVEL: compile-time probe levels */

/* Core options v2 table ported from the reference libretro core
 * (lr-yabasanshiro) -- same keys, defaults, categories and descriptions. */
#include "libretro_core_options.h"

yabauseinit_struct yinit;

static char slash = path_default_slash_c();

static char g_save_dir[PATH_MAX];
static char g_system_dir[PATH_MAX];
static char full_path[PATH_MAX];
static char bios_path[PATH_MAX];
/* ST-V (Sega Titan Video) -- ported from libretro/yabause@kronos */
static char stv_bios_path[PATH_MAX];
static char stv_eeprom_dir[PATH_MAX];
static char *stvgame = NULL;
static bool stv_mode = false;
static int stv_favorite_region = STV_REGION_EU;
/* Mirror of the stv_mode flag for yabause.c (per-line and the 68k lockstep
 * rules exclude ST-V).  The old "sound CPU lead" workaround that this mirror
 * also served is gone: after the SH-2 FRT doorbell / sync-model alignment
 * (commit d45b31d8) seabass boots with the sound CPU free-running. */
int yab_stv_mode = 0;
static bool service_enabled = false;   /* kronos_service_enabled: Test/Service/Pause keys */
static char bup_path[PATH_MAX];

static int game_width  = 320;
static int game_height = 240;
static int game_interlace;

static int current_width;
static int current_height;

static bool renderer_running = false;

/* Async emulation thread control (defined next to retro_run). */
static void yab_emu_start(void);
static void yab_emu_stop(void);
static void yab_emu_pause(void);
static void yab_emu_resume(void);
static bool hle_bios_force = false;
static bool one_frame_rendered = false;
static volatile unsigned yab_swap_count = 0;   /* pictures drawn by the worker */
static volatile unsigned yab_present_ack = 0;  /* pictures the frontend has presented */
static int yab_vcrc_nopresent = 0;
/* 最近一次合成（YglRender 末尾）的画面 CRC，见 yab_after_compose()。 */
static unsigned yab_last_crc = 0;

static bool libretro_supports_bitmasks = false;
 
#ifdef DYNAREC_DEVMIYAX
static int g_sh2coretype = 3;
#else
static int g_sh2coretype = SH2CORE_INTERPRETER;
#endif

/* Renderer mode.  There is only one model now (design §1): the machine runs on
 * its own thread and the frontend's retro_run renders the field it dispatches.
 * The old "Async/Sync Renderer" core option is gone (76ca714b); g_vdp_async is
 * re-asserted to 1 in retro_load_game() and nothing ever sets it to 0, so every
 * test of it reads a constant.
 *
 * TODO(2026-10-31): the `if (g_vdp_async)` bodies are the live path (mirror
 * FBO, the core-owned shared context, VDP1 readback) -- keep them.  Only the
 * alternates that need `!g_vdp_async` to be reachable are dead (e.g. the early
 * return of yk_adopt_front_fbo()).  The synchronous path may come back later
 * (user decision, 2026-10-31); this flag is the switch to revive, see
 * .notes/fork-lineage/SYNCPATH-STATUS-2026-10-31.md */
/* Video standard (core option "yabasanshiro_videoformattype"): _opt is what
 * the option/env asks for (-1 = auto), _pending is the value latched for this
 * content in retro_load_game() before the frontend asks for av_info/region.
 * auto reads the Saturn disc's IP.BIN region field (E/A/L = PAL, J/U/T/K/B =
 * NTSC, anything unreadable = NTSC).  ST-V is NTSC-only hardware, so its
 * pending value is always NTSC. */
#define YAB_VIDEOFORMAT_AUTO (-1)
static int g_videoformat_opt = YAB_VIDEOFORMAT_AUTO;
static int g_videoformat_pending = -1;
/* Auto Frameskip (core option "yabasanshiro_frameskip"): 0 = off, 1 = auto.
 * Declared here because check_variables() (below) reads it; the skip decision
 * lives in vdp2.cpp (frameSkipAndLimit at VIN, applied by the next VOUT). */
static int g_frameskip = 0;
static int g_rbg_resolution_mode = 0;
static int g_rbg_use_compute_shader = 1;
static int addon_cart_type = CART_DRAM32MBIT;
static int resolution_mode = 1;
static int initial_resolution_mode = 0;
static int g_resolution_mode = 0;
#ifdef LOW_END
static int max_resolution_mode = 2;
#else
static int max_resolution_mode = 4;
#endif
static int polygon_mode = PERSPECTIVE_CORRECTION;
static int pad_type[12] = {1,1,1,1,1,1,1,1,1,1,1,1};
static int multitap[2] = {0,0};
static unsigned players = 7;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

#if defined(_USEGLEW_)
static struct retro_hw_render_callback hw_render;
#else
extern struct retro_hw_render_callback hw_render;
#endif

/* defined with the disk-control block further down */
static void disk_register(void);

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_controller_description peripherals[] = {
       { "Saturn Pad", RETRO_DEVICE_JOYPAD },
       { "Saturn 3D Pad", RETRO_DEVICE_ANALOG },
       { "None", RETRO_DEVICE_NONE },
   };

   static const struct retro_controller_info ports[] = {
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { peripherals, 3 },
      { NULL, 0 },
   };

   environ_cb = cb;

   /* Options come from libretro_core_options.h (v2 with categories; the setter
    * falls back to v1/SET_VARIABLES on older frontends). */
   {
      bool option_cats_supported = false;
      libretro_set_core_options(cb, &option_cats_supported);
   }

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   /* Multi-disc swap / .m3u playlists (same interface set as the reference core). */
   disk_register();

   /* RetroAchievements: the Saturn work RAM published through set_memory_maps()
    * (HWRAM @0x06000000, LWRAM @0x00200000) is stored byte-swapped the same way
    * Beetle Saturn/Yabause store it, which is what the RA sets were authored
    * against (same statement as the reference core). */
   {
      bool support_achievements = true;
      environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &support_achievements);
   }
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// PERLIBRETRO
#define PERCORE_LIBRETRO 2

/* ST-V JAMMA key maps (ported from libretro/yabause@kronos libretro.c). */
typedef struct { unsigned id; unsigned key; unsigned player; const char *description; } KeyConfig_struct;

static KeyConfig_struct system_key_config[] = {
   { RETRO_DEVICE_ID_JOYPAD_L2, PERJAMMA_TEST,    0, "Test"    },
   { RETRO_DEVICE_ID_JOYPAD_R2, PERJAMMA_SERVICE, 0, "Service" },
   { RETRO_DEVICE_ID_JOYPAD_L3, PERJAMMA_PAUSE,   0, "Pause"   },
};

static KeyConfig_struct stv_key_config[] = {
   { RETRO_DEVICE_ID_JOYPAD_SELECT, PERJAMMA_COIN1,      0, "Coin"   },
   { RETRO_DEVICE_ID_JOYPAD_START,  PERJAMMA_START1,     0, "Start"  },
   { RETRO_DEVICE_ID_JOYPAD_UP,     PERPAD_UP,           0, "Up"     },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT,  PERPAD_RIGHT,        0, "Right"  },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,   PERPAD_DOWN,         0, "Down"   },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,   PERPAD_LEFT,         0, "Left"   },
   { RETRO_DEVICE_ID_JOYPAD_B,      PERPAD_A,            0, "Button1"},
   { RETRO_DEVICE_ID_JOYPAD_A,      PERPAD_B,            0, "Button2"},
   { RETRO_DEVICE_ID_JOYPAD_Y,      PERPAD_C,            0, "Button3"},
   { RETRO_DEVICE_ID_JOYPAD_X,      PERPAD_X,            0, "Button4"},
   { RETRO_DEVICE_ID_JOYPAD_SELECT, PERJAMMA_COIN2,      1, "Coin"   },
   { RETRO_DEVICE_ID_JOYPAD_START,  PERJAMMA_START2,     1, "Start"  },
   { RETRO_DEVICE_ID_JOYPAD_UP,     PERJAMMA_P2_UP,      1, "Up"     },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT,  PERJAMMA_P2_RIGHT,   1, "Right"  },
   { RETRO_DEVICE_ID_JOYPAD_DOWN,   PERJAMMA_P2_DOWN,    1, "Down"   },
   { RETRO_DEVICE_ID_JOYPAD_LEFT,   PERJAMMA_P2_LEFT,    1, "Left"   },
   { RETRO_DEVICE_ID_JOYPAD_B,      PERJAMMA_P2_BUTTON1, 1, "Button1"},
   { RETRO_DEVICE_ID_JOYPAD_A,      PERJAMMA_P2_BUTTON2, 1, "Button2"},
   { RETRO_DEVICE_ID_JOYPAD_Y,      PERJAMMA_P2_BUTTON3, 1, "Button3"},
   { RETRO_DEVICE_ID_JOYPAD_X,      PERJAMMA_P2_BUTTON4, 1, "Button4"},
};

static KeyConfig_struct* current_key_config = NULL;
static int current_key_config_nb = 0;
static int system_key_config_nb = 0;

#define STV_KEYCFG_NB(a) ((int)(sizeof(a)/sizeof(a[0])))

/* Ported from libretro/yabause@kronos.  Descriptors must be sent AFTER
   PERLIBRETROInit() has filled system_key_config_nb/current_key_config_nb --
   sending them from retro_load_game_common() (where those are still 0) gives
   the frontend an EMPTY list, and minarch then marks every button as
   unavailable and ignores it, so no input reaches the core at all. */
static void set_descriptors(void)
{
   int nb_descriptors = ((stv_mode?(system_key_config_nb+current_key_config_nb):(17*players))+1);
   struct retro_input_descriptor *input_descriptors = (struct retro_input_descriptor*)calloc(nb_descriptors, sizeof(struct retro_input_descriptor));

   if(stv_mode)
   {
      unsigned j = 0;
      if (service_enabled)
      {
         for (unsigned i = 0; i < (unsigned)system_key_config_nb; i++)
            input_descriptors[j++] = (struct retro_input_descriptor){ system_key_config[i].player, RETRO_DEVICE_JOYPAD, 0, system_key_config[i].id, system_key_config[i].description };
      }
      for (unsigned i = 0; i < (unsigned)current_key_config_nb; i++)
         input_descriptors[j++] = (struct retro_input_descriptor){ current_key_config[i].player, RETRO_DEVICE_JOYPAD, 0, current_key_config[i].id, current_key_config[i].description };
      input_descriptors[j].description = NULL;
   }
   else
   {
      unsigned j = 0;
      for (unsigned i = 0; i < players; i++)
      {
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Z" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "C" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,  "Analog X" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,  "Analog Y" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" };
         input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" };
      }
      input_descriptors[j].description = NULL;
   }
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
   free(input_descriptors);
}

/* Running first frame: all device ids were set, so send the descriptors. */
static bool all_devices_ready = false;
int PERLIBRETROInit(void)
{
   void *controller;

   uint32_t i, j;

   if (stv_mode) {
      /* Kronos only registers the system keys (Test/Service/Pause) when the
         service option is on; the game keys are always registered. */
      system_key_config_nb = service_enabled ? STV_KEYCFG_NB(system_key_config) : 0;
      current_key_config_nb = STV_KEYCFG_NB(stv_key_config);
      current_key_config = stv_key_config;
      PerPortReset();
      controller = (void*)PerCabAdd(NULL);   /* JAMMA cabinet: keys write IOPORT[] */
      for (i = 0; i < (uint32_t)system_key_config_nb; i++)
         PerSetKey(system_key_config[i].key, system_key_config[i].key, controller);
      for (i = 0; i < (uint32_t)current_key_config_nb; i++)
         PerSetKey(current_key_config[i].key, current_key_config[i].key, controller);

      players = 2;
      return 0;
   }
   PortData_struct* portdata = NULL;

   //1 multitap + 1 peripherial
   if(multitap[0] == 0 && multitap[1] == 0)
      players = 2;
   else if(multitap[0] == 1 && multitap[1] == 1)
      players = 12;

   PerPortReset();

   for(i = 0; i < players; i++)
   {
      //Ports can handle 6 peripherals, fill port 1 first.
      if((players > 2 && i < 6) || i == 0)
         portdata = &PORTDATA1;
      else
         portdata = &PORTDATA2;

      switch(pad_type[i])
      {
         case RETRO_DEVICE_NONE:
            controller = NULL;
            break;
         case RETRO_DEVICE_ANALOG:
            controller = (void*)Per3DPadAdd(portdata);
            for(j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, j, controller);
            for(j = PERANALOG_AXIS1; j <= PERANALOG_AXIS7; j++)
               PerSetKey((i << 8) + j, j, controller);
            break;
         case RETRO_DEVICE_JOYPAD:
         default:
            controller = (void*)PerPadAdd(portdata);
            for(j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, j, controller);
            break;
      }
   }

   return 0;
}

/* ---------------------------------------------------------------------------
 * Input snapshot (design §7 #4)
 *
 * input_poll_cb()/input_state_cb() may only be called from the frontend thread.
 * The frontend's retro_run() polls once per call and fills this snapshot; the
 * emulation thread (PERLIBRETROHandleEvents) only reads it.  The snapshot holds
 * everything the machine can ask for: the 16 joypad bits and, for analog pads,
 * the four axes plus the two analog triggers (with the digital fallback already
 * resolved, exactly like the old code did inline).
 *
 * Double buffered with an atomic index: the producer never writes the buffer the
 * machine is reading, so a mid-frame poll cannot tear the state.
 * ------------------------------------------------------------------------- */
#define YAB_INPUT_PORTS 12
typedef struct {
   u16 joypad;           /* RETRO_DEVICE_ID_JOYPAD_* bits, index 0 */
   int analog[4];        /* left X, left Y, right X, right Y */
   int trig_l2, trig_r2; /* analog-button L2/R2 with the digital fallback applied */
} yab_input_port_t;
static yab_input_port_t yab_input_buf[2][YAB_INPUT_PORTS];
static volatile int yab_input_cur = -1;   /* index of the last COMPLETE snapshot */

/* Frontend thread: poll + snapshot. */
static void yab_input_snapshot(void)
{
   int cur = yab_input_cur;
   int next = (cur == 0) ? 1 : 0;
   unsigned i;

   if (input_poll_cb) input_poll_cb();

   for (i = 0; i < YAB_INPUT_PORTS; i++)
   {
      yab_input_port_t *p = &yab_input_buf[next][i];
      u16 mask = 0;
      int id;

      p->analog[0] = p->analog[1] = p->analog[2] = p->analog[3] = 0;
      p->trig_l2 = p->trig_r2 = 0;

      if (i >= players || !input_state_cb)
      {
         p->joypad = 0;
         continue;
      }

      if (libretro_supports_bitmasks)
         mask = (u16)input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      else
         for (id = 0; id < 16; id++)
            if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, (unsigned)id))
               mask |= (u16)(1u << id);
      p->joypad = mask;

      if (pad_type[i] == RETRO_DEVICE_ANALOG)
      {
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
         if (l2 == 0) l2 = (mask & (1u << RETRO_DEVICE_ID_JOYPAD_L2)) ? 0x7FFF : 0;
         if (r2 == 0) r2 = (mask & (1u << RETRO_DEVICE_ID_JOYPAD_R2)) ? 0x7FFF : 0;
         p->trig_l2 = l2;
         p->trig_r2 = r2;
      }
   }

   __atomic_store_n(&yab_input_cur, next, __ATOMIC_RELEASE);
}

/* Emulation thread: read the last complete snapshot (no libretro callbacks). */
static int input_state_cb_wrapper(unsigned port, unsigned device, unsigned index, unsigned id)
{
   const yab_input_port_t *p;
   int cur = __atomic_load_n(&yab_input_cur, __ATOMIC_ACQUIRE);

   if (cur < 0 || port >= YAB_INPUT_PORTS) return 0;
   p = &yab_input_buf[cur][port];

   if (device == RETRO_DEVICE_JOYPAD)
      return (id < 16) ? ((p->joypad >> id) & 1) : 0;

   if (device == RETRO_DEVICE_ANALOG)
   {
      if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
      {
         if (id == RETRO_DEVICE_ID_ANALOG_X) return p->analog[0];
         if (id == RETRO_DEVICE_ID_ANALOG_Y) return p->analog[1];
      }
      else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
      {
         if (id == RETRO_DEVICE_ID_ANALOG_X) return p->analog[2];
         if (id == RETRO_DEVICE_ID_ANALOG_Y) return p->analog[3];
      }
      else if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
      {
         if (id == RETRO_DEVICE_ID_JOYPAD_L2) return p->trig_l2;
         if (id == RETRO_DEVICE_ID_JOYPAD_R2) return p->trig_r2;
      }
   }
   return 0;
}

static int PERLIBRETROHandleEvents(void)
{
   unsigned i = 0;

   /* Input is read from the snapshot the frontend filled (design §7 #4): no
    * libretro callback may run on this thread. */
   if (stv_mode) {
      /* ST-V: cabinet inputs only.  Kronos uses if/else here, so the Saturn
         pad loop below must NOT run in ST-V mode (it would push PERPAD_* keys
         through the shared perkeyconfig and clear the bitmask cache the ST-V
         polling just filled). */
      for (i = 0; i < (unsigned)system_key_config_nb; i++) {
         if (input_state_cb_wrapper(system_key_config[i].player, RETRO_DEVICE_JOYPAD, 0, system_key_config[i].id))
            PerKeyDown(system_key_config[i].key);
         else
            PerKeyUp(system_key_config[i].key);
      }
      for (i = 0; i < (unsigned)current_key_config_nb; i++) {
         if (input_state_cb_wrapper(current_key_config[i].player, RETRO_DEVICE_JOYPAD, 0, current_key_config[i].id))
            PerKeyDown(current_key_config[i].key);
         else
            PerKeyUp(current_key_config[i].key);
      }
   } else {
   for(i = 0; i < players; i++)
   {
         int analog_left_x = 0;
         int analog_left_y = 0;
         int analog_right_x = 0;
         int analog_right_y = 0;
         uint16_t l_trigger, r_trigger;

         switch(pad_type[i])
         {
            case RETRO_DEVICE_ANALOG:
               analog_left_x = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);

               PerAxisValue((i << 8) + PERANALOG_AXIS1, (u8)((analog_left_x + 0x8000) >> 8));

               analog_left_y = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

               PerAxisValue((i << 8) + PERANALOG_AXIS2, (u8)((analog_left_y + 0x8000) >> 8));

               // analog triggers
               l_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2 );
               r_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2 );

               // if no analog trigger support, use digital
               if (l_trigger == 0)
                  l_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2 ) ? 0x7FFF : 0;
               if (r_trigger == 0)
                  r_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2 ) ? 0x7FFF : 0;

               PerAxisValue((i << 8) + PERANALOG_AXIS3, (u8)((r_trigger > 0 ? r_trigger + 0x8000 : 0) >> 8));
               PerAxisValue((i << 8) + PERANALOG_AXIS4, (u8)((l_trigger > 0 ? l_trigger + 0x8000 : 0) >> 8));

            case RETRO_DEVICE_JOYPAD:

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
                  PerKeyDown((i << 8) + PERPAD_UP);
               else
                  PerKeyUp((i << 8) + PERPAD_UP);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
                  PerKeyDown((i << 8) + PERPAD_DOWN);
               else
                  PerKeyUp((i << 8) + PERPAD_DOWN);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
                  PerKeyDown((i << 8) + PERPAD_LEFT);
               else
                  PerKeyUp((i << 8) + PERPAD_LEFT);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
                  PerKeyDown((i << 8) + PERPAD_RIGHT);
               else
                  PerKeyUp((i << 8) + PERPAD_RIGHT);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
                  PerKeyDown((i << 8) + PERPAD_X);
               else
                  PerKeyUp((i << 8) + PERPAD_X);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
                  PerKeyDown((i << 8) + PERPAD_A);
               else
                  PerKeyUp((i << 8) + PERPAD_A);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
                  PerKeyDown((i << 8) + PERPAD_B);
               else
                  PerKeyUp((i << 8) + PERPAD_B);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
                  PerKeyDown((i << 8) + PERPAD_Y);
               else
                  PerKeyUp((i << 8) + PERPAD_Y);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
                  PerKeyDown((i << 8) + PERPAD_LEFT_TRIGGER);
               else
                  PerKeyUp((i << 8) + PERPAD_LEFT_TRIGGER);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
                  PerKeyDown((i << 8) + PERPAD_RIGHT_TRIGGER);
               else
                  PerKeyUp((i << 8) + PERPAD_RIGHT_TRIGGER);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
                  PerKeyDown((i << 8) + PERPAD_START);
               else
                  PerKeyUp((i << 8) + PERPAD_START);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
                  PerKeyDown((i << 8) + PERPAD_Z);
               else
                  PerKeyUp((i << 8) + PERPAD_Z);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
                  PerKeyDown((i << 8) + PERPAD_C);
               else
                  PerKeyUp((i << 8) + PERPAD_C);
               break;

            default:
               break;
         }
   }
   }



   if ( YabauseExec() != 0 )
      return -1;
   return 0;
}

void PERLIBRETRODeInit(void) {}

void PERLIBRETRONothing(void) {}

u32 PERLIBRETROScan(u32 flags) { return 0;}

void PERLIBRETROKeyName(u32 key, char *name, int size) {}

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

// SNDLIBRETRO
#define SNDCORE_LIBRETRO   11
#define SAMPLERATE         44100

static u32 audio_size;
static u32 soundlen;
static u32 soundbufsize;
static s16 *sound_buf;

static int SNDLIBRETROInit(void) {
    /* yabsys.IsPal is not set yet: YabauseSetVideoFormat() runs after ScspInit()
       inside YabauseInit().  Use the format latched by retro_load_game(). */
    int vertfreq = (g_videoformat_pending == VIDEOFORMATTYPE_PAL) ? 50 : 60;
    soundlen = (SAMPLERATE * 100 + (vertfreq >> 1)) / vertfreq;
    log_cb(RETRO_LOG_INFO, "SNDLIBRETROInit: %d Hz -> scratch buffer %u samples\n", vertfreq, soundlen);
    soundbufsize = (soundlen<<2 * sizeof(s16));
    if ((sound_buf = (s16 *)malloc(soundbufsize)) == NULL)
        return -1;
    memset(sound_buf, 0, soundbufsize);
    return 0;
}

static void SNDLIBRETRODeInit(void) {
   if (sound_buf)
      free(sound_buf);
}

static int SNDLIBRETROReset(void) { return 0; }

static int SNDLIBRETROChangeVideoFormat(int vertfreq)
{
    soundlen = (SAMPLERATE * 100 + (vertfreq >> 1)) / vertfreq;
    log_cb(RETRO_LOG_INFO, "SNDLIBRETROChangeVideoFormat: %d Hz -> scratch buffer %u samples\n", vertfreq, soundlen);
    soundbufsize = (soundlen<<2 * sizeof(s16));
    if (sound_buf)
        free(sound_buf);
    if ((sound_buf = (s16 *)malloc(soundbufsize)) == NULL)
        return -1;
    memset(sound_buf, 0, soundbufsize);
    return 0;
}

static void sdlConvert32uto16s(int32_t *srcL, int32_t *srcR, int16_t *dst, size_t len)
{
   u32 i;

   for (i = 0; i < len; i++)
   {
      // Left Channel
      if (*srcL > 0x7FFF)
         *dst = 0x7FFF;
      else if (*srcL < -0x8000)
         *dst = -0x8000;
      else
         *dst = *srcL;
      srcL++;
      dst++;

      // Right Channel
      if (*srcR > 0x7FFF)
         *dst = 0x7FFF;
      else if (*srcR < -0x8000)
         *dst = -0x8000;
      else
         *dst = *srcR;
      srcR++;
      dst++;
   }
}

/* ---------------------------------------------------------------------------
 * Audio: pirry's model -- the sound thread hands its own audio to the frontend.
 *
 * pirry's wall-clock SCSP thread calls SNDCore->UpdateAudio() itself
 * (scsp.c:6364 in 1.6.2) and the SDL ring behind it blocks when full, so the
 * sound card paces the SOUND thread and the frame path never waits for audio.
 * libretro does not require the audio callback to come from the retro_run
 * thread: retro_audio_sample_batch_t (:7589-7601) says nothing about threads,
 * and the only "must be thread-safe" wording is about callbacks the FRONTEND
 * invokes (:1119-1120).  This frontend's write is mutex-protected (minarch
 * SND_batchSamples), so the same shape is available here.
 *
 * This replaces the ring + per-retro_run flush: that shape made the machine
 * wait for the frontend's blocking write (a backlog of several fields became
 * ~43 ms inside retro_run; PERF-REGRESS §10), needed a one-field cap and still
 * dropped stale audio on pause/load.  With the write moved onto the sound
 * thread, backpressure lands where pirry puts it -- on the sound thread -- and
 * the frame path never pays for audio.
 *
 * Deliberate trade-off to watch: the frontend's "audio wrote this frame" pacing
 * flag (minarch ma_audio_wrote_frame, MA_pace_frame/PACE_AUDIO) is now set from
 * this thread, i.e. asynchronously.  If the frontend loses its audio clock the
 * frame rate runs away -- measure fps before judging anything else.
 * ------------------------------------------------------------------------- */
static volatile int yab_audio_live;    /* 0 while the frontend is tearing down */
static u64 yab_ft_now(void);           /* defined next to the [FT] probe below */
/* Probe: cost of the frontend's write, counted on the sound thread. */
static u64 yab_audio_cb_ns;
static u32 yab_audio_cb_calls;
static u32 yab_audio_cb_max_ns;
static u32 yab_audio_write_max;

/* vdp2.cpp calls this on pause-resume / load / reset to drop audio that belongs
 * to the timeline being replaced.  With no ring nothing is queued: the sound
 * thread only ever holds the field it is converting, so this is a no-op and the
 * call sites stay as they are. */
void yab_audio_drop_pending(void) { }

static void SNDLIBRETROUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   static u32 logged_samples = 0;
   if (num_samples != logged_samples)
   {
      logged_samples = num_samples;
      log_cb(RETRO_LOG_INFO, "SNDLIBRETROUpdateAudio: %u samples/frame (%d Hz, queued for the frontend)\n",
             num_samples, g_videoformat_pending == VIDEOFORMATTYPE_PAL ? 50 : 60);
   }
   if (num_samples > soundbufsize / 4) num_samples = soundbufsize / 4;   /* scratch size (stereo s16) */
   sdlConvert32uto16s((int32_t*)leftchanbuffer, (int32_t*)rightchanbuffer, sound_buf, num_samples);

   /* pirry shape: hand it over right here, on the sound thread.  The frontend's
    * write blocks while its ring is full, and that blocks this thread -- not
    * retro_run (minarch SND_batchSamples; the same backpressure pirry gets from
    * SDL's ring). */
   if (yab_audio_live && audio_batch_cb && num_samples)
   {
#if YAB_TRACE_LEVEL >= 2
      /* L2 probe [AUD]: callback timing.  Costs two clock reads plus four
       * atomics per callback (~600/s), so it is compiled out at level 0/1. */
      u64 t0 = yab_ft_now();
      audio_batch_cb((const int16_t *)sound_buf, num_samples);
      {
         u64 dt = yab_ft_now() - t0;
         u32 prev;
         __atomic_add_fetch(&yab_audio_cb_ns, dt, __ATOMIC_RELAXED);
         __atomic_add_fetch(&yab_audio_cb_calls, 1, __ATOMIC_RELAXED);
         prev = __atomic_load_n(&yab_audio_cb_max_ns, __ATOMIC_RELAXED);
         if (dt > prev) __atomic_store_n(&yab_audio_cb_max_ns, dt, __ATOMIC_RELAXED);
         prev = __atomic_load_n(&yab_audio_write_max, __ATOMIC_RELAXED);
         if (num_samples > prev) __atomic_store_n(&yab_audio_write_max, num_samples, __ATOMIC_RELAXED);
      }
#else
      audio_batch_cb((const int16_t *)sound_buf, num_samples);
#endif
   }

   audio_size -= num_samples;

#if YAB_TRACE_LEVEL >= 2
   {
      static u32 calls = 0;
      if (++calls >= 600)
      {
         u64 ns = __atomic_exchange_n(&yab_audio_cb_ns, 0, __ATOMIC_RELAXED);
         u32 nc = (u32)__atomic_exchange_n(&yab_audio_cb_calls, 0, __ATOMIC_RELAXED);
         printf("[AUD] writes=%u cb_avg=%.2fms cb_max=%.2fms max_frames=%u (sound thread)\n",
                nc, nc ? (double)ns / 1e6 / nc : 0.0,
                (double)__atomic_exchange_n(&yab_audio_cb_max_ns, 0, __ATOMIC_RELAXED) / 1e6,
                (unsigned)__atomic_exchange_n(&yab_audio_write_max, 0, __ATOMIC_RELAXED));
         fflush(stdout);
         calls = 0;
      }
   }
#endif
}

static u32 SNDLIBRETROGetAudioSpace(void) { return audio_size; }

void SNDLIBRETROMuteAudio(void) {}

void SNDLIBRETROUnMuteAudio(void) {}

void SNDLIBRETROSetVolume(int volume) {}

SoundInterface_struct SNDLIBRETRO = {
    SNDCORE_LIBRETRO,
    "Libretro Sound Interface",
    SNDLIBRETROInit,
    SNDLIBRETRODeInit,
    SNDLIBRETROReset,
    SNDLIBRETROChangeVideoFormat,
    SNDLIBRETROUpdateAudio,
    SNDLIBRETROGetAudioSpace,
    SNDLIBRETROMuteAudio,
    SNDLIBRETROUnMuteAudio,
    SNDLIBRETROSetVolume
};

M68K_struct *M68KCoreList[] = {
    &M68KDummy,
#ifdef HAVE_MUSASHI
    &M68KMusashi,
#else
    &M68KC68K,
#endif
    NULL
};

SH2Interface_struct *SH2CoreList[] = {
    &SH2Interpreter,
    &SH2DebugInterpreter,
#ifdef DYNAREC_DEVMIYAX
    &SH2Dyn,
#endif
    NULL
};

PerInterface_struct *PERCoreList[] = {
    &PERDummy,
    &PERLIBRETROJoy,
    NULL
};

CDInterface *CDCoreList[] = {
    &DummyCD,
    &ISOCD,
    NULL
};

SoundInterface_struct *SNDCoreList[] = {
    &SNDDummy,
    &SNDLIBRETRO,
    NULL
};

VideoInterface_struct *VIDCoreList[] = {
    //&VIDDummy,
    &VIDOGL,
    &VIDSoft,
    NULL
};

#pragma mark Yabause Callbacks

void YuiMsg(const char *format, ...)
{
  char buf[512]; 
  va_list arglist;
  va_start( arglist, format );
  int rc = vsnprintf(buf, 512, format, arglist);
  va_end( arglist );
  log_cb(RETRO_LOG_INFO, buf);
}

void YuiErrorMsg(const char *string)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "Yabause: %s\n", string);
}

/* ============ core-side shared GL context (renderer = retro_run) ============
 * All GL is issued from the frontend's thread (retro_run: VOUT + VIN), but the
 * engine cannot be given the frontend's own context to hold: a libretro
 * frontend also draws with it (minarch-gl's menu captures the frame from its
 * own FBO -- stealing the context makes that fail with "capture FBO
 * incomplete" and then SIGSEGV).  Device-verified on this libmali:
 *   - a third party inside the frontend process can read the frontend's
 *     EGLDisplay/EGLContext and create a context SHARING with it (pbuffer),
 *   - textures are shared between the two contexts, FBOs are NOT,
 *   - so the renderer targets a core-created "mirror" FBO that attaches the
 *     FRONTEND's colour texture: what the renderer draws lands in the texture
 *     the frontend presents (zero copy, hw-render contract unchanged).
 * context_reset() creates ctx_sub, makes it current on the frontend's thread
 * BEFORE YabauseInit(), and hands the frontend's context back afterwards; the
 * engine's own GL work re-borrows it (yk_attach/yk_detach, with
 * yk_remember_front/yk_restore_front so the frontend's notion of the current
 * context stays true).  Every GL object the engine creates (its internal FBOs
 * included) therefore lives in ctx_sub, the context the renderer borrows.
 * Frame delivery: YuiSwapBuffers() runs inside the engine's Sync() on the
 * frontend's thread and only records the frame's size/format; retro_run()
 * issues video_cb() itself.
 * (Historical: until f34f0ddc a separate VDP worker thread owned ctx_sub and
 * YuiSwapBuffers() ran there.  See SYNCPATH-STATUS-2026-10-31.md.)
 * ========================================================================== */
#include <dlfcn.h>

typedef void *YK_Display; typedef void *YK_Context; typedef void *YK_Surface;
typedef void *YK_Config;  typedef unsigned int YK_Uint; typedef int YK_Int;
typedef unsigned int YK_Enum; typedef unsigned int YK_Bool;

static struct {
   void *lib;
   YK_Display dpy; YK_Context ctx_front; YK_Context ctx_sub; YK_Surface pb;
   YK_Surface draw, read;
   YK_Config cfg; YK_Int cfgid;
   YK_Uint front_fbo, front_tex, mirror_fbo;
   /* The mirror FBO must carry the same depth+stencil attachment the frontend
    * gives the core (see yk_mirror_depth): the engine clips VDP2 layers with
    * the STENCIL buffer and composites them with GL_GEQUAL against a DEPTH
    * buffer cleared to 0.0. */
   YK_Uint mirror_rb;            /* depth+stencil renderbuffer (core context) */
   int mirror_rb_w, mirror_rb_h; /* its size == the frontend colour texture's */
   int front_tex_w, front_tex_h;
   int state;                    /* 0 = untried, -1 = failed, 1 = ready */
   volatile int frame_pending, res_pending;
   int ring_log, ring_adopt_log; /* bounded ring tracing (first frames) */
   /* The frontend may rotate the hw-render FBO between ring slots (minarch-gl
    * keeps three and advances them per presented frame, the same ring RetroArch
    * gives a threaded hw-render core); when the current FBO changes, the
    * mirror's colour attachment has to follow it.  Set by yk_adopt_front_fbo()
    * when the frontend publishes a new frame, consumed by YuiGetFB() -- now
    * both on the frontend's thread (until f34f0ddc the consumer was the VDP
    * worker). */
   volatile int retarget;
   int worker_valid; pthread_t worker; pthread_t main_thread;
   /* What the frontend's thread had current before the core borrowed ctx_sub
    * (see yk_remember_front/yk_restore_front). Only ever touched on the
    * frontend's thread; ctx_sub itself is borrowed for the duration of one
    * engine GL call stack, not owned by a worker for its lifetime. */
   YK_Context prev_ctx; YK_Surface prev_draw, prev_read;
   YK_Display (*GetCurrentDisplay)(void);
   YK_Context (*GetCurrentContext)(void);
   YK_Surface (*GetCurrentSurface)(int);
   YK_Bool (*QueryContext)(YK_Display, YK_Context, YK_Int, YK_Int *);
   YK_Bool (*GetConfigs)(YK_Display, YK_Config *, YK_Int, YK_Int *);
   YK_Bool (*GetConfigAttrib)(YK_Display, YK_Config, YK_Int, YK_Int *);
   YK_Surface (*CreatePbufferSurface)(YK_Display, YK_Config, const YK_Int *);
   YK_Context (*CreateContext)(YK_Display, YK_Config, YK_Context, const YK_Int *);
   YK_Bool (*MakeCurrent)(YK_Display, YK_Surface, YK_Surface, YK_Context);
   void *(*GetProcAddress)(const char *);
   void (*GenFramebuffers)(YK_Int, YK_Uint *);
   void (*BindFramebuffer)(YK_Enum, YK_Uint);
   void (*FramebufferTexture2D)(YK_Enum, YK_Enum, YK_Enum, YK_Uint, YK_Int);
   void (*FramebufferRenderbuffer)(YK_Enum, YK_Enum, YK_Enum, YK_Uint);
   void (*GenRenderbuffers)(YK_Int, YK_Uint *);
   void (*DeleteRenderbuffers)(YK_Int, const YK_Uint *);
   void (*BindRenderbuffer)(YK_Enum, YK_Uint);
   void (*RenderbufferStorage)(YK_Enum, YK_Enum, YK_Int, YK_Int);
   void (*BindTexture)(YK_Enum, YK_Uint);
   void (*GetTexLevelParameteriv)(YK_Enum, YK_Int, YK_Enum, YK_Int *);
   YK_Enum (*CheckFramebufferStatus)(YK_Enum);
   void (*GetFramebufferAttachmentParameteriv)(YK_Enum, YK_Enum, YK_Enum, YK_Int *);
   void (*GetIntegerv)(YK_Enum, YK_Int *);
   void (*Finish)(void);
   void (*Flush)(void);
   void (*ReadPixels)(YK_Int, YK_Int, YK_Int, YK_Int, YK_Enum, YK_Enum, void *);
   YK_Uint mirror_color_tex;
   /* YAB_NOWAIT double buffering: the core renders into its own pair of colour
    * textures (textures are shared between the two EGL contexts, FBOs are NOT),
    * so the worker can draw frame N+1 while the frontend still samples frame N. */
   YK_Uint rt_tex[2], rt_fbo[2], rt_read_fbo, rt_present_fbo;
   int rt_back, rt_front, rt_w, rt_h, rt_ok;
   volatile int rt_attached;      /* this frame's renders went to an RT */
} yk;

#define YK_EGL_NONE              0x3038
#define YK_EGL_CONFIG_ID         0x3028
#define YK_EGL_WIDTH             0x3057
#define YK_EGL_HEIGHT            0x3056
#define YK_EGL_CONTEXT_CLIENT_VERSION 0x3098
#define YK_EGL_DRAW              0x3059
#define YK_EGL_READ              0x305A
#define YK_GL_FRAMEBUFFER        0x8D40
#define YK_GL_COLOR_ATTACHMENT0  0x8CE0
#define YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define YK_GL_TEXTURE            0x1702
#define YK_GL_TEXTURE_2D         0x0DE1
#define YK_GL_FRAMEBUFFER_BINDING 0x8CA6
#define YK_GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define YK_GL_RENDERBUFFER       0x8D41
#define YK_GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define YK_GL_DEPTH24_STENCIL8   0x88F0
#define YK_GL_TEXTURE_BINDING_2D 0x8069
#define YK_GL_TEXTURE_WIDTH      0x1000
#define YK_GL_TEXTURE_HEIGHT     0x1001
#define YK_GL_RGBA               0x1908
#define YK_GL_UNSIGNED_BYTE      0x1401
/* Fallback depth/stencil size when the frontend colour texture size cannot be
 * queried (see yk_query_front_size); every frontend we target hands out a
 * power-of-two FBO of at least this size for a 320x224..704x448 core. */
#define YK_MIRROR_FALLBACK_DIM   1024
/* Slack added to the engine's render area before sizing the depth/stencil
 * attachment: aspect/offset modes can shift the viewport by a few pixels, and
 * the attachment must never clip a draw. */
#define YK_MIRROR_MARGIN         32

/* Context-handoff trace: build with -DYAB_CTX_TRACE to see, on every borrow /
 * hand-back, which EGL context really ends up current on which thread.  The
 * hi-res crash is a resolution-switch (res_pending) handover, so this is the
 * cheap way to tell "engine GL ran in the frontend's context" (yk_attach
 * failed) apart from "engine GL ran in the right one". */
/* L1: context borrow / hand-back trace (a few lines per resolution or option
 * change, never per frame).  Uses the same level scheme as every other probe:
 * YAB_TRACE=1 or higher turns it on, 0 compiles it out. */
#define YK_TRACE(...) YAB_TR_L1("[CTX] " __VA_ARGS__)

#if YAB_TRACE_LEVEL >= 1
static void yk_log_impl(const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[YK] %s\n", buf);
   else
      printf("[YK] %s\n", buf);
   fflush(stdout);
}
#endif /* YAB_TRACE_LEVEL >= 1 */
/* L1: context/ring setup messages (startup + ring slot changes, < 1/s).  At
 * YAB_TRACE_LEVEL 0 the macro compiles the call (and its arguments) away. */
#define yk_log(...) YAB_TR_L1(__VA_ARGS__)

/* Frontend context current: size of the colour texture the frontend hands the
 * core.  The depth/stencil attachment of the mirror FBO has to match it -- a
 * smaller attachment shrinks the framebuffer's effective render area. */
static void yk_query_front_size(YK_Uint tex)
{
   YK_Int prev = 0, w = 0, h = 0;

   if (!tex || !yk.BindTexture || !yk.GetTexLevelParameteriv)
      return;
   if (yk.GetIntegerv)
      yk.GetIntegerv(YK_GL_TEXTURE_BINDING_2D, &prev);
   yk.BindTexture(YK_GL_TEXTURE_2D, tex);
   yk.GetTexLevelParameteriv(YK_GL_TEXTURE_2D, 0, YK_GL_TEXTURE_WIDTH, &w);
   yk.GetTexLevelParameteriv(YK_GL_TEXTURE_2D, 0, YK_GL_TEXTURE_HEIGHT, &h);
   yk.BindTexture(YK_GL_TEXTURE_2D, (YK_Uint)prev);
   if (w > 0 && h > 0)
   {
      yk.front_tex_w = w;
      yk.front_tex_h = h;
   }
}

/* Core (shared) context current: (re)create and attach the mirror FBO's
 * depth+stencil renderbuffer.
 *
 * The engine does not only use depth for the 3D-ish VDP1 work: its VDP2 layer
 * compositor clears DEPTH to 0.0 and draws every layer with glDepthFunc
 * (GL_GEQUAL / GL_GREATER) plus per-layer z, and it implements VDP2 windows by
 * writing the line/rectangle window into the STENCIL buffer (YglSetVdp2Window)
 * and testing layers against it.  A colour-only FBO silently loses both: with
 * no depth attachment every fragment passes and the layers end up composited in
 * draw order, and with no stencil attachment the window clip is a no-op -- so a
 * background layer that should be clipped to a window covers the whole screen
 * (Demon Castle Dracula X: walls and stairs vanish, the forest outside shows
 * through).  The frontend's own FBO has the attachment (glsm asks for
 * depth+stencil; minarch-gl attaches GL_DEPTH24_STENCIL8), the core-owned
 * mirror FBO needs its own -- FBOs are not shared between the two contexts. */
static void yk_mirror_depth(void)
{
   /* Size the depth/stencil to the area the engine actually renders, not to
    * the frontend's power-of-two colour texture.
    *
    * A framebuffer's dimensions are the INTERSECTION of its attachments', and
    * the engine's per-frame clear is not scissored (ygles.c YglRender():
    * glDisable(GL_SCISSOR_TEST); glClear(COLOR|DEPTH|STENCIL)), so with a
    * frontend-sized (1024x1024) depth/stencil the engine clears
    * 1024*1024*4 = 4 MB of depth+stencil every frame on top of the colour
    * clear -- and every layer fragment does a depth read-modify-write against
    * it.  In RES_NATIVE (the mode this port uses) the engine draws only
    * current_width x current_height at the origin, so a render-area-sized
    * attachment makes both clears (and the attachment traffic) 10x+ smaller at
    * 320x224 while every draw stays inside the area.  Clamped to the
    * frontend's texture: never larger than before. */
   int w, h;
   int fw = yk.front_tex_w, fh = yk.front_tex_h;

   if (current_width > 0 && current_height > 0)
   {
      w = current_width  + YK_MIRROR_MARGIN;
      h = current_height + YK_MIRROR_MARGIN;
   }
   else
   {
      /* resolution not known yet (context_reset runs before the first
       * retro_set_resolution): stay frontend-sized, shrink on the first frame */
      w = fw;
      h = fh;
   }

   if (!yk.FramebufferRenderbuffer || !yk.GenRenderbuffers ||
       !yk.RenderbufferStorage || !yk.BindRenderbuffer)
      return;
   if (fw > 0 && w > fw) w = fw;
   if (fh > 0 && h > fh) h = fh;
   if (w <= 0 || h <= 0)
   {
      w = fw > 0 ? fw : YK_MIRROR_FALLBACK_DIM;
      h = fh > 0 ? fh : YK_MIRROR_FALLBACK_DIM;
   }
   if (yk.mirror_rb && (yk.mirror_rb_w != w || yk.mirror_rb_h != h))
   {
      if (yk.DeleteRenderbuffers)
         yk.DeleteRenderbuffers(1, &yk.mirror_rb);
      yk.mirror_rb = 0;
      yk.mirror_rb_w = yk.mirror_rb_h = 0;
   }
   if (!yk.mirror_rb)
   {
      yk.GenRenderbuffers(1, &yk.mirror_rb);
      yk.BindRenderbuffer(YK_GL_RENDERBUFFER, yk.mirror_rb);
      yk.RenderbufferStorage(YK_GL_RENDERBUFFER, YK_GL_DEPTH24_STENCIL8, w, h);
      yk.BindRenderbuffer(YK_GL_RENDERBUFFER, 0);
      yk.mirror_rb_w = w;
      yk.mirror_rb_h = h;
      yk_log("mirror depth/stencil rb=%u %dx%d", (unsigned)yk.mirror_rb, w, h);
   }
   yk.FramebufferRenderbuffer(YK_GL_FRAMEBUFFER, YK_GL_DEPTH_STENCIL_ATTACHMENT,
         YK_GL_RENDERBUFFER, yk.mirror_rb);
}

/* Emulation thread, frontend context current (called from context_reset, i.e.
 * after the frontend created its FBO).  Creates the shared context, the mirror
 * FBO, and leaves the SHARED context current on this thread so that
 * YabauseInit()/VIDOGLInit() create the engine's GL objects here. */
static void yk_load(void)
{

   YK_Config cfgs[64];
   YK_Int ncfg = 0, i, prev_fbo = 0, type = -1, name = 0;
   YK_Int pbattr[5], ctxattr[5];
   YK_Enum st;

   if (yk.state)
      return;
   yk.state = -1;
   yk.main_thread = pthread_self();

   yk.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!yk.lib)
      yk.lib = dlopen("libmali.so.0", RTLD_NOW | RTLD_LOCAL);
   if (!yk.lib) { yk_log("no EGL library"); return; }

   yk.GetCurrentDisplay = dlsym(yk.lib, "eglGetCurrentDisplay");
   yk.GetCurrentContext = dlsym(yk.lib, "eglGetCurrentContext");
   yk.GetCurrentSurface = dlsym(yk.lib, "eglGetCurrentSurface");
   yk.QueryContext      = dlsym(yk.lib, "eglQueryContext");
   yk.GetConfigs        = dlsym(yk.lib, "eglGetConfigs");
   yk.GetConfigAttrib   = dlsym(yk.lib, "eglGetConfigAttrib");
   yk.CreatePbufferSurface = dlsym(yk.lib, "eglCreatePbufferSurface");
   yk.CreateContext     = dlsym(yk.lib, "eglCreateContext");
   yk.MakeCurrent       = dlsym(yk.lib, "eglMakeCurrent");
   yk.GetProcAddress    = dlsym(yk.lib, "eglGetProcAddress");
   if (!yk.GetCurrentDisplay || !yk.GetCurrentContext || !yk.GetCurrentSurface ||
       !yk.QueryContext || !yk.GetConfigs || !yk.GetConfigAttrib ||
       !yk.CreatePbufferSurface || !yk.CreateContext || !yk.MakeCurrent ||
       !yk.GetProcAddress)
   { yk_log("missing EGL entry points"); return; }

   yk.dpy       = yk.GetCurrentDisplay();
   yk.ctx_front = yk.GetCurrentContext();
   yk.draw      = yk.GetCurrentSurface(YK_EGL_DRAW);
   yk.read      = yk.GetCurrentSurface(YK_EGL_READ);
   /* The frontend's context is the one current here (yk_load() is called from
    * context_reset with the frontend's context current): seed the hand-back
    * record from it, since yk_remember_front() cannot run before the entry
    * points above exist. */
   yk.prev_ctx  = yk.ctx_front;
   yk.prev_draw = yk.draw;
   yk.prev_read = yk.read;
   if (!yk.dpy || !yk.ctx_front)
   { yk_log("frontend has no current EGL context"); return; }

   if (!yk.QueryContext(yk.dpy, yk.ctx_front, YK_EGL_CONFIG_ID, &yk.cfgid))
   { yk_log("eglQueryContext(CONFIG_ID) failed"); return; }
   if (yk.GetConfigs(yk.dpy, cfgs, 64, &ncfg))
   {
      for (i = 0; i < ncfg; i++)
      {
         YK_Int id = -1;
         if (yk.GetConfigAttrib(yk.dpy, cfgs[i], YK_EGL_CONFIG_ID, &id) && id == yk.cfgid)
            yk.cfg = cfgs[i];
      }
   }
   if (!yk.cfg) { yk_log("frontend EGLConfig not found"); return; }

   /* The frontend's colour texture: query it while ITS context is current. */
   yk.GenFramebuffers     = yk.GetProcAddress("glGenFramebuffers");
   yk.BindFramebuffer     = yk.GetProcAddress("glBindFramebuffer");
   yk.FramebufferTexture2D = yk.GetProcAddress("glFramebufferTexture2D");
   yk.FramebufferRenderbuffer = yk.GetProcAddress("glFramebufferRenderbuffer");
   yk.GenRenderbuffers    = yk.GetProcAddress("glGenRenderbuffers");
   yk.DeleteRenderbuffers = yk.GetProcAddress("glDeleteRenderbuffers");
   yk.BindRenderbuffer    = yk.GetProcAddress("glBindRenderbuffer");
   yk.RenderbufferStorage = yk.GetProcAddress("glRenderbufferStorage");
   yk.BindTexture         = yk.GetProcAddress("glBindTexture");
   yk.GetTexLevelParameteriv = yk.GetProcAddress("glGetTexLevelParameteriv");
   yk.CheckFramebufferStatus = yk.GetProcAddress("glCheckFramebufferStatus");
   yk.GetFramebufferAttachmentParameteriv =
         yk.GetProcAddress("glGetFramebufferAttachmentParameteriv");
   yk.GetIntegerv         = yk.GetProcAddress("glGetIntegerv");
   yk.Finish              = yk.GetProcAddress("glFinish");
   yk.Flush               = yk.GetProcAddress("glFlush");
   yk.ReadPixels          = yk.GetProcAddress("glReadPixels");

   yk.front_fbo = (YK_Uint)(uintptr_t)hw_render.get_current_framebuffer();
   if (yk.GetIntegerv)
      yk.GetIntegerv(YK_GL_FRAMEBUFFER_BINDING, &prev_fbo);
   if (yk.front_fbo && yk.BindFramebuffer && yk.GetFramebufferAttachmentParameteriv)
   {
      yk.BindFramebuffer(YK_GL_FRAMEBUFFER, yk.front_fbo);
      yk.GetFramebufferAttachmentParameteriv(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
            YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
      if (type == YK_GL_TEXTURE)
         yk.GetFramebufferAttachmentParameteriv(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
               YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
      yk.BindFramebuffer(YK_GL_FRAMEBUFFER, (YK_Uint)prev_fbo);
      yk.front_tex = (YK_Uint)name;
   }
   if (!yk.front_tex)
   { yk_log("frontend FBO has no colour texture -> async rendering disabled"); return; }

   pbattr[0] = YK_EGL_WIDTH;  pbattr[1] = 16;
   pbattr[2] = YK_EGL_HEIGHT; pbattr[3] = 16;
   pbattr[4] = YK_EGL_NONE;
   yk.pb = yk.CreatePbufferSurface(yk.dpy, yk.cfg, pbattr);
   if (!yk.pb) { yk_log("eglCreatePbufferSurface failed"); return; }

   /* PORT: pirry's renderer runs on the VdpProc thread, so the core needs its
    * OWN context, sharing textures with the frontend's (a context cannot be
    * current on two threads: making the frontend's current on the worker fails
    * with "already current").  Textures are shared, FBOs are not, so the mirror
    * FBO and the depth/stencil renderbuffer are created in ctx_sub below.  This
    * is the shape our tree used for the VDP worker before 0ccea5cf. */
   ctxattr[0] = YK_EGL_CONTEXT_CLIENT_VERSION; ctxattr[1] = 3;
   ctxattr[2] = YK_EGL_NONE;
   yk.ctx_sub = yk.CreateContext(yk.dpy, yk.cfg, yk.ctx_front, ctxattr);
   if (!yk.ctx_sub)
   {
      ctxattr[1] = 2;
      yk.ctx_sub = yk.CreateContext(yk.dpy, yk.cfg, yk.ctx_front, ctxattr);
   }
   if (!yk.ctx_sub) { yk_log("eglCreateContext(share) failed"); return; }

   if (!yk.MakeCurrent(yk.dpy, yk.pb, yk.pb, yk.ctx_sub))
   { yk_log("eglMakeCurrent(share) on the emulation thread failed"); return; }

   yk.GenFramebuffers(1, &yk.mirror_fbo);
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, yk.mirror_fbo);
   yk.FramebufferTexture2D(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
         YK_GL_TEXTURE_2D, yk.front_tex, 0);
   yk_query_front_size(yk.front_tex);
   yk_mirror_depth();
   st = yk.CheckFramebufferStatus ? yk.CheckFramebufferStatus(YK_GL_FRAMEBUFFER) : 0;
   yk_log("share ctx=%p pbuffer=%p front_ctx=%p cfgid=%d front_fbo=%u front_tex=%u "
          "mirror_fbo=%u depth_rb=%u status=0x%x",
          yk.ctx_sub, yk.pb, yk.ctx_front, (int)yk.cfgid, yk.front_fbo, yk.front_tex,
          yk.mirror_fbo, (unsigned)yk.mirror_rb, (unsigned)st);
   if (yk.CheckFramebufferStatus && st != YK_GL_FRAMEBUFFER_COMPLETE)
   { yk_log("mirror FBO incomplete -> async rendering disabled"); return; }

   yk.state = 1;
}

/* Make the shared context current on the calling thread (VDP thread: for its
 * lifetime; emulation thread: only while it borrows the context for engine GL
 * work, e.g. a resolution/option change). */
static void yk_remember_front(void);
static void yk_restore_front(void);

static int yk_attach(void)
{
   if (yk.state != 1)
      return 0;
   /* NB: do NOT use pthread_equal() here.  In this cross toolchain/glibc combo
    * it returns 0 for two identical pthread_t values (observed: eq=0 with
    * me==worker), which silently disables the frame-delivery path.  Compare the
    * values directly (glibc's pthread_equal is exactly this comparison). */
   if (!yk.worker_valid && (pthread_self() != yk.main_thread))
   {
      yk.worker = pthread_self();
      yk.worker_valid = 1;
   }
   /* A borrower that is not the VDP thread (i.e. the frontend's thread) must
    * remember what it is taking the thread away from, so yk_detach() can hand
    * it back instead of leaving the thread context-less. */
   if (!(yk.worker_valid && pthread_self() == yk.worker))
      yk_remember_front();
   if (yk.GetCurrentContext && yk.GetCurrentContext() == yk.ctx_sub)
   {
      YK_TRACE("attach tid=%lu: already sub", (unsigned long)pthread_self());
      return 0;                 /* already ours on this thread */
   }
   {
      YK_Bool ok = yk.MakeCurrent(yk.dpy, yk.pb, yk.pb, yk.ctx_sub);
      YK_TRACE("attach tid=%lu rc=%d now=%p sub=%p front=%p", (unsigned long)pthread_self(),
            (int)ok,
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL),
            (void *)yk.ctx_sub, (void *)yk.ctx_front);
      return ok ? 0 : -1;
   }
}

/* Remember the frontend's context (and the surfaces it has current) so it can
 * be handed back later.  Called whenever the frontend's context is the one
 * current on this thread; a no-op while the core's own context is current or
 * before yk_load() has resolved the EGL entry points. */
static void yk_remember_front(void)
{
   YK_Context cur;
   if (!yk.GetCurrentContext)
      return;
   cur = yk.GetCurrentContext();
   if (!cur || cur == yk.ctx_sub)
      return;
   yk.prev_ctx  = cur;
   yk.prev_draw = yk.GetCurrentSurface ? yk.GetCurrentSurface(YK_EGL_DRAW) : NULL;
   yk.prev_read = yk.GetCurrentSurface ? yk.GetCurrentSurface(YK_EGL_READ) : NULL;
}

/* Hand the calling thread its own context back.
 *
 * Why this matters: the frontend (SDL) keeps a PER-THREAD record of which
 * context it made current and treats "the context I would make current is
 * already the one I recorded" as a no-op.  Releasing the borrowing thread to
 * EGL_NO_CONTEXT behind SDL's back therefore leaves the frontend believing its
 * context is current while the thread really has none, so every frontend GL
 * call of that frame is silently void -- measured on device as
 * "present shader compile failed" with an uninitialised info-log one frame
 * before the retry succeeded, and in the offscreen harness (with the frontend's
 * unconditional eglMakeCurrent disabled) as a permanently context-less frontend
 * thread whose every FBO readback comes back black.  Restoring the saved
 * context keeps that record true, and keeps the frontend's FBO/texture
 * namespace (names 1/3, the same names the engine's own objects use) from ever
 * being touched by the wrong context.
 *
 * Unconditional on purpose: if this thread already holds a context that is not
 * ours there is nothing to give back; if it holds ctx_sub (the normal case) or
 * nothing at all (the VDP thread took ctx_sub over while the engine was
 * starting) the frontend's context is the right thing to have current.
 */
static void yk_restore_front(void)
{
   YK_Context cur, back;
   if (!yk.GetCurrentContext || !yk.MakeCurrent)
      return;
   cur = yk.GetCurrentContext();
   if (cur && cur != yk.ctx_sub)
      return;                   /* somebody else's context is current here */
   back = yk.prev_ctx ? yk.prev_ctx : yk.ctx_front;
   if (!back)
      return;
   {
      YK_Bool ok = yk.MakeCurrent(yk.dpy,
            yk.prev_ctx ? yk.prev_draw : yk.draw,
            yk.prev_ctx ? yk.prev_read : yk.read,
            back);
      YK_TRACE("restore-front tid=%lu cur_was=%p back=%p rc=%d now=%p",
            (unsigned long)pthread_self(), (void *)cur, (void *)back, (int)ok,
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL));
   }
}

static void yk_detach(void)
{
   if (yk.state != 1)
      return;
   if (yk.worker_valid && pthread_self() == yk.worker)
   {
      if (yk.GetCurrentContext && yk.GetCurrentContext() != yk.ctx_sub)
         return;                /* not ours on this thread */
      yk.MakeCurrent(yk.dpy, NULL, NULL, NULL);   /* VDP thread: plain release */
      YK_TRACE("detach[worker] tid=%lu -> released (now=%p)",
            (unsigned long)pthread_self(),
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL));
      return;
   }
   /* Frontend thread: hand back the context this thread owns (no-op when it is
    * already current, which is also what makes a failed yk_attach() harmless). */
   if (yk.main_thread && pthread_self() != yk.main_thread)
   {
      /* NEITHER the render thread NOR the frontend thread: this is the engine's
       * machine thread calling YuiRevokeOGLOnThisThread() -- vdp2.cpp does that
       * once, at the top of Vdp2VBlankIN() when it starts VdpProc.  The
       * standalone's hook is SDL_GL_MakeCurrent(wnd, nullptr), i.e. RELEASE.
       * yk_restore_front() instead binds the FRONTEND's context on the calling
       * thread, which is wrong here twice over: (a) that context is normally
       * already current on the frontend's thread, so EGL fails and we leave the
       * machine thread in an arbitrary state; (b) if it were not, the frontend's
       * context would become current on the machine thread, and SDL's per-thread
       * make-current bookkeeping would then make every later frontend GL call a
       * silent no-op (see yk_restore_front's comment).  Release instead, exactly
       * like the standalone. */
      yk.MakeCurrent(yk.dpy, NULL, NULL, NULL);
      YK_TRACE("detach[other] tid=%lu -> released (now=%p)",
            (unsigned long)pthread_self(),
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL));
      return;
   }
   yk_restore_front();
}

/* Does the calling thread currently hold the shared context? */
static int yk_held(void)
{
   return yk.state == 1 && yk.GetCurrentContext && yk.GetCurrentContext() == yk.ctx_sub;
}


static int first_ctx_reset = 1;

/* TODO(2026-10-31): dead in the libretro build -- nothing calls either of these
 * (the whole tree only defines them; the platform ports have their own).  They
 * are the hook pirry's standalone frontend uses to hand its single context
 * between threads, and the hook the deleted VDP worker used to take the core's
 * shared context.  Kept because a restored render thread (or a standalone
 * frontend) would call them again; see SYNCPATH-STATUS-2026-10-31.md. */
int YuiUseOGLOnThisThread()
{
  /* Async renderer: the renderer borrows the core-owned shared context.
   * Sync renderer: nothing to borrow (the engine draws with the frontend's
   * context on the frontend's thread), keep the old glsm bind for parity. */
  if (g_vdp_async)
    return yk_attach();
#if !defined(_USEGLEW_)
  return glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
#else
  return 0;
#endif
}

int YuiRevokeOGLOnThisThread()
{
  if (g_vdp_async)
  {
    yk_detach();
    return 0;
  }
#if !defined(_USEGLEW_)
  return glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
#else
  return 0;
#endif
}

/* ---------------------------------------------------------------------------
 * Frontend hw-render FBO rotation (YAB_CORE_SHARED_CONTEXT).
 *
 * minarch-gl hands the core a ring of hw-render FBOs (three slots) and advances
 * the slot once per presented frame -- the same ring RetroArch gives a
 * hw-render core that renders on its own thread (gfx/video_thread_hw.c: the
 * ring "gives them what the swapchain gave them unthreaded").  The core renders
 * into slot N while the frontend still samples slot N-1, so the frame the
 * frontend presents is never the frame the VDP thread is drawing.
 *
 * Our engine draws into the core-owned MIRROR FBO (FBOs are not shared between
 * contexts on this driver, see the notes above), so following the rotation is
 * one glFramebufferTexture2D: re-point the mirror's colour attachment at the
 * new slot's texture.  Runs on the frontend's thread with the FRONTEND's
 * context current (retro_run, right after video_cb), which is where the
 * frontend's FBO and its attachment can be queried.
 *
 * The attachment is re-read EVERY frame, not only when the FBO name changes:
 * a frontend that recreates its hw-render FBO (resolution/geometry change,
 * MA_GL_update_fbo_size) can get the same FBO name back from the driver and
 * would otherwise leave the mirror pointing at a deleted texture.
 * ------------------------------------------------------------------------- */
/* Diagnostic only: read back what the ENGINE has just rendered, i.e. the mirror
 * FBO the VDP thread draws into, and write it as a PPM on the sdcard.  The
 * frontend's own readback cannot be trusted here (it reported nonblack=0 for
 * cotton2 even when the game was clearly displaying something), so this is the
 * only way to tell "the engine is producing black frames" (an emulation/VDP2
 * problem) from "the engine is fine, the present path hands over an empty slot".
 *   YAB_MIRROR_DUMP=<from>-<to>[:step]   e.g. 600-900:20
 */

static void yk_adopt_front_fbo(void)
{
   YK_Uint fbo, name = 0;
   YK_Int  type = 0, prev_fbo = 0;

   if (!g_vdp_async || yk.state != 1 || !hw_render.get_current_framebuffer)
      return;
   fbo = (YK_Uint)(uintptr_t)hw_render.get_current_framebuffer();
   if (!fbo)
      return;
   if (!yk.BindFramebuffer || !yk.GetFramebufferAttachmentParameteriv)
      return;
   if (yk.GetIntegerv)
      yk.GetIntegerv(YK_GL_FRAMEBUFFER_BINDING, &prev_fbo);
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, fbo);
   yk.GetFramebufferAttachmentParameteriv(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
         YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
   if (type == YK_GL_TEXTURE)
      yk.GetFramebufferAttachmentParameteriv(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
            YK_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, (YK_Uint)prev_fbo);
   if (name && name != yk.front_tex)
   {
      yk.front_fbo = fbo;
      yk.front_tex = name;
      yk.retarget  = 1;
      yk_query_front_size((YK_Uint)name);
      if (yk.ring_adopt_log < 4)   /* one-time confirmation that the ring is followed */
      {
         YAB_TR_L1("[YK] ring adopt: front fbo=%u tex=%u\n",
                   (unsigned)fbo, (unsigned)name);
         yk.ring_adopt_log++;
      }
   }
}

/* Applied on the VDP thread, at the top of YglRender(): the engine binds the
 * mirror FBO (its cached _Ygl->default_fbo == yk.mirror_fbo) for the whole
 * frame, so following the frontend's ring is one attachment change here.
 *
 * RetroArch's rule for the same hand-over (gfx/drivers/gl3.c: the hw-render
 * ring attaches hw_render_rb_ds -- GL_DEPTH24_STENCIL8 when the core asked for
 * stencil -- to EVERY ring slot, "sharing the depth renderbuffer, which only
 * the core's rendering touches") is that the FBO handed to the core always
 * carries the depth/stencil the core asked for in retro_hw_render_callback.
 * RetroArch's driver owns that FBO and re-establishes it; here the core owns
 * the mirror, so the core has to RESTORE the attachments itself whenever the
 * frontend hands the frame back.  Done unconditionally, once per frame: three
 * cheap GL calls, and it also covers a ring slot whose colour texture the
 * frontend replaced without changing the FBO name. */
/* ---------------------------------------------------------------------------
 * YAB_NOWAIT render targets
 *
 * Decoupling presentation from rendering means the renderer must not draw into
 * the slot the frontend is presenting, and the frontend's ring only ever hands
 * out ONE slot at a time.  So the core owns two colour textures (shared between
 * the EGL contexts) and alternates them: the worker draws into rt_tex[rt_back],
 * flips to it when the frame is complete, and the frontend thread blits the
 * finished one into the slot the frontend is about to show.
 * ------------------------------------------------------------------------- */
/* The worker recreates these textures on a resolution change while the
 * frontend thread may be blitting the previous ones -- deleting a texture in
 * flight presents black.  The sections are tiny (a few GL calls, one blit), so a
 * plain mutex is enough. */
/* Spinlock: the critical sections are a few GL calls, and the case only happens
 * on a resolution change.  (pthread.h cannot be included here: the port's own
 * rthreads layer redefines part of it.) */
static volatile int yk_rt_mtx = 0;
static void yk_rt_lock(void) { while (__atomic_exchange_n(&yk_rt_mtx, 1, __ATOMIC_ACQUIRE)) { } }
static void yk_rt_unlock(void) { __atomic_store_n(&yk_rt_mtx, 0, __ATOMIC_RELEASE); }

static int yk_rt_ensure(int w, int h)
{
   if (!yk.GenFramebuffers || !yk.BindFramebuffer || !yk.FramebufferTexture2D)
      return -1;
   if (w <= 0 || h <= 0) return -1;
   /* Never shrink and never recreate: the resolution wobbles between modes (e.g.
    * 320x240 <-> 352x224) and recreating the textures mid-stream leaves the
    * frontend's read FBO pointing at a deleted id -- measured: whole runs
    * presenting black.  Allocate once, grow only. */
   if (yk.rt_ok && w <= yk.rt_w && h <= yk.rt_h) return 0;
   yk_rt_lock();
   if (yk.rt_ok && w <= yk.rt_w && h <= yk.rt_h) { yk_rt_unlock(); return 0; }
   for (int i = 0; i < 2; i++)
   {
      if (!yk.rt_tex[i]) glGenTextures(1, &yk.rt_tex[i]);
      glBindTexture(GL_TEXTURE_2D, yk.rt_tex[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      if (!yk.rt_fbo[i]) yk.GenFramebuffers(1, &yk.rt_fbo[i]);
      yk.BindFramebuffer(YK_GL_FRAMEBUFFER, yk.rt_fbo[i]);
      yk.FramebufferTexture2D(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
            YK_GL_TEXTURE_2D, yk.rt_tex[i], 0);
      if (yk.mirror_rb && yk.FramebufferRenderbuffer)
      {
         yk.FramebufferRenderbuffer(YK_GL_FRAMEBUFFER, YK_GL_DEPTH_STENCIL_ATTACHMENT,
               YK_GL_RENDERBUFFER, yk.mirror_rb);
      }
   }
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, 0);
   if (w > yk.rt_w) yk.rt_w = w;
   if (h > yk.rt_h) yk.rt_h = h;
   if (!yk.rt_ok) { yk.rt_back = 0; yk.rt_front = 1; }
   yk.rt_ok = 1;
   yk_rt_unlock();
   return 0;
}

static YK_Uint yk_rt_acquire(int w, int h)
{
   if (yk_rt_ensure(w, h) != 0) return 0;
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, yk.rt_fbo[yk.rt_back]);
   if (yk.mirror_rb && yk.FramebufferRenderbuffer)
   {
      yk.FramebufferRenderbuffer(YK_GL_FRAMEBUFFER, YK_GL_DEPTH_STENCIL_ATTACHMENT,
            YK_GL_RENDERBUFFER, yk.mirror_rb);
   }
   return yk.rt_fbo[yk.rt_back];
}

static void yk_rt_flip(void)
{
   yk_rt_lock();
   if (yk.rt_ok) {
      yk.rt_front = yk.rt_back;
      yk.rt_back ^= 1;
   }
   yk_rt_unlock();
}

/* Worker side: copy the finished render target into the frontend's colour
 * texture.  Textures are shared across the two contexts, so the core can do this
 * with its own FBO; doing it here (after the composite and its glFinish) means
 * the frontend thread only has to present what is already in its FBO. */
static void yk_rt_blit_to_front(int w, int h)
{
   if (!yk.rt_ok || !yk.front_tex) return;
   /* The render targets only ever grow, so after a resolution SHRINK they are
    * larger than the picture: copy exactly the area the engine renders
    * (current_width x current_height), never the whole target -- blitting the
    * full target scaled the picture up by 352/320 and shifted every pixel
    * (measured: 75% of the pixels differing from the baseline at the same
    * machine frame). */
   if (w > yk.rt_w) w = yk.rt_w;
   if (h > yk.rt_h) h = yk.rt_h;
   yk_rt_lock();
   if (!yk.rt_present_fbo) yk.GenFramebuffers(1, &yk.rt_present_fbo);
   yk.BindFramebuffer(GL_READ_FRAMEBUFFER, yk.rt_fbo[yk.rt_front]);
   yk.BindFramebuffer(GL_DRAW_FRAMEBUFFER, yk.rt_present_fbo);
   yk.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
         YK_GL_TEXTURE_2D, yk.front_tex, 0);
   glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   yk.BindFramebuffer(YK_GL_FRAMEBUFFER, 0);
   yk_rt_unlock();

}

static void yk_rt_present_to(unsigned dst_fbo, int w, int h)
{
   yk_rt_lock();
   if (!yk.rt_ok || !yk.rt_tex[yk.rt_front]) { yk_rt_unlock(); return; }
   if (!yk.rt_read_fbo) yk.GenFramebuffers(1, &yk.rt_read_fbo);
   yk.BindFramebuffer(GL_READ_FRAMEBUFFER, yk.rt_read_fbo);
   yk.FramebufferTexture2D(GL_READ_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
         YK_GL_TEXTURE_2D, yk.rt_tex[yk.rt_front], 0);
   yk.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
   glBlitFramebuffer(0, 0, yk.rt_w, yk.rt_h, 0, 0, w, h,
         GL_COLOR_BUFFER_BIT, GL_NEAREST);
   yk.BindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
   yk_rt_unlock();
}


void YuiRetargetFB(void)
{
   if (g_vdp_async && yk.state == 1 && yk.front_tex)
   {
      yk.BindFramebuffer(YK_GL_FRAMEBUFFER, yk.mirror_fbo);
      /* Zero copy: draw straight into the frontend's colour texture.  With the
       * flycast-style split (machine on its own thread, all GL and presentation
       * on the frontend thread) nothing else can be drawing from, or rotating,
       * the frontend's ring while this frame is being rendered, so the
       * double-buffered render target and the present-time blit are not needed
       * -- and while they were still in the path the picture came out black. */
      yk.FramebufferTexture2D(YK_GL_FRAMEBUFFER, YK_GL_COLOR_ATTACHMENT0,
            YK_GL_TEXTURE_2D, yk.front_tex, 0);
      yk_mirror_depth();
      if (yk.retarget && yk.ring_log < 24)
      {
         YAB_TR_L1("[YK] ring draw: mirror=%u <- front fbo=%u tex=%u\n",
                   (unsigned)yk.mirror_fbo, (unsigned)yk.front_fbo, (unsigned)yk.front_tex);
         yk.ring_log++;
      }
      yk.retarget = 0;
   }
}

int YuiGetFB(void)
{
  /* async: the engine renders into the core's mirror FBO, which attaches the
   * frontend's colour texture (re-pointed per frame by YuiRetargetFB).
   * sync: the engine renders straight into the frontend's hw-render FBO. */

  /* Draw through the core's mirror FBO: it is the frontend's colour texture
   * plus our own depth/stencil attachments, which VDP2's compositing needs
   * (stencil windows, priority).  The cross-thread reason for the mirror is
   * gone, but the attachment reason is not -- without it some titles render a
   * flat background only.  Same context, same thread, so this is still one GL
   * thread. */
  if (yk.mirror_fbo)
     return (int)yk.mirror_fbo;
  return hw_render.get_current_framebuffer();
}

/* what the engine actually renders (ygles.c globals); see YuiSwapBuffers */
extern int GlWidth, GlHeight;

/* libretro contract: RETRO_ENVIRONMENT_SET_GEOMETRY "can only be called from
 * within retro_run()" (libretro.h, RETRO_ENVIRONMENT_SET_GEOMETRY).  The
 * geometry also changes outside retro_run -- context_reset (video init) and
 * retro_unserialize (a save state with another Saturn mode) -- and the frontend
 * re-reads retro_get_system_av_info() at both points anyway, so publishing from
 * there is not just unnecessary, it is out of contract. */
static int yab_in_run = 0;

void retro_reinit_av_info(void)
{
    struct retro_system_av_info av_info;
    retro_get_system_av_info(&av_info);
    if (yab_in_run)
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

void retro_set_resolution()
{
   // If resolution_mode > initial_resolution_mode, we'll need a restart to reallocate the max size for buffer
   if (resolution_mode > initial_resolution_mode)
   {
      log_cb(RETRO_LOG_INFO, "Restart the core for x%d resolution\n", resolution_mode);
      resolution_mode = initial_resolution_mode;
   }
   // Downscale resolution_mode for Hi-Res games
   if (game_height > 256 && resolution_mode > max_resolution_mode/2)
   {
      log_cb(RETRO_LOG_INFO, "Halving Hi-Res games resolution mode\n", resolution_mode);
      resolution_mode = max_resolution_mode/2;
   }
   switch(resolution_mode)
   {
      case 1:
         /* "original" -> RES_NATIVE (0): draw straight into the framebuffer the
          * frontend gave us (YuiGetFB(), i.e. the mirror FBO over its colour
          * texture).  RES_ORIGINAL (3) instead renders into a game-sized
          * sub-render-target and blits/upscales it every frame: on a hi-res game
          * (e.g. Virtua Fighter 2 at 704x480) that costs an extra target + blit
          * per frame, and the mode switch on the BIOS->game handover reallocates
          * those targets mid-stream.  Aligned with lr-yabasanshiro's libretro
          * frontend, which picks native for the same reason (its comment: the
          * upscale path is broken under the libretro HW context). */
         g_resolution_mode = RES_NATIVE;
         break;
      case 2:
         g_resolution_mode = 2;
         break;
      case 4:
         g_resolution_mode = 1;
         break;
   }
   if (current_width != game_width * resolution_mode ||
       current_height != game_height * resolution_mode)
   {
      current_width = game_width * resolution_mode;
      current_height = game_height * resolution_mode;
      /* Resize(originx, originy, w, h, on, aspect_rate_mode).  `on` is the
       * "fullscreen" flag, and the aspect mode decides whether the ENGINE letterboxes
       * inside the surface it was given:
       *
       *   on=0 + ORIGINAL -> SetSaturnResolution() runs the 4:3 fit
       *                      (GlHeight = GlWidth*3/4, and originy = -20 for a
       *                      352x224 request), so a 352x224 mode was rendered as
       *                      352x264 and reported to the frontend that way.
       *   on=1 + FULL     -> the fit block is skipped entirely: GlWidth/GlHeight are
       *                      exactly the frame size and origin is 0.
       *
       * A libretro core renders ITS OWN frame and hands it over; fitting that frame
       * into a window is the frontend's job (it scales the reported frame itself).
       * Letting the engine fit it made the core's render size depend on display
       * aspect and fed the result back into game_height (see YuiSwapBuffers), which
       * is how a 352x224 mode ended up rendering 264 lines -- a 1.18x vertical
       * resample that no Saturn mode has.  FULL keeps the core at
       * Saturn-mode x resolution_mode, independent of anything the frontend does. */
      VIDCore->Resize(0, 0, current_width, current_height, 1, FULL);
      YAB_TR_L1("[res] native=%dx%d mode=%d -> frame=%dx%d engineGl=%dx%d\n",
                game_width, game_height, resolution_mode, current_width, current_height,
                GlWidth, GlHeight);
   }
   retro_reinit_av_info();
   VIDCore->SetSettingValue(VDP_SETTING_RESOLUTION_MODE, g_resolution_mode);
}

/* Called by VIDOGLVdp2SetResolution() (vidogl.c) on the VdpProc thread, right
 * before SetSaturnResolution() runs for the TVMD mode the next field will use.
 *
 * Why it has to happen here: in the standalone the engine's "window" is a fixed
 * 720x480 SDL window, so SetSaturnResolution() always derives GlWidth/GlHeight
 * from a constant and a mid-game mode change is correct in the very field that
 * changes it.  Here the window size IS the frontend frame size (Saturn mode x
 * scale factor), and SetSaturnResolution() copies it out of
 * _Ygl->screen_width/height -- which would still be the previous mode's value,
 * so that one field would be rendered with the old viewport (a 352x224 ->
 * 704x480 switch drew the picture at quarter size for 1/60 s).  Updating the
 * size here, before the field is drawn, makes the switch atomic like the
 * standalone.
 *
 * Safe on this thread: VIDOGLResize() is CPU-only (YglTMRequestFlush() sets a
 * flag, YglGLInit() sets rebuild_frame_buffer, SetSaturnResolution() recomputes
 * GlWidth/GlHeight and the VDP1 target size); the GL objects are rebuilt later
 * by YglGenFrameBuffer() during this same field, on this same thread. */
void YabVdp2ModeChanged(int native_w, int native_h)
{
   int fw, fh;
   if (native_w <= 0 || native_h <= 0 || resolution_mode < 1)
      return;
   fw = native_w * resolution_mode;
   fh = native_h * resolution_mode;
   if (fw == current_width && fh == current_height)
      return;
   VIDCore->Resize(0, 0, fw, fh, 1, FULL);
   current_width  = fw;
   current_height = fh;
   YAB_TR_L1("[res] mode change -> frame=%dx%d engineGl=%dx%d\n",
             current_width, current_height, GlWidth, GlHeight);
}

/* what the engine actually renders (ygles.c globals); see YuiSwapBuffers */

void YuiSwapBuffers(void)
{
   int prev_game_width = game_width;
   int prev_game_height = game_height;
   VIDCore->GetNativeResolution(&game_width, &game_height, &game_interlace);
   /* Some Saturn modes are DOUBLE HEIGHT: the engine renders 704x448 while
    * GetNativeResolution reports the field height (224) with interlace=0, so the
    * frontend was handed 704x224 out of a 704x448 render -- half the picture
    * (Virtua Fighter's attract mode lost the fighters' legs and the floor).
    * ygles.c's GlWidth/GlHeight are what the engine actually renders; take the
    * larger of the two.  No effect when they agree (BIOS 320x224, VF2's
    * 704x448, 352x224 games). */
   /* DOUBLE HEIGHT only: the case this was added for renders *twice* the field
    * height (704x448 while GetNativeResolution reports 224).  A +22 line wobble
    * is not that, and promoting it turned one switch into two full
    * VdpRevoke/Resize/mirror-rebuild round trips.  YAB_HEIGHT_PROMOTE=any
    * restores the old behaviour. */
   /* Always take the engine's render height.  An earlier version of this tried
    * to ignore a "+22 line wobble" (352x224 -> 352x246 -> 352x224) as spurious,
    * but that switch is part of the normal VDP2 mode change: with it suppressed
    * cotton2's picture never comes back after the switch (verified on the
    * device: old core reaches 352x246 and back and renders 69196 non-black
    * pixels; the gated core produces no [res] events at all and stays black). */
   /* Take the engine's render size as the NATIVE size only when it is larger
    * than what GetNativeResolution reported.  It must be divided by
    * resolution_mode first: GlWidth/GlHeight are the scaled frame, so comparing
    * them raw against the native size promoted 224 -> 448 at x2, which fed back
    * into retro_set_resolution() and doubled the frame again (x2 ended up
    * rendering 528 lines for a 224-line mode before the hi-res clamp caught it).
    * With Resize(..., on=1, FULL) the engine no longer aspect-fits, so in the
    * normal case GlHeight == game_height*resolution_mode and this never fires. */
   if (resolution_mode > 0)
   {
      if (GlHeight / resolution_mode > game_height) game_height = GlHeight / resolution_mode;
      if (GlWidth  / resolution_mode > game_width)  game_width  = GlWidth  / resolution_mode;
   }
   if ((prev_game_width != game_width) || (prev_game_height != game_height))
   {
      if (g_vdp_async)
      {
         /* VIDOGLResize() runs YglGLInit(), i.e. a full GL re-init (FBOs,
          * textures, shaders).  We are inside YglRender's call stack here, so
          * doing it now tears down objects the ongoing render is still using --
          * measured on cotton2: the frontend thread never came back after the
          * 352x224 -> 352x246 switch.  Defer it to retro_run, outside the
          * render, with the machine parked. */
         yk.res_pending = 1;
      }
      else
         retro_set_resolution();
   }
   /* Audio space is granted once per field in retro_run(), not here: a
    * frameskip field renders no picture but must not starve the sound. */
   /* There is no GL worker any more: with the machine on its own thread and all
    * GL on the frontend thread, this function always runs on the thread that
    * must present, so video_cb is called unconditionally.  The old early-return
    * ("mark the frame and let retro_run deliver it") belongs to the worker
    * model; left in place it silently stopped presenting after a resolution
    * change on cotton2/shienryu/seabass -- frames kept coming, no video_cb. */
   /* PORT: this runs on the VdpProc render thread (ygles.c's YglRender calls it),
    * so it must NOT call video_cb -- libretro requires that on the frontend
    * thread.  Count the picture; retro_run presents exactly one per count. */
   {
      /* PORT: the standalone's YuiSwapBuffers() is SDL_GL_SwapWindow().  With ONE
       * context that gives two things for free: the frame is submitted, and the
       * frontend's own draw (its blit/scaling) is ordered after it by the
       * driver.  Here the engine renders in ctx_sub while the frontend samples
       * the shared colour texture from its OWN context, so neither is free:
       *
       *  - the engine only flushes at the end of YglRenderVDP1(), i.e. BEFORE the
       *    composition (ygles.c:3933); nothing submits the finished frame, and
       *  - Mali is a tile-based renderer: two contexts writing/reading the same
       *    texture with no sync can have their tile batches interleaved, so the
       *    frontend can sample a frame that is half new and half old -- a wrong
       *    frame, which a global post-process (shader, scaling) can never cause.
       *
       * glFinish() is the cross-context equivalent of what the swap guarantees:
       * the frame is complete before the ack below lets the frontend sample it.
       * (Cheaper later: a GLsync fence here + the frontend waiting on it; the
       * alignment comes first, performance later.) */
      if (yk.Finish)
         yk.Finish();
      unsigned me = __atomic_add_fetch(&yab_swap_count, 1, __ATOMIC_RELEASE);
      one_frame_rendered = true;
      /* PORT, aligned with the standalone: there YuiSwapBuffers() is
       * SDL_GL_SwapWindow(), i.e. the render thread BLOCKS until the display has
       * the frame, and the machine waits for that VIN token before the next
       * field.  Here the frontend presents in retro_run, so wait for its ack
       * (bounded, so a paused frontend cannot wedge the machine).  Without this
       * the worker never waits for the present, the machine runs ahead and VDP1
       * is walked ~3x more often per second than the standalone in heavy
       * scenes -- which is where the picture differences came from. */
      {
         int spins = 0;
         while (__atomic_load_n(&yab_present_ack, __ATOMIC_ACQUIRE) < me && ++spins < 200)
            usleep(500);
      }
   }
}

/* ---------------------------------------------------------------------------
 * Frontend-visible memory: backup RAM (.srm / RetroAchievements) and the work
 * RAM layout.  Ported from lr-yabasanshiro src/libretro/libretro.c:1700-1793.
 *
 * The frontend may write its .srm into the buffer returned by
 * retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) before the core's own BupRam
 * exists, so expose a shadow buffer, seed it from the core's backup.bin, push it
 * into BupRam once that exists, then mirror it back out every frame.  The core
 * keeps writing its own backup.bin as well, so saves persist either way.
 * ------------------------------------------------------------------------- */
#define YABA_INTERNAL_BUP_SIZE 0x10000

static u8  sram_shadow[YABA_INTERNAL_BUP_SIZE];
static int sram_apply_pending = 0; /* push shadow -> BupRam once BupRam exists */

static void sram_seed_from_backup(const char *path)
{
   FILE *bf = fopen(path, "rb");
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
   /* Which card file is in use, and whether it was found: Saturn shares one
      internal card, an ST-V board has a per-game SRAM (see the bup_path setup). */
   log_cb(RETRO_LOG_INFO, "Backup RAM: %s (%s)\n", path,
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

/* Publish the Saturn work-RAM layout so the frontend (RetroAchievements, cheat
 * search, ...) can address it.  Called from context_reset() once YabauseInit()
 * has allocated the buffers. */
static void set_memory_maps(void)
{
   static struct retro_memory_descriptor descs[2];
   struct retro_memory_map mmap;

   if (!HighWram || !LowWram)
      return;

   memset(descs, 0, sizeof(descs));

   /* High Work RAM -- HWRAM @ 0x06000000, 1 MB (the primary region). */
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
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
}

static void context_reset(void)
{
   /* Async renderer only: create the core's own (shared) context and leave it
    * current on this thread, so YabauseInit()'s GL objects live in the context
    * the VDP thread uses.  Must happen before YabauseInit() starts that thread.
    * yk_remember_front() first, and yk_restore_front() at the end: the engine's GL
    * work below is the only part of this call that must run in the core's
    * context -- handing the frontend's context back afterwards is what keeps
    * the frontend thread's notion of "which context is current" true (see
    * yk_restore_front).  yk_attach() re-borrows ctx_sub on a repeat reset,
    * where yk_load() is a no-op. */
   /* If the core's context is already current on this thread, an OUTER borrow
    * (retro_run's option/res_pending bracket, or apply_state_buffer) is in
    * progress and owns the hand-back -- this re-entry (MA_GL_update_fbo_size ->
    * context_reset) must not hand the frontend's context back early. */
   int outer_borrow = 0;
   if (g_vdp_async)
   {
      outer_borrow = yk_held();
      YK_TRACE("context_reset enter outer_borrow=%d now=%p", outer_borrow,
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL));
      if (!outer_borrow)
         yk_remember_front();
      yk_load();
      yk_attach();
   }
#if !defined(_USEGLEW_)
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);
   glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);
#endif
#if defined(_OGLES3_)
   /* glsym_es3.c's symbol table has no ES 3.1 entries, so the rglgen pass above
    * leaves glBindImageTexture()/glDispatchCompute()/glMemoryBarrier() NULL and
    * the first game that takes RBGGenerator's compute path (e.g. Virtua
    * Fighter 2's title screen) jumps to NULL: si_code=SEGV_MAPERR, si_addr=0,
    * PC=0.  Resolve them here, and if the driver cannot provide them, keep the
    * engine on the fragment-shader path instead of calling NULL. */
   glsym_private_resolve_es31((void *(*)(const char *))hw_render.get_proc_address);
   log_cb(RETRO_LOG_INFO,
          "ES 3.1 entry points: glBindImageTexture=%p glDispatchCompute=%p glMemoryBarrier=%p",
          (void *)__rglgen_glBindImageTexture, (void *)__rglgen_glDispatchCompute,
          (void *)__rglgen_glMemoryBarrier);
   if (!__rglgen_glBindImageTexture || !__rglgen_glDispatchCompute || !__rglgen_glMemoryBarrier)
   {
      log_cb(RETRO_LOG_WARN,
             "RBG compute shader unavailable (missing ES 3.1 image/compute entry points): using the fragment-shader path");
      g_rbg_use_compute_shader = 0;
      if (VIDCore)
         VIDCore->SetSettingValue(VDP_SETTING_RBG_USE_COMPUTESHADER, g_rbg_use_compute_shader);
   }
#endif
   if (first_ctx_reset == 1)
   {
      first_ctx_reset = 0;
      /* PORT, aligned with the standalone: retro_arena sets g_texcache_persist from
       * the "texcache persist" preference (default true, and the device's Daytona
       * config has it true -- see PORT-TO-PIRRY §30/§47).  Our libretro glue never
       * set it, so the core ran with the texture cache OFF while the standalone
       * runs with it ON: without it VIDOGLVdp1DrawStart does YglTMReset +
       * YglCacheReset every frame, i.e. every tile is re-decoded instead of being
       * validated against the VRAM page stamps.  YAB_TEXCACHE=0 keeps the A/B
       * switch.  (This hunk was lost once when libretro.c was restored from the
       * index during the st1 audit -- keep it here, it is not optional.) */
      {
         extern int g_texcache_persist;
         const char *e = getenv("YAB_TEXCACHE");
         g_texcache_persist = (e && *e) ? (atoi(e) ? 1 : 0) : 1;
         log_cb(RETRO_LOG_INFO, "texture cache persist = %d\n", g_texcache_persist);
      }
      YabauseInit(&yinit);
      log_cb(RETRO_LOG_INFO, "Video format after YabauseInit: engine IsPal=%d (pending=%d)\n",
             yabsys.IsPal, g_videoformat_pending);
      renderer_running = true;
      retro_set_resolution();
      /* PORT, aligned with the standalone (retro_arena/main.cpp:452-456):
       *
       *     extern int g_perline_requested;
       *     g_perline_requested = 1;
       *     YabauseSetDecilineMode(1);
       *
       * vdp2.cpp only switches the SH-2 loop from the per-deciline path to the
       * whole-line path once the game has been drawing for 3 seconds ("enabling it
       * during boot can do damage that is not reversible"), and it gates that on
       * g_perline_requested.  Our glue never set the flag, so this core stayed on
       * the 10x finer per-deciline loop for the whole session while the reference
       * switches to the whole-line path -- a different execution granularity (and
       * 10x the loop iterations).  It has to come AFTER YabauseInit(), which resets
       * the deciline mode. */
      {
         extern int g_perline_requested;
         g_perline_requested = 1;
         YabauseSetDecilineMode(1);
      }
      OSDChangeCore(OSDCORE_DUMMY);
      set_memory_maps();   /* HighWram/LowWram exist only after YabauseInit() */
   }
   else
   {
      if (!renderer_running)
         VIDCore->Init();
      renderer_running = true;
      retro_set_resolution();
      set_memory_maps();
   }
   /* Engine is up: give the frontend's thread its own context back (unless an
    * outer borrow is still holding it; see outer_borrow above). */
   if (g_vdp_async)
   {
      YK_TRACE("context_reset engine-up now=%p (outer_borrow=%d)",
            (void *)(yk.GetCurrentContext ? yk.GetCurrentContext() : NULL), outer_borrow);
      if (!outer_borrow)
         yk_restore_front();
      /* Engine is fully up: start the machine on its own thread. */
      yab_emu_start();
   }
}

static void context_destroy(void)
{
   /* The frontend calls context_destroy before retro_unload_game.  There is no
    * VDP worker thread to join any more (the renderer is retro_run itself), so
    * stop the machine first and then hold the core's shared context on this
    * thread for the engine's GL teardown.  yk_attach() remembers the frontend's
    * context and yk_detach() hands it back, so the frontend keeps drawing with
    * the context it thinks it owns.  (Vdp2StopRenderThread() is a no-op stub
    * left for the exit order in AGENTS #17; see SYNCPATH-STATUS-2026-10-31.md.) */
   yab_emu_stop();
   Vdp2StopRenderThread();
   if (g_vdp_async)
      yk_attach();
   if (renderer_running)
      VIDCore->DeInit();
   renderer_running = false;
   if (g_vdp_async)
      yk_detach();
#if !defined(_USEGLEW_)
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
#endif
}

static bool retro_init_hw_context(void)
{
#if defined(_USEGLEW_)
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = true;
   /* the engine clips VDP2 layers with the stencil buffer (YglSetVdp2Window) */
   hw_render.stencil = true;
   hw_render.bottom_left_origin = true;
#ifdef _OGLES3_
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
       return false;
#endif
#else
   glsm_ctx_params_t params = {0};
   params.context_reset = context_reset;
   params.context_destroy = context_destroy;
   params.environ_cb = environ_cb;
   params.stencil = true;
#ifdef _OGLES3_
   params.context_type = RETRO_HW_CONTEXT_OPENGLES_VERSION;
   params.major = 3;
   params.minor = 1;
   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
      return false;
#else
   params.context_type = RETRO_HW_CONTEXT_OPENGL;
   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
      return false;
#endif
#endif
   return true;
}

/************************************
 * libretro implementation
 ************************************/

static struct retro_system_av_info g_av_info;

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "YabaSanshiro";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "v" VERSION GIT_VERSION;
   info->need_fullpath    = true;
   /* ST-V games are MAME romsets: the core opens the .zip itself and matches its
      entries by name + CRC32 against the game list, so the frontend must hand the
      archive over as-is.  With block_extract false RetroArch extracts the zip and
      passes the extracted member (measured on EmuELEC/RetroArch 1.21.0:
      "extracting archive to temporary directory ... /tmp/cache/eeprom-<game>.bin"),
      the detection never runs and the zip is then tried as a Saturn CD image
      ("Unsupported CD image!").  Both reference branches (libretro/yabause
      yabasanshiro and kronos) set this to true. */
   info->block_extract    = true;
   info->valid_extensions = "bin|ccd|chd|cue|iso|mds|m3u|zip";
}

void check_variables(void)
{
   struct retro_variable var;

   /* ST-V: which region's BIOS from stvbios.zip to run a Titan cart with.  The
    * loader in stv.c only ever picks a BIOS whose region the cart header allows,
    * so this is a preference among legal choices, not an override -- it exists
    * because some Japanese carts (e.g. seabass V0.001) only come up on the JP
    * BIOS.  Kronos has the same option as kronos_stv_favorite_region. */
   var.key = "yabasanshiro_stv_region";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "US") == 0)      stv_favorite_region = STV_REGION_US;
      else if (strcmp(var.value, "JP") == 0) stv_favorite_region = STV_REGION_JP;
      else if (strcmp(var.value, "TW") == 0) stv_favorite_region = STV_REGION_TW;
      else                                   stv_favorite_region = STV_REGION_EU;
   }
   /* YAB_STV_REGION=eu|us|jp|tw still wins, so the headless harness (which has
    * no core options) can test regions without a separate build. */
   {
      const char *e = getenv("YAB_STV_REGION");
      if (e && *e)
      {
         if (!strcmp(e, "us"))      stv_favorite_region = STV_REGION_US;
         else if (!strcmp(e, "jp")) stv_favorite_region = STV_REGION_JP;
         else if (!strcmp(e, "tw")) stv_favorite_region = STV_REGION_TW;
         else if (!strcmp(e, "eu")) stv_favorite_region = STV_REGION_EU;
      }
   }

   /* Video standard: auto (default) reads the disc IP.BIN region at load;
    * NTSC/PAL force one standard.  ST-V is handled separately and is always
    * NTSC.  NTSC remains the unreadable-disc fallback, so existing content
    * keeps working; "play PAL at 60" is the explicit NTSC choice. */
   var.key = "yabasanshiro_videoformattype";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "PAL") == 0)
         g_videoformat_opt = VIDEOFORMATTYPE_PAL;
      else if (strcmp(var.value, "NTSC") == 0)
         g_videoformat_opt = VIDEOFORMATTYPE_NTSC;
      else if (strcmp(var.value, "auto") == 0)
         g_videoformat_opt = YAB_VIDEOFORMAT_AUTO;
   }
   /* Probe override, same pattern as YAB_STV_REGION: lets an on-device A/B
      test both formats without editing the per-core cfg. */
   {
      const char *e = getenv("YAB_VIDEOFORMAT");
      if (e && *e)
      {
         if (!strcasecmp(e, "pal"))       g_videoformat_opt = VIDEOFORMATTYPE_PAL;
         else if (!strcasecmp(e, "ntsc")) g_videoformat_opt = VIDEOFORMATTYPE_NTSC;
         else if (!strcasecmp(e, "auto")) g_videoformat_opt = YAB_VIDEOFORMAT_AUTO;
      }
   }

   var.key = "yabasanshiro_force_hle_bios";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0 && hle_bios_force)
         hle_bios_force = false;
      else if (strcmp(var.value, "enabled") == 0 && !hle_bios_force)
         hle_bios_force = true;
   }

   var.key = "yabasanshiro_rbg_resolution_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "original") == 0)
      {
         g_rbg_resolution_mode = 0;
      }
      else if (strcmp(var.value, "2x") == 0)
      {
         g_rbg_resolution_mode = 1;
      }
      else if (strcmp(var.value, "720p") == 0)
      {
         g_rbg_resolution_mode = 2;
      }
      else if (strcmp(var.value, "1080p") == 0)
      {
         g_rbg_resolution_mode = 3;
      }
   }

   var.key = "yabasanshiro_rbg_use_compute_shader";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         g_rbg_use_compute_shader = 1;
      else if (strcmp(var.value, "disabled") == 0)
         g_rbg_use_compute_shader = 0;
   }

#ifdef DYNAREC_DEVMIYAX
   var.key = "yabasanshiro_sh2coretype";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "dynarec") == 0)
         g_sh2coretype = 3;
      else if (strcmp(var.value, "interpreter") == 0)
         g_sh2coretype = SH2CORE_INTERPRETER;

      /* Debug override: YAB_SH2=interp forces the interpreter even when the
         frontend option says dynarec.  The dynarec only syncs regs.PC at block
         boundaries, so PC-based probes read 0 and cannot tell which CPU (BIOS
         or game) is polling the IOGA ports. */
      {
         const char *e = getenv("YAB_SH2");
         if (e && strcmp(e, "interp") == 0)
            g_sh2coretype = SH2CORE_INTERPRETER;
      }
   }
#endif

   var.key = "yabasanshiro_addon_cart";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "none") == 0 && addon_cart_type != CART_NONE)
         addon_cart_type = CART_NONE;
      else if (strcmp(var.value, "1M_extended_ram") == 0 && addon_cart_type != CART_DRAM8MBIT)
         addon_cart_type = CART_DRAM8MBIT;
      else if (strcmp(var.value, "4M_extended_ram") == 0 && addon_cart_type != CART_DRAM32MBIT)
         addon_cart_type = CART_DRAM32MBIT;
   }

   var.key = "yabasanshiro_multitap_port1";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         multitap[0] = 0;
      else if (strcmp(var.value, "enabled") == 0)
         multitap[0] = 1;
   }

   var.key = "yabasanshiro_multitap_port2";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         multitap[1] = 0;
      else if (strcmp(var.value, "enabled") == 0)
         multitap[1] = 1;
   }
   var.key = "yabasanshiro_service_enabled";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         service_enabled = true;
      else
         service_enabled = false;
   }

   var.key = "yabasanshiro_resolution_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "original") == 0)
      {
         resolution_mode = 1;
         g_resolution_mode = 3;
      }
      else if (strcmp(var.value, "2x") == 0)
      {
         resolution_mode = 2;
         g_resolution_mode = 2;
      }
#ifndef LOW_END
      else if (strcmp(var.value, "4x") == 0)
      {
         resolution_mode = 4;
         g_resolution_mode = 1;
      }
#endif
   }

#ifdef ALLOW_POLYGON_MODE
   var.key = "yabasanshiro_polygon_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "perspective_correction") == 0)
         polygon_mode = PERSPECTIVE_CORRECTION;
      else if (strcmp(var.value, "gpu_tesselation") == 0)
         polygon_mode = GPU_TESSERATION;
      else if (strcmp(var.value, "cpu_tesselation") == 0)
         polygon_mode = CPU_TESSERATION;
   }
#endif

   /* There is no "Async Renderer" option any more: the model is fixed
    * (design §1), so g_vdp_async is latched to 1 in retro_load_game(). */
   /* Frameskip is not implemented in this model yet and the core option stays
    * off (design §5.4: mechanism only, default unchanged). */
   /* 每次解析选项后打一行"生效配置"，测量日志里能自证：这轮跑的是不是
    * 关跳帧 + 100% 时钟（历史上有过拿错 cfg 测出假结论的教训）。 */
#if YAB_TRACE_LEVEL >= 1
   {
      static int first = 1;
      if (first)
      {
         first = 0;
         var.key = "yabasanshiro_frameskip"; var.value = NULL;
         environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
         printf("[CFG] frameskip=%s", (var.value && *var.value) ? var.value : "?");
         var.key = "yabasanshiro_cpu_clock"; var.value = NULL;
         environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
         printf(" cpu_clock=%s", (var.value && *var.value) ? var.value : "?");
         var.key = "yabasanshiro_resolution_mode"; var.value = NULL;
         environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
         printf(" resolution=%s", (var.value && *var.value) ? var.value : "?");
         printf(" async=always");
         var.key = "yabasanshiro_sh2coretype"; var.value = NULL;
         environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
         printf(" sh2=%s\n", (var.value && *var.value) ? var.value : "?");
         fflush(stdout);
      }
   }
#endif

   var.key = "yabasanshiro_frameskip";
   var.value = NULL;
   g_frameskip = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      if (strcmp(var.value, "auto") == 0 || strcmp(var.value, "enabled") == 0)
         g_frameskip = 1;
   /* Hand the decision to the renderer: it latches it in VIN and skips the next
    * VDP2 compose (design §5.4).  Only on change, so the drift accounting is
    * not restarted by every option refresh. */
   {
      static int applied = -1;
      if (applied != g_frameskip)
      {
         applied = g_frameskip;
         if (g_frameskip) EnableAutoFrameSkip();
         else             DisableAutoFrameSkip();
      }
   }

   /* SH-2 CPU clock percentage ("cycle rate").  Applied live: the per-line
    * SH-2 budget is re-derived between frames, the counters are untouched. */
   var.key = "yabasanshiro_cpu_clock";
   var.value = NULL;
   if (!yab_cpu_clock_env && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int pct = atoi(var.value);
      if (pct < 25) pct = 25;
      else if (pct > 100) pct = 100;
      if (pct != yab_cpu_clock_pct)
      {
         yab_cpu_clock_pct = pct;
         /* PORT: CPU clock scaling is not ported */
      }
   }

   /* VDP1 framebuffer readback.  Off by default: most titles never read the
      framebuffer.  When on, the async renderer records the completed drawframe
      and downloads it on demand, plus a short prefetch window after a read;
      when off, the pixel read returns 0. */
   var.key = "yabasanshiro_vdp1_fb_readback";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         yab_vdp1_fb_readback = 1;
      else if (strcmp(var.value, "disabled") == 0)
         yab_vdp1_fb_readback = 0;
   }
   {
      const char *e = getenv("YAB_VDP1_FB_READBACK");
      if (e && *e)
         yab_vdp1_fb_readback = (e[0] != '0');
   }
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));

   if(initial_resolution_mode == 0)
   {
      // Get the initial resolution mode at start
      // It will be the resolution_mode limit until the core is restarted
      check_variables();
      initial_resolution_mode = resolution_mode;
   }

   {
      int pal = (retro_get_region() == RETRO_REGION_PAL);
      info->timing.fps = pal ? 50.0f : 60.0f;
      info->timing.sample_rate    = SAMPLERATE;
      info->geometry.base_width   = game_width;
      info->geometry.base_height  = game_height;
      // No need to go above 8x what is needed by Hi-Res games, we disallow 16x for Hi-Res games
      info->geometry.max_width    = 704 * (initial_resolution_mode == max_resolution_mode ? max_resolution_mode/2 : initial_resolution_mode);
      info->geometry.max_height   = 512 * (initial_resolution_mode == max_resolution_mode ? max_resolution_mode/2 : initial_resolution_mode);
      /* Native 4:3 for both standards: Kronos/Yabause/Beetle all document
       * PAL as 4:3, and the old 5:4 branch was copied from the reference core
       * without a pixel check. */
      info->geometry.aspect_ratio = 4.0 / 3.0;
   }
   /* Screen rotation, following the RetroArch convention the way flycast does it
      (shell/libretro/libretro.cpp: setRotation() + setGameGeometry()): the core
      leaves the framebuffer alone and asks the frontend to turn it.
      Kronos -- where this ST-V support came from -- is the same in its libretro
      build: its renderer's own rotation is guarded by
      `#ifndef __LIBRETRO__ ... #else u32 isRotated = 0;` (opengl/common/src/
      ogl_shader.c), i.e. the rotated texture-coordinate set is for its
      standalone build only.  So no renderer work belongs here; this core has
      none and needs none.
      Vertical ST-V boards produce a sideways picture -- the ported game list
      carries a per-title ROTATED flag (shienryu is one) and STVInit() copies it
      into yabsys.isRotated; a Saturn game leaves that 0.
      The command is sent unconditionally, including 0: a frontend keeps the last
      value it was handed (minarch-gl's ma_gl_rotation is a plain static, reset by
      nothing between games), so staying silent would leave the previous title's
      rotation applied.  The aspect ratio is inverted only while rotated, so that
      the frontend's own width/height swap lands on the right box -- minarch-gl
      documents exactly this pairing ("it fits the core's reported aspect, which
      for flycast rotated games is 0.75"), and Kronos, where this ST-V support
      came from, does the same in its av_info path. */
   {
      unsigned rotation = 0;   /* yabsys.isRotated comes with the ST-V port */
      environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);
      if (rotation != 0)
         info->geometry.aspect_ratio = 1.0f / info->geometry.aspect_ratio;
   }
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if(pad_type[port] != device)
   {
      pad_type[port] = device;
      if(PERCore)
         PERCore->Init();
      // When all devices are set, we can send input descriptors
      if (all_devices_ready)
         set_descriptors();
   }
}

/* libretro requires retro_serialize_size() to stay constant for the lifetime of
 * a loaded game, but Yabause's raw state size varies a little frame to frame
 * (VDP1 command lists, movie data, ...).  Probe once, then return a generous
 * fixed upper bound for the rest of the session and zero-pad every serialize to
 * fill it.  Reset per content in retro_load_game_common().
 *
 * This is the reference implementation (lr-yabasanshiro
 * src/libretro/libretro.c:976-1034).  It replaces the previous no-op stubs that
 * returned size 0 / true, which left the frontend unable to save states at all
 * (minarch's State_write() bails out when serialize_size() is 0) while making
 * every load look successful.  The old dead code below them memcpy'd the
 * caller's size out of a freshly allocated buffer with no bounds check. */
static size_t g_serialize_size = 0;

size_t retro_serialize_size(void)
{
   void *buffer = NULL;
   size_t size = 0;
   int error;

   if (g_serialize_size != 0)
      return g_serialize_size;

   ScspMuteAudio(SCSP_MUTE_SYSTEM);
   error = YabSaveStateBuffer (&buffer, &size);
   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);

   free(buffer);

   if (error || size == 0)
      return 0; /* not ready yet -- do not cache, the frontend retries */

   /* +25% +2MB headroom over the first observed state size. */
   g_serialize_size = size + (size / 4) + (2 * 1024 * 1024);
   return g_serialize_size;
}

bool retro_serialize(void *data, size_t size)
{
   void *buffer = NULL;
   size_t out_size = 0;
   int error;

   /* The machine runs on its own thread: park it so the state we save is a
    * between-frames state, not one being mutated under us.  The state lives in
    * plain globals now, so no binding is needed. */
   yab_emu_pause();
   ScspMuteAudio(SCSP_MUTE_SYSTEM);
   error = YabSaveStateBuffer (&buffer, &out_size);
   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
   yab_emu_resume();

   if (error || !buffer)
      return false;

   if (out_size > size)
   {
      /* State grew past the cached upper bound -- fail rather than truncate. */
      free(buffer);
      return false;
   }

   memcpy(data, buffer, out_size);
   if (out_size < size)
      memset((unsigned char *)data + out_size, 0, size - out_size);
   free(buffer);
   return true;
}

/* ---------------------------------------------------------------------------
 * State loads apply immediately.
 *
 * This used to be a "warm-up gate": a load arriving during the first ~2 s of
 * boot -- exactly what resume-on-launch does -- was stashed and replayed from
 * retro_run() once the machine was warm, because loading it cold left the sound
 * path silent for the rest of the session.
 *
 * That was a workaround for a real bug in the SCSP savestate, fixed at the
 * source: the sound CPU (68k) register block was delegated to M68K->SaveState()
 * / M68K->LoadState(), which the Musashi interface implemented as empty stubs,
 * so no state file carried any 68k state; and restoring the stack pointers
 * before writing SR made Musashi swap them whenever the live mode differed from
 * the saved one.  With both fixed a state loads correctly at any frame (verified
 * on device: a load at frame 1 now matches a plain boot sample for sample), so
 * the gate is gone.  See .notes/audio-ab/STATE-68K-ROOTCAUSE.md.
 *
 * Caveat: state files written before that fix (SCSP chunk version < 5) contain
 * no 68k state at all and still cannot be replayed correctly from a cold start;
 * re-save them with this build.
 * ------------------------------------------------------------------------- */
static int apply_state_buffer(const void *data, size_t size)
{
   int error = YabLoadStateBuffer(data, size);
   if (g_vdp_async)
   {
      /* retro_set_resolution() rebuilds the video core's GL objects, and the
       * thread driving retro_run does not have the core's shared context
       * current -- borrow it for the duration, exactly like the option-change
       * and res_pending paths in retro_run().  VdpRevoke()/VdpResume() are the
       * old handshake with the deleted VDP worker and are no-ops today
       * (vdp2.cpp); they stay so a restored render thread keeps working. */
      VdpRevoke();
      yk_attach();
      retro_set_resolution();
      yk_detach();
      VdpResume();
   }
   else
      retro_set_resolution();
   return error;
}

bool retro_unserialize(const void *data, size_t size)
{
   bool ok;
   yab_emu_pause();
   /* A state load restores VRAM/registers through the thread-local pointers: bind
    * them to the machine's buffers first, otherwise the restored data lands in
    * this thread's own pointers and the machine keeps running with empty VRAM
    * (measured: every layer sampled nothing and the picture was black).
    * Pointers only -- the full bind would zero the live VDP1 handshake, the same
    * way it did on the save path. */
   ok = apply_state_buffer(data, size) == 0;
   Vdp2AfterLoadState();      /* PORT: pirry's reset of Vdp1External/waits after a load */
   /* Audio queued before the load belongs to the old timeline (the sound thread
    * keeps producing while the frontend is busy loading); handing it over after
    * the load is both wrong and a blocking burst. */
   yab_audio_drop_pending();
   yab_emu_resume();
   return ok;
}

void retro_cheat_reset(void)
{
   CheatClearCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;

   if (CheatAddARCode(code) == 0)
      return;
}

static int does_file_exist(const char *filename)
{
   struct stat st;
   int result = stat(filename, &st);
   return result == 0;
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   strncpy(buf, path_basename(path), size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

void retro_init(void)
{
   struct retro_log_callback log;
   const char *dir = NULL;

   log_cb                   = NULL;
   perf_get_cpu_features_cb = NULL;
   uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;
   /* Performance level for interpreter CPU core is 16 */
   unsigned level           = 16;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;

   /* The machine runs on its own thread and reads input from there, so ask the
    * frontend to poll BEFORE retro_run (libretro's POLL_TYPE_EARLY -- the same
    * request mupen64plus-next makes when its threaded renderer is on; minarch-gl
    * honours it, see minarch.c:294-311). */
   { unsigned poll_early = 1; /* RA poll_type_override_t: 1 = EARLY.  The
                                * command is a RetroArch extension (0x800004) that
                                * this tree's libretro.h predates. */
     environ_cb(0x800004u, &poll_early); }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir));
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
   }

   char save_dir[PATH_MAX];
   snprintf(save_dir, sizeof(save_dir), "%s%cyabasanshiro%c", g_save_dir, slash, slash);
   path_mkdir(save_dir);

   /* ST-V EEPROM/NVRAM lives in <save_dir>/stv/ (see the eepromdir set in
      retro_load_game).  T123Save() just fopen()s the path, so the directory
      has to exist or every write silently fails and the machine settings
      (credits, coin mode) are lost on exit.  Kronos creates its equivalent
      directory the same way. */
   snprintf(save_dir, sizeof(save_dir), "%s%cstv%c", g_save_dir, slash, slash);
   path_mkdir(save_dir);

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);
}


/* ---- Disk control (multi-disc swap / .m3u playlists) ----------------------
 * Ported from the reference libretro core (lr-yabasanshiro).  Saturn games like
 * Panzer Dragoon Saga, Shining Force III and Grandia ship on several CDs; the
 * frontend drives the virtual tray through this interface and the media change
 * is delegated to cs2.c (Cs2ForceOpenTray / Cs2ForceCloseTray reopen the ISO CD
 * core on a new image). */

#define YABA_MAX_DISKS 16

static char     disk_paths[YABA_MAX_DISKS][PATH_MAX];
static char     disk_labels[YABA_MAX_DISKS][PATH_MAX];
static unsigned disk_count        = 0;
static unsigned disk_index        = 0;
static unsigned disk_initial_idx  = 0;
static char     disk_initial_path[PATH_MAX];
static bool     disk_tray_open    = false;

static int ext_is_m3u(const char *path)
{
   const char *e = path_get_extension(path);
   return e && (e[0] == 'm' || e[0] == 'M')
            && (e[1] == '3')
            && (e[2] == 'u' || e[2] == 'U')
            &&  e[3] == '\0';
}

static void disk_set_label_from_path(unsigned idx)
{
   char base[PATH_MAX];
   if (idx >= YABA_MAX_DISKS || disk_paths[idx][0] == '\0')
      return;
   strncpy(base, path_basename(disk_paths[idx]), sizeof(base) - 1);
   base[sizeof(base) - 1] = '\0';
   path_remove_extension(base);
   strncpy(disk_labels[idx], base, sizeof(disk_labels[idx]) - 1);
   disk_labels[idx][sizeof(disk_labels[idx]) - 1] = '\0';
}

static bool disk_set_eject_state(bool ejected)
{
   if (ejected)
      Cs2ForceOpenTray();
   else
   {
      if (disk_index >= disk_count || disk_paths[disk_index][0] == '\0')
         return false;
      if (Cs2ForceCloseTray(CDCORE_ISO, disk_paths[disk_index]) != 0)
         return false;
      /* keep yabause's notion of the loaded image in sync */
      snprintf(full_path, sizeof(full_path), "%s", disk_paths[disk_index]);
   }
   disk_tray_open = ejected;
   return true;
}

static bool     disk_get_eject_state(void) { return disk_tray_open; }
static unsigned disk_get_image_index(void) { return disk_index; }
static unsigned disk_get_num_images(void)  { return disk_count; }

static bool disk_set_image_index(unsigned index)
{
   /* The media is only swapped when the tray is closed (set_eject_state(false));
    * here we just record which image will be inserted. index == disk_count is the
    * frontend's "no disk" sentinel. */
   if (index == disk_count)
   {
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
   if (index >= YABA_MAX_DISKS)
      return false;

   if (!info || !info->path) /* remove this image */
   {
      unsigned i;
      if (index >= disk_count)
         return false;
      for (i = index; i + 1 < disk_count; i++)
      {
         snprintf(disk_paths[i], PATH_MAX, "%s", disk_paths[i + 1]);
         disk_set_label_from_path(i);
      }
      if (disk_count > 0)
         disk_count--;
      if (disk_count > 0 && disk_index >= disk_count)
         disk_index = disk_count - 1;
      return true;
   }

   snprintf(disk_paths[index], PATH_MAX, "%s", info->path);
   disk_set_label_from_path(index);
   if (index >= disk_count)
      disk_count = index + 1;
   return true;
}

static bool disk_add_image_index(void)
{
   if (disk_count >= YABA_MAX_DISKS)
      return false;
   disk_paths[disk_count][0]  = '\0';
   disk_labels[disk_count][0] = '\0';
   disk_count++;
   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
   disk_initial_idx = index;
   if (path)
      snprintf(disk_initial_path, sizeof(disk_initial_path), "%s", path);
   else
      disk_initial_path[0] = '\0';
   return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
   if (index >= disk_count || !path || len == 0 || disk_paths[index][0] == '\0')
      return false;
   strncpy(path, disk_paths[index], len - 1);
   path[len - 1] = '\0';
   return true;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
   if (index >= disk_count || !label || len == 0 || disk_labels[index][0] == '\0')
      return false;
   strncpy(label, disk_labels[index], len - 1);
   label[len - 1] = '\0';
   return true;
}

static const struct retro_disk_control_ext_callback disk_ext_cb = {
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

static const struct retro_disk_control_callback disk_cb = {
   disk_set_eject_state,
   disk_get_eject_state,
   disk_get_image_index,
   disk_set_image_index,
   disk_get_num_images,
   disk_replace_image_index,
   disk_add_image_index,
};

/* Build the disk list from the content path: either an .m3u playlist (one disc
 * per line, '#' comments, paths absolute or relative to the playlist dir) or a
 * single disc image. */
static void disk_init_from_content(const char *content_path)
{
   disk_count     = 0;
   disk_index     = 0;
   disk_tray_open = false;

   if (ext_is_m3u(content_path))
   {
      char basedir[PATH_MAX];
      char line[PATH_MAX];
      FILE *fp;
      fill_pathname_basedir(basedir, content_path, sizeof(basedir));
      fp = fopen(content_path, "r");
      if (fp)
      {
         while (fgets(line, sizeof(line), fp) && disk_count < YABA_MAX_DISKS)
         {
            char *nl;
            char *p = line;
            while (*p == ' ' || *p == '\t')
               p++;
            if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
               continue;
            nl = strpbrk(p, "\r\n");
            if (nl)
               *nl = '\0';
            if (*p == '\0')
               continue;
            if (path_is_absolute(p))
               snprintf(disk_paths[disk_count], PATH_MAX, "%s", p);
            else
               fill_pathname_join(disk_paths[disk_count], basedir, p, PATH_MAX);
            disk_set_label_from_path(disk_count);
            disk_count++;
         }
         fclose(fp);
      }
   }

   if (disk_count == 0) /* single image, or empty/unreadable playlist */
   {
      snprintf(disk_paths[0], PATH_MAX, "%s", content_path);
      disk_set_label_from_path(0);
      disk_count = 1;
   }

   /* honour a frontend-requested initial image (last-used disc restore) */
   if (disk_initial_idx < disk_count &&
       (disk_initial_path[0] == '\0' ||
        strcmp(disk_initial_path, disk_paths[disk_initial_idx]) == 0))
      disk_index = disk_initial_idx;

   disk_initial_idx     = 0;
   disk_initial_path[0] = '\0';
}

static void disk_register(void)
{
   unsigned version = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version)
         && version >= 1)
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE,
            (void *)&disk_ext_cb);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE,
            (void *)&disk_cb);
}

/* AUTO video format: read the Saturn IP.BIN region field from the first disc
 * image.  The field is a list of allowed area codes; the emulated SMPC uses the
 * first one (Cs2GetIP()), so we do the same:
 *   E = Europe, A = Asia PAL, L = Latin America   -> PAL
 *   J = Japan, U = USA, T = Taiwan, K = Korea, B = Brazil -> NTSC
 * A multi-region "JUE" disc therefore starts with J and follows the Japanese
 * (NTSC) path, exactly like Cs2GetIP()'s autodetect.
 * Anything unreadable or unknown defaults to NTSC; no BIOS-region guess (the
 * US/EU mpr-17933 image is dual-region and cannot decide this). */
static int saturn_disc_video_format(const char *path, char *region_out,
                                    size_t region_out_size, const char **source)
{
   char region[17];
   int i;

   region[0] = '\0';
   if (ISOCDReadRegionCode(path, region, sizeof(region)) != 0)
   {
      if (region_out && region_out_size)
         region_out[0] = '\0';
      *source = "disc header unreadable";
      return VIDEOFORMATTYPE_NTSC;
   }

   for (i = 15; i >= 0 && (region[i] == ' ' || region[i] == '\0'); i--)
      region[i] = '\0';
   if (region_out && region_out_size)
      snprintf(region_out, region_out_size, "%s", region);

   switch (region[0])
   {
      case 'E': case 'A': case 'L':
         *source = "disc header PAL";
         return VIDEOFORMATTYPE_PAL;
      case 'J': case 'U': case 'T': case 'K': case 'B':
         *source = "disc header NTSC";
         return VIDEOFORMATTYPE_NTSC;
      default:
         *source = "disc header unknown region";
         return VIDEOFORMATTYPE_NTSC;
   }
}

bool retro_load_game_common()
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   g_serialize_size = 0; /* re-probe the savestate size for this content */
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;
   if (!retro_init_hw_context())
      return false;

   yinit.vidcoretype               = VIDCORE_OGL;
   yinit.percoretype               = PERCORE_LIBRETRO;
   yinit.sh2coretype               = g_sh2coretype;
   yinit.sndcoretype               = SNDCORE_LIBRETRO;
#ifdef HAVE_MUSASHI
   yinit.m68kcoretype              = M68KCORE_MUSASHI;
#else
   yinit.m68kcoretype              = M68KCORE_C68K;
#endif
   yinit.mpegpath                  = NULL;
   yinit.frameskip                 = 0;   /* Auto Frameskip option removed (see frameSkipAndLimit) */
   yinit.rbg_resolution_mode       = g_rbg_resolution_mode;
   yinit.rbg_use_compute_shader    = g_rbg_use_compute_shader;
   yinit.usethreads                = 0;
   yinit.rotate_screen             = 0;
   yinit.skip_load                 = 0;
   yinit.polygon_generation_mode   = polygon_mode;
   /* Back to the value committed by ac63199e.  The "= 1" that sat here was a
    * failed experiment from the picture hunt (see .notes/STV-HANDOFF.md 79.2:
    * reverting extend_backup alone did NOT restore the picture), but with the
    * Kronos-aligned backup-RAM handlers that commit also introduced, leaving it
    * at 1 breaks the ST-V BIOS' credit counting: cotton2 then shows CREDIT 0 and
    * a coin insert never rises -- measured 2026-10-02, stvS1..stvS4. */
   yinit.extend_backup             = 0;
   yinit.buppath                   = bup_path;
   /* SCSP core: 1 = the "new" one, which is what the STANDALONE runs
    * (retro_arena/main.cpp:345 `yinit.use_new_scsp = 1`), i.e. what this port is
    * being aligned against.
    *
    * This is NOT only a mixer choice -- it changes the 68k sound CPU's schedule:
    *   use_new_scsp = 0 -> the 68k is executed from the main emulation loop, once
    *                       per declline slice (yabause.c:962 `M68KExec(cycles)`),
    *   use_new_scsp = 1 -> it is executed from ScspExec()'s sample clock
    *                       (scsp.c:6062 `MM68KExec(samplecnt)`), and the cycle
    *                       budget is derived per frame (yabause.c:718).
    * So 0 vs 1 gives the sound CPU a different phase relative to the SH-2s, which
    * is an emulation-timing difference (and `scsp.rbp` even has different units,
    * scsp.c:2922/3086), not just a volume difference.
    *
    * The old comment here justified 0 with our fork's Yabadabadoo loudness
    * comparison (the new mixer is ~11 dB quieter on the same state).  That
    * comparison is not the reference any more: pirry's standalone is, and it uses
    * the new core.  Flip this back to 0 only as an explicit listening-test
    * decision, never for "alignment". */
   yinit.use_new_scsp              = 1;
   yinit.scsp_sync_count_per_frame = 1;
   yinit.scsp_main_mode            = 1;
   yinit.videoformattype           = (g_videoformat_pending == VIDEOFORMATTYPE_PAL)
                                       ? VIDEOFORMATTYPE_PAL : VIDEOFORMATTYPE_NTSC;
   yinit.video_filter_type         = 0;

   return true;
}

/* Frontend capability: may we hand it a NULL frame?  The libretro contract says
 * a dropped field may be duped with NULL only when the frontend answers
 * RETRO_ENVIRONMENT_GET_CAN_DUPE with true (libretro.h:7819-7823); a frontend
 * that cannot accept NULL would take the call as an invalid frame.  We do not
 * support such a frontend as a first-class target, but it must never crash or
 * hang, so yab_video_dupe() degrades to a second RETRO_HW_FRAME_BUFFER_VALID
 * instead of NULL -- see the comment there.  Conservative default: false until
 * the frontend answers (an unanswered query is not a "yes"). */
static bool g_can_dupe;

/* "Present nothing new this frame."  This core is hw-render only: video_cb's
 * data argument is RETRO_HW_FRAME_BUFFER_VALID, not a pixel pointer, so the
 * NULL-free fallback is to send VALID again and let the frontend present
 * whatever its current render target still holds (the frame we drew last).
 * That is a dupe in every way that matters.  It is a degraded mode for a
 * ring-based frontend (it may show an older slot -- AGENTS #18 is about
 * minarch, which answers CAN_DUPE=true and never takes this path), but it
 * cannot crash or hang: no buffer is written, no pointer is handed over, and
 * the call returns immediately. */
static void yab_video_dupe(void)
{
   if (g_can_dupe)
      video_cb(NULL, current_width, current_height, 0);
   else
      video_cb(RETRO_HW_FRAME_BUFFER_VALID, current_width, current_height, 0);
}

static void yab_query_frontend_caps(void)
{
   bool can_dupe = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
      g_can_dupe = can_dupe;
   else
      g_can_dupe = false;
   log_cb(RETRO_LOG_INFO, "GET_CAN_DUPE: %s (dupes %s)\n",
          g_can_dupe ? "true" : "false",
          g_can_dupe ? "use NULL" : "re-present RETRO_HW_FRAME_BUFFER_VALID");
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (!info)
      return false;

   check_variables();
   yab_query_frontend_caps();
   yab_audio_live = 1;

   /* The renderer model is fixed (design §1). */
   g_vdp_async = 1;

   /* Resolve the disc to boot: a single image, or the first entry of an .m3u
    * playlist (disk control can swap to the other entries later). */
   disk_init_from_content(info->path);
   snprintf(full_path, sizeof(full_path), "%s", disk_paths[disk_index]);
   /* ST-V: an ST-V game is a MAME romset zip whose entries match Kronos' GameList
      (filename + CRC32); the ST-V BIOS is stvbios.zip. Detect it before the Saturn
      BIOS probing below, because an ST-V game needs no Saturn BIOS. */
   snprintf(stv_bios_path, sizeof(stv_bios_path), "%s%cstvbios.zip", g_system_dir, slash);
   stvgame = NULL;
   stv_mode = false;
   yab_stv_mode = 0;
   {
      const char *ext = path_get_extension(info->path);

      if (ext != NULL && strcasecmp(ext, "zip") == 0)
      {
         STVGetSingle(info->path, stv_bios_path, &stvgame);
         if (stvgame != NULL)
         {
            stv_mode = true;
            yab_stv_mode = 1;
            log_cb(RETRO_LOG_INFO, "ST-V game detected: %s\n", stvgame);
         }
         else
            log_cb(RETRO_LOG_WARN,
                   "%s: no ST-V romset in the game list matched this zip -- "
                   "trying it as a Saturn CD image\n", info->path);
      }
   }

   /* Latch the video standard for this content before retro_load_game returns.
    * minarch calls retro_get_system_av_info() (Core_updateAVInfo) immediately
    * after load_game and initializes SND from core.fps, so deciding this in
    * context_reset/YabauseInit would be too late (see .notes/PAL-50FPS*.md).
    * ST-V hardware has no PAL mode: force NTSC and say so if the option asked
    * for PAL.  auto reads the disc IP.BIN region; unreadable defaults to NTSC. */
   {
      const char *source = "option";
      char region[17] = "";

      if (stv_mode)
      {
         g_videoformat_pending = VIDEOFORMATTYPE_NTSC;
         source = "ST-V";
         if (g_videoformat_opt == VIDEOFORMATTYPE_PAL)
            log_cb(RETRO_LOG_WARN, "ST-V is NTSC only; PAL video format ignored\n");
      }
      else if (g_videoformat_opt != YAB_VIDEOFORMAT_AUTO)
      {
         g_videoformat_pending = g_videoformat_opt;
         source = (g_videoformat_opt == VIDEOFORMATTYPE_PAL) ? "option PAL" : "option NTSC";
      }
      else
      {
         g_videoformat_pending =
            saturn_disc_video_format(full_path, region, sizeof(region), &source);
      }

      log_cb(RETRO_LOG_INFO, "Video format: %s (source=%s, region='%s', stv=%d)\n",
             g_videoformat_pending == VIDEOFORMATTYPE_PAL ? "PAL 50Hz/313" : "NTSC 60Hz/263",
             source, region, stv_mode ? 1 : 0);
   }

   snprintf(bios_path, sizeof(bios_path), "%s%csaturn_bios.bin", g_system_dir, slash);
   if (does_file_exist(bios_path) != 1)
   {
      log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
      snprintf(bios_path, sizeof(bios_path), "%s%csega_101.bin", g_system_dir, slash);
      if (does_file_exist(bios_path) != 1)
      {
         log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
         snprintf(bios_path, sizeof(bios_path), "%s%cmpr-17933.bin", g_system_dir, slash);
         if (does_file_exist(bios_path) != 1)
         {
            log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
         }
      }
   }

   // Real bios is REQUIRED, even if we support HLE bios
   // HLE bios is deprecated and causing more issues than it solves
   // No "autoselect HLE when bios is missing" ever again !
   if (stv_mode)
   {
      if (does_file_exist(stv_bios_path) != 1)
      {
         /* Experiment: YAB_STV_HLE=1 lets an ST-V game run on the HLE BIOS
            instead of aborting, to find out whether the coin path depends on
            the real BIOS.  Kronos itself always tolerates a missing ST-V BIOS
            (it only warns), so this is also closer to its behaviour. */
         const char *hle = getenv("YAB_STV_HLE");
         if (hle && strcmp(hle, "1") == 0)
            log_cb(RETRO_LOG_WARN, "ST-V BIOS missing but YAB_STV_HLE=1 -- using HLE BIOS\n");
         else
         {
            log_cb(RETRO_LOG_ERROR, "ST-V game detected but %s is missing, ABORTING\n", stv_bios_path);
            return false;
         }
      }
   }
   else if (does_file_exist(bios_path) != 1)
   {
      log_cb(RETRO_LOG_ERROR, "We are missing the bios, ABORTING\n");
      return false;
   }
   if (hle_bios_force)
   {
      log_cb(RETRO_LOG_WARN, "HLE bios is enabled, this is for debugging purpose only, expect lots of issues\n");
   }

   /* Backup-RAM path.  A Saturn keeps ONE internal memory card shared by every
      game, so it stays in the core's shared card file.  An ST-V board has its
      SRAM on the ROM board, i.e. per game: use Kronos' naming
      (<save>/stv/<game>.ram, next to the <game>.nv EEPROM).  The file layout is
      still our classic 64 KB card image (card byte N at offset 2N+1), not
      Kronos' dense 32 KB one -- see README, "Sega Titan Video (ST-V)". */
   if (stv_mode && stvgame != NULL)
      snprintf(bup_path, sizeof(bup_path), "%s%cstv%c%s.ram", g_save_dir, slash, slash, stvgame);
   else
      snprintf(bup_path, sizeof(bup_path), "%s%cyabasanshiro%cbackup.bin", g_save_dir, slash, slash);

   /* Prime the SAVE_RAM shadow from the core's backup card; the frontend may
    * then overlay its .srm on top before the first frame. */
   sram_seed_from_backup(bup_path);

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 8, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 8, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 8, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 8, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 8, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 9, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 9, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 9, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 9, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 9, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 10, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 10, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 10, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 10, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 10, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "C" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" },
      { 11, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 11, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 11, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
      { 11, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" },
      { 11, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" }, 

      { 0 },
   };

   /* NOTE: descriptors are NOT sent here.  Kronos sends them from
      set_descriptors() (first retro_run frame / port-device change), because
      this static Saturn table has no JOYPAD_SELECT and minarch's Input_init()
      latches on the FIRST SET_INPUT_DESCRIPTORS it ever sees -- sending this
      one here marked Select as unavailable and swallowed the ST-V coin key. */
   if (stv_mode)
   {
      /* ST-V is ported in a later step (PORT-TO-PIRRY-2026-11-01.md §3.4): the
         romset loader, the ST-V board hooks and the yabauseinit fields do not
         exist in this tree yet.  Fail loudly instead of booting a Saturn BIOS
         with an ST-V cart. */
      snprintf(stv_eeprom_dir, sizeof(stv_eeprom_dir), "%s%cstv%c", g_save_dir, slash, slash);
      yinit.stvgamepath         = full_path;
      yinit.stvgame             = stvgame;
      yinit.stvbiospath         = stv_bios_path;
      yinit.eepromdir           = stv_eeprom_dir;
      yinit.stv_favorite_region = stv_favorite_region;
      log_cb(RETRO_LOG_INFO, "ST-V preferred BIOS region: %s\n",
             stv_favorite_region == STV_REGION_JP ? "JP" :
             stv_favorite_region == STV_REGION_US ? "US" :
             stv_favorite_region == STV_REGION_TW ? "TW" : "EU");
      yinit.carttype            = CART_ROMSTV;
      yinit.cartpath            = NULL;
      yinit.cdcoretype          = CDCORE_DUMMY;
      yinit.cdpath              = NULL;
      yinit.biospath            = NULL;
   }
   else
   {
      yinit.cdcoretype       = CDCORE_ISO;
      yinit.cdpath           = full_path;
      yinit.biospath         = (hle_bios_force ? NULL : bios_path);
      yinit.carttype         = addon_cart_type;
      yinit.cartpath         = "\0";
   }

   return retro_load_game_common();
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
   /* The sound thread may be inside audio_batch_cb(); stop handing it audio
    * before the frontend tears its audio device down. */
   yab_audio_live = 0;
   /* The frontend runs context_destroy() before retro_unload_game() (minarch:
    * MA_GL_context_destroy() then this; the offscreen harness mirrors that
    * order), so context_destroy() has already DeInit()ed VIDCore and there is
    * no valid GL context left to re-init against -- the old VIDCore->Init()
    * here re-initialised the video core after teardown (lr-yabasanshiro calls
    * the equivalent change its clean-quit crash fix).  Detach the pointer
    * instead: YabauseDeInit()'s VideoDeInit() then skips a second DeInit
    * (VideoDeInit() NULL-checks VIDCore and clears it). */
   if (!renderer_running)
      VIDCore = NULL;
   YabauseDeInit();
}

unsigned retro_get_region(void)
{
   /* Before load_game(), fall back to the engine state (0 = NTSC).  After it,
    * the latched format is authoritative: minarch's chooseSyncRef() uses this
    * return value to decide whether the core FPS is the pacing reference. */
   if (g_videoformat_pending >= 0)
      return (g_videoformat_pending == VIDEOFORMATTYPE_PAL)
                ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
   return yabsys.IsPal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:   return sram_shadow;
      case RETRO_MEMORY_SYSTEM_RAM: return HighWram;
      default:                      return NULL;
   }
}

size_t retro_get_memory_size(unsigned id)
{
   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:   return sizeof(sram_shadow);
      case RETRO_MEMORY_SYSTEM_RAM: return 0x100000;
      default:                      return 0;
   }
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;
   all_devices_ready = false;
}

void retro_reset(void)
{
   yab_emu_pause();
   YabauseReset();
   Vdp2AfterLoadState();      /* PORT: pirry's reset of Vdp1External/waits after a load */
   yab_audio_drop_pending();  /* the queued audio is from the state being reset */
   yab_emu_resume();
   VdpResume();
   // The following function crashes the core when you use "restart"
   //YabauseResetButton();
}

void reset_global_gl_state()
{
   glUseProgram(0);
   glGetError();
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
   glDisableVertexAttribArray(0);
   glDisableVertexAttribArray(1);
   glDisableVertexAttribArray(2);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_STENCIL_TEST);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   
}

/* Per-frame trace used to find the FIRST divergence between a run that goes
 * black and one that does not: the PC sequence up to that point is identical, so
 * the first differing line is where the machine took a different path. */


/* Present the picture the VDP worker finished, if any (async renderer).  With
 * YAB_NOWAIT this runs at the START of retro_run: the worker had a whole frame
 * to finish the picture posted at the previous vblank, so it is always ready
 * and the hw-render slot it would render into next has already been presented
 * (no overwriting of an unpresented picture). */
/* ---------------------------------------------------------------------------
 * Async emulation (flycast model).
 *
 * The machine runs on this thread; the frontend thread (retro_run) does ALL the
 * GL and the presentation, from the frame slots the machine publishes at each
 * frame boundary.  That is what keeps every GL call and the frontend's
 * hw-render ring rotation on the same thread, which is the property flycast and
 * mupen64plus-next both rely on -- see .notes/THREADING-REFACTOR-PLAN-*.md.
 * ------------------------------------------------------------------------- */
static pthread_t yab_emu_tid;
static volatile int yab_emu_running = 0;
static volatile int yab_emu_paused = 0;
static volatile int yab_emu_parked = 1;

/* L2 probe [FT] (+[MW]): per-frame cost split, printed every <n> frames.
 * n defaults to 60 at level 2 and 1 at level 3; YAB_FT=<n> overrides it.
 *
 * When the host cannot emulate 59.94 VBlanks per second the game runs slow, and
 * the only questions that matter are "which stage is the ceiling" and "how much
 * of the frame is fixed overhead".  The async path has two independent stages
 * that overlap -- the machine thread's frame (PERCore->HandleEvents) and the
 * frontend thread's field render (yab_render_frame) -- so the frame rate is
 * max(machine, render) plus the queue handover, not their sum.  This probe
 * prints the average of both (plus how long the frontend waited for a slot,
 * i.e. the backpressure) over <n> frames, so a slow title can be attributed
 * instead of guessed at. */
#if YAB_TRACE_LEVEL >= 2
static u64 yab_ft_machine_ns = 0, yab_ft_render_ns = 0, yab_ft_wait_ns = 0;
/* [MW] (2026-10-30): how long the machine thread sits *blocked* outside its own
 * frame.  machine = its HandleEvents() call, so anything here is time the
 * machine spent waiting for the render thread's slot ring; [MP] all does not
 * cover it either.  This is the direct measurement of "does the renderer hold
 * the machine back". */
static u64 yab_ft_slotwait_ns = 0;
static int yab_ft_frames = 0, yab_ft_every = -1;
static u64 yab_ft_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static int yab_ft_on(void)
{
   if (yab_ft_every < 0)
   {
      const char *e = getenv("YAB_FT");
#if YAB_TRACE_LEVEL >= 3
      yab_ft_every = 1;            /* L3: every frame */
#else
      yab_ft_every = 60;           /* L2: about once a second */
#endif
      if (e && *e) { yab_ft_every = atoi(e); if (yab_ft_every <= 0) yab_ft_every = 60; }
   }
   return yab_ft_every;
}
static void yab_ft_report(void)
{
   int n = yab_ft_on();
   if (!n) return;
   if (++yab_ft_frames < n) return;
   printf("[FT] n=%d machine=%.2fms render=%.2fms wait=%.2fms\n",
          n,
          (double)yab_ft_machine_ns / (double)n / 1e6,
          (double)yab_ft_render_ns  / (double)n / 1e6,
          (double)yab_ft_wait_ns    / (double)n / 1e6);
   /* [MW] machine-thread blocking outside its own frame, printed with [FT] so a
    * run either shows "nobody holds it" or names the holder:
    *   slotwait = waiting for a free render slot
    * VDP1 has no wait on this thread in the async renderer (vdp2.cpp:2028), and
    * the readback option (its only other blocking point) is off for these
    * titles, so this is the complete list. */
   printf("[MW] n=%d slotwait=%.2fms\n",
          n,
          (double)yab_ft_slotwait_ns / (double)n / 1e6);
   yab_ft_slotwait_ns = 0;
   fflush(stdout);
   yab_ft_machine_ns = yab_ft_render_ns = yab_ft_wait_ns = 0;
   yab_ft_frames = 0;
}
#endif /* YAB_TRACE_LEVEL >= 2 */

static void *yab_emu_main(void *arg)
{
   (void)arg;
   /* PORT: match the standalone's machine-thread placement.  retro_arena/main.cpp
    * calls YabThreadSetAffinityRaw(0xFu) right before its emulation loop: the
    * machine may use all four cores, while VdpProc is pinned to core 0
    * (YabThreadSetCurrentThreadAffinityMask(0x1) at the top of VdpProc) and the
    * SCSP thread to cores 0-1 (scsp.c).
    *
    * Our emu thread is a plain pthread_create() from the frontend's thread, so it
    * INHERITS whatever mask the frontend set for itself.  If that mask is narrow,
    * the machine and the renderer end up on the same core -- and placement is not
    * just a performance question here: pirry's async model makes emulated timing
    * depend on the renderer's progress (vdp2.cpp:1441-1459 sends ScuSendDrawEnd at
    * the line where the VDP1 walk happens to have finished; if it has not, the
    * check is retried 10 lines later), so a starved machine/renderer pair can put
    * the game's frame swap on a different emulated line than the standalone's. */
   { extern void YabThreadSetCurrentThreadAffinityMask(int mask);
     YabThreadSetCurrentThreadAffinityMask(0xF); }
#ifdef YAB_HANGDBG
   printf("[DBGT] emu thread tid=%d\n", (int)syscall(SYS_gettid));
   fflush(stdout);
   YABEMU("emu:start");
#endif
   for (;;)
   {
      /* SAFE POINT: not touching machine state, so a pause request can be
       * honoured here.  Everything the machine blocks on inside a frame (the
       * J1a/J1b/J2 joins and the direct-draw ring) is abortable, so a pause
       * requested mid-frame also lands here within one frame. */
      yab_emu_parked = 1;
      while (yab_emu_paused)
      {
         if (!yab_emu_running) break;
         usleep(500);
      }
      if (!yab_emu_running) break;
      yab_emu_parked = 0;

      YABEMU("emu:handle-events");
#if YAB_TRACE_LEVEL >= 2
      if (yab_ft_on())
      {
         u64 _t0 = yab_ft_now();
         if (PERCore) PERCore->HandleEvents();       /* one machine frame */
         __atomic_add_fetch(&yab_ft_machine_ns, yab_ft_now() - _t0, __ATOMIC_RELAXED);
      }
      else
#endif
      if (PERCore)
         PERCore->HandleEvents();                    /* one machine frame */
      YABEMU("emu:after-events");
      sram_sync();
   }
   yab_emu_parked = 1;
   return NULL;
}

static void yab_emu_start(void)
{
   if (yab_emu_running) return;
   YglVdp1ReadbackResume();
   /* PORT: no pause protocol yet (the machine keeps running while the frontend is away) */
   yab_emu_paused = 0;
   yab_emu_parked = 0;
   yab_emu_running = 1;
   pthread_create(&yab_emu_tid, NULL, yab_emu_main, NULL);
#ifdef YAB_HANGDBG
   yab_dbg_watchdog_start();
#endif
}

static void yab_emu_stop(void)
{
   if (!yab_emu_running) return;
   /* Broadcast the join abort BEFORE waiting: the machine may be blocked in a
    * J1/J2 join or the direct-draw ring and must not hold up pthread_join. */
   yab_frame_abort();
   /* A blocked readback wait must not keep the thread from joining either. */
   YglVdp1ReadbackAbort();
   yab_emu_running = 0;
   yab_emu_paused = 0;
   pthread_join(yab_emu_tid, NULL);
   yab_emu_parked = 1;
}

/* Stop the machine between frames so the frontend thread can touch state it
 * owns (save states, reset, option changes that resize GL objects). */
static void yab_emu_pause(void)
{
   int spins = 0;
   if (!yab_emu_running) return;
   yab_emu_paused = 1;
   /* Abort the joins (J1a/J1b/J2 + the direct-draw ring) so the machine reaches
    * its safe point instead of waiting for a renderer that is about to park.
    * The machine may also be blocked in a VDP1 readback wait (the frontend is
    * the only thread that can service it), so wake that too. */
   yab_frame_abort();
   YglVdp1ReadbackAbort();
   /* Bounded: if the machine cannot reach its safe point (it may be inside a
    * long call), carry on after ~300 ms rather than blocking the frontend for
    * ever.  A resize that races one frame is recoverable; a frozen frontend is
    * not (measured: cold boot froze at the BIOS->game resolution change). */
   while (!yab_emu_parked && ++spins < 1500) usleep(200);
}

/* Frame CRC capture (L3 only).  YAB_VCRC=1 hashes every composed frame,
 * YAB_FRAMECRC=<f1,f2,...> hashes only those frame numbers, so two builds that
 * must be pixel-identical (e.g. a renderer change) can be compared without
 * depending on when a screenshot happened to be taken.  Emulation is
 * deterministic and frameskip is off in this mode, so the same frame number
 * must hash the same.  The frame counter is the number of composed frames,
 * i.e. the emulated frame number while frameskip is off. */
#if YAB_TRACE_LEVEL >= 3
static unsigned yab_fcrc_frame = 0;

static int yab_fcrc_wanted(unsigned frame_now)
{
   static char *cur = NULL;
   static int init = 0;
   if (!init)
   {
      const char *e = getenv("YAB_FRAMECRC");
      init = 1;
      cur = (e && *e) ? strdup(e) : NULL;
      /* no list -> YAB_VCRC: every frame */
   }
   if (!cur) return 1;
   while (*cur)
   {
      char *end;
      long v = strtol(cur, &end, 10);
      if (end == cur) return 0;             /* malformed tail */
      if (v == (long)frame_now) { cur = end; if (*cur == ',') cur++; return 1; }
      if (v > (long)frame_now) return 0;    /* that frame is still ahead */
      cur = end;
      if (*cur == ',') cur++;
   }
   return 0;
}

static unsigned yab_crc32(const unsigned char *p, size_t n)
{
   unsigned crc = 0xFFFFFFFFu;
   size_t i; int k;
   for (i = 0; i < n; i++)
   {
      crc ^= p[i];
      for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
   }
   return ~crc;
}

extern int GlWidth, GlHeight;
/* Hash the **currently bound draw FBO** (the composed frame).  Do not bind
 * mirror_fbo here: after present/adopt its attachments already point at the
 * next ring slot. */
static unsigned yab_fbo_crc(void)
{
   int w = GlWidth, h = GlHeight;
   size_t n;
   unsigned char *buf;
   unsigned crc = 0;
   if (w <= 0 || h <= 0) return 0;
   n = (size_t)w * (size_t)h * 4u;
   buf = (unsigned char *)malloc(n);
   if (!buf) return 0;
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
   crc = yab_crc32(buf, n);
   free(buf);
   return crc;
}

static int yab_crc_enabled(void)
{
   static int v = -1;
   if (v < 0)
   {
      const char *a = getenv("YAB_VCRC");
      const char *b = getenv("YAB_FRAMECRC");
      v = ((a && *a && *a != '0') || (b && *b)) ? 1 : 0;
   }
   return v;
}
#endif /* YAB_TRACE_LEVEL >= 3 */

/* 由 ygles.c 的 YglRender() 在 present 之前调用（合成点，唯一可信的位置）。
 * 低于 L3 时是空函数：调用点留在 ygles.c 里（一次空调用，可忽略），
 * 昂贵的 glReadPixels 只在 L3 及以上才会编译进来。 */
void yab_after_compose(void)
{
#if YAB_TRACE_LEVEL >= 3
   if (!yab_crc_enabled()) return;
   yab_last_crc = yab_fbo_crc();
   if (yab_fcrc_wanted(++yab_fcrc_frame))
      YAB_TR_L3("[FCRC] frame=%u (%dx%d) crc=%08x\n",
                yab_fcrc_frame, GlWidth, GlHeight, yab_last_crc);
#endif
}
static void yab_emu_resume(void)
{
   YglVdp1ReadbackResume();
   yab_frame_resume();
   yab_emu_paused = 0;
}

void retro_run(void)
{
   bool updated  = false;
   one_frame_rendered = false;
   yab_in_run = 1;          /* SET_GEOMETRY is legal only while this is set */

   /* Tell the machine (and the sound thread) that the frontend is running
    * again: this is the only "the frontend is not paused" signal a libretro
    * core gets.  See the pause block in vdp2.cpp. */
   yab_front_beat();
   { extern void YabSetRenderThread(void); YabSetRenderThread(); }

#ifdef YAB_HANGDBG
   {
      static int once = 0;
      if (!once) { once = 1; printf("[DBGT] front thread tid=%d\n", (int)syscall(SYS_gettid)); fflush(stdout); }
   }
   YABFRONT("retro_run:begin");
#endif

   if (!all_devices_ready)
   {
      // Running first frame, so we can assume all devices id were set
      // Let's send input descriptors
      all_devices_ready = true;
      set_descriptors();
   }

   /* Input: every libretro input callback happens HERE, on the frontend thread
    * (design §7 #4).  Audio no longer passes through this thread at all -- the
    * sound thread writes it itself (pirry's shape, see SNDLIBRETROUpdateAudio). */
   yab_input_snapshot();

   /* Renderer (design §1/§5): the machine runs on the emu thread and dispatches
    * one field at its VOUT point (line ~5); this thread renders exactly that
    * field - the queued PTMR=1 direct draws, then VOUT (Vdp1Draw /
    * Vdp2DrawScreens / Vdp1DrawEnd) and VIN (Vdp2DrawEnd -> video_cb).  All GL
    * and the frontend's hw-render ring stay on this thread. */
   if (yk.res_pending)
   {
      /* Deferred resolution change: outside the render call stack and with the
       * machine parked, so YglGLInit() does not tear down GL objects a frame in
       * progress is using. */
      yk.res_pending = 0;
      yab_emu_pause();
      /* PORT: the engine's GL objects live in ctx_sub (the worker's context).
       * This thread holds the frontend's context, so the resize has to borrow
       * ctx_sub for the call -- otherwise the new FBOs/textures are created in
       * the frontend context, where the renderer cannot see them. */
      yk_attach();
      retro_set_resolution();
      yk_detach();
      yab_emu_resume();
   }
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      /* Option changes rebuild GL objects and may resize the engine's
       * framebuffers: park the machine so it is not mid-frame. */
      int prev_resolution_mode = resolution_mode;
      int prev_multitap[2] = {multitap[0],multitap[1]};
      bool prev_service_enabled = service_enabled;
      yab_emu_pause();
      check_variables();
      if(prev_resolution_mode != resolution_mode)
         retro_set_resolution();
      VIDCore->SetSettingValue(VDP_SETTING_RBG_RESOLUTION_MODE, g_rbg_resolution_mode);
      VIDCore->SetSettingValue(VDP_SETTING_RBG_USE_COMPUTESHADER, g_rbg_use_compute_shader);
      if(PERCore && (prev_multitap[0] != multitap[0] || prev_multitap[1] != multitap[1]
                     || prev_service_enabled != service_enabled))
         PERCore->Init();
      yab_emu_resume();
   }

   {
      /* PORT (pirry model): the machine thread runs the emulation and dispatches
       * a field at its VOUT point; the VdpProc worker rasterises it into the
       * mirror FBO (the frontend's current colour texture).  This thread waits
       * until that field retires (Vdp1MarkRenderRetired) and then presents. */
      static unsigned swap_seen = 0;
      int spins = 0;
      unsigned now;
      /* Wait for a *picture* (YuiSwapBuffers on the worker), not for a retire:
       * VOUT retires every field, but only fields that reach YglRender produce
       * a new picture, and the flag/false race between the two threads made
       * almost every field look like a duplicate. */
      for (;;)
      {
         now = __atomic_load_n(&yab_swap_count, __ATOMIC_ACQUIRE);
      /* One field of audio space per field, whether or not this field produced
       * a picture: the SCSP thread blocks in SNDLIBRETROGetAudioSpace() until
       * the frontend grants it, and a frameskip field must not starve the
       * sound (our fork's model; without this the thread produces nothing and
       * UpdateAudio is never called -- measured: silent core). */
      audio_size = soundlen;
         if (now != swap_seen) break;
         if (++spins > 500) break;          /* 5 s safety cap */
         usleep(1000);
      }
      now = __atomic_load_n(&yab_swap_count, __ATOMIC_ACQUIRE);

      {
         /* Present every field the worker composed -- that is what the standalone
          * does, and the earlier "only present when VDP1 produced a new frame"
          * gate was based on a premise that does not hold:
          *
          *   - ygles.c:4952 calls YuiSwapBuffers() UNCONDITIONALLY at the end of
          *     YglRender(), and VIDOGLVdp2DrawEnd() (vidogl.c:7028) calls
          *     YglRender() on every VIN, so the standalone swaps the display 60
          *     times/s regardless of VDP1 frame changes.
          *   - vdp2.cpp's Vdp2VBlankOUT runs VIDCore->Vdp2DrawScreens() on every
          *     field too (only the VDP1 layer is double-buffered), so the
          *     composition IS a new picture even when VDP1 did not swap: the VDP2
          *     backgrounds/line-scroll move on.
          *   - No intermediate VDP1 state can reach the screen: the composition
          *     samples vdp1FrameBuff[_Ygl->readframe], the buffer VDP1 finished
          *     writing, never the one it is drawing into.
          *
          * Measured with the gate (device log SATURN.txt, v1gate core): in a
          * static scene [V1] shows vdp1=0/cmds=0 for seconds and the frontend
          * reports "6 new, 296 dupe (M:60.0 P:43.6)" -- i.e. the display stopped
          * updating while the machine kept running.  Without it the same log
          * shows "298 new, 0 dupe (M:60.4 P:60.4)". */
         if (now != swap_seen)
         {
            swap_seen = now;
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, current_width, current_height, 0);
         }
         else
         {
            /* Safety-cap path only (the worker produced no picture within the
             * wait window): a dupe keeps the frontend's clock alive. */
            video_cb(NULL, current_width, current_height, 0);
         }
      }
      /* The frontend advanced its hw-render ring inside that video_cb. */
      yk_adopt_front_fbo();
#if YAB_TRACE_LEVEL >= 2
      /* L2 probe [V1]: the numbers the standalone's OSD shows, measured the same
       * way -- VDP1 frame changes per second (vdp1_frame), commands drawn
       * (g_v1_drawn) and fields retired.  Once per second. */
      {
         /* vdp2.cpp's FPSDisplay() latches the per-second counters into
          * show_vdp1_frame / show_skipped_frame and resets the raw ones, so read
          * the latched values (that is exactly what the standalone's OSD shows). */
         extern int show_vdp1_frame;
         extern u32 show_skipped_frame;
         extern unsigned int g_v1_drawn;
         extern int YabVdp1RetiredCount(void);
         static unsigned last_s = 0;
         unsigned now_s = (unsigned)(YabauseGetTicks() / 1000000ULL);
         if (now_s != last_s)
         {
            printf("[V1] t=%u vdp1=%d skip=%u cmds=%u walks=%d\n",
                   now_s, show_vdp1_frame, (unsigned)show_skipped_frame,
                   g_v1_drawn, YabVdp1RetiredCount());
            fflush(stdout);
            last_s = now_s;
         }
      }
#endif /* YAB_TRACE_LEVEL >= 2 */
      /* Release the render thread (it blocks in YuiSwapBuffers until the frame
       * is presented, like the standalone's SDL swap). */
      __atomic_store_n(&yab_present_ack, now, __ATOMIC_RELEASE);
      YABFRONT("retro_run:end");
#if YAB_TRACE_LEVEL >= 2
      yab_ft_report();
#endif
   }
   yab_in_run = 0;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (!pwcs)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif
