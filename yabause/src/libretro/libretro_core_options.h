/*  YabaSanshiro — libretro port: core options (v2 description table).

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
 * Core options, libretro v2 (SET_CORE_OPTIONS_V2), with the canonical
 * fallback chain down to SET_VARIABLES for old front ends.
 *
 * The option set merges two sources (see .notes/CURRENT/REQ-CORE-OPTIONS-20261111.md):
 * the pcsx-style table the h700 branch of projects/yaba carries, and the knobs the
 * standalone's in-game menu exposes.  Naming follows the libretro convention: an
 * option whose value is only read while the core (re)loads a game carries
 * "(restart)" in its name; the front end cannot restart a core by itself, the
 * text is the only signal it can show.
 *
 * `desc` is the short name a front end lists, `info` the one-line description.
 */

#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

static struct retro_core_option_v2_category option_cats_us[] = {
   {
      "video",
      "Video",
      "Internal resolution, rotated backgrounds, frame pacing."
   },
   {
      "system",
      "System",
      "BIOS, cartridge, CPU core, language."
   },
   {
      "audio",
      "Audio",
      "SCSP options and volume."
   },
   {
      "input",
      "Input",
      "Multitap (6Player) adaptors."
   },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs_us[] = {
   /* ---- video --------------------------------------------------------- */
   {
      "yabasanshiro_video_core",
      "Video Core (restart)",
      NULL,
      "OpenGL ES 3 is the only renderer this core wires up.",
      NULL,
      "video",
      {
         { "opengl", "OpenGL ES 3" },
         { NULL, NULL },
      },
      "opengl"
   },
   {
      "yabasanshiro_videoformattype",
      "Video Format (restart)",
      NULL,
      "Auto reads the disc region; PAL is 50 Hz.",
      NULL,
      "video",
      {
         { "auto", "Auto" },
         { "NTSC", NULL },
         { "PAL",  NULL },
         { NULL, NULL },
      },
      "auto"
   },
   {
      "yabasanshiro_resolution_mode",
      "Resolution Mode",
      NULL,
      "Main screen internal resolution.",
      NULL,
      "video",
      {
         { "original", "Native" },
         { "2x",       "2x" },
         { "4x",       "4x" },
         { NULL, NULL },
      },
      "original"
   },
   {
      "yabasanshiro_rbg_resolution_mode",
      "RBG Resolution Mode",
      NULL,
      "Rotated background (RBG0) resolution.",
      NULL,
      "video",
      {
         { "original", "Native" },
         { "2x",       "2x" },
         { "720p",     "720p" },
         { "1080p",    "1080p" },
         { NULL, NULL },
      },
      "original"
   },
   {
      "yabasanshiro_rbg_use_compute_shader",
      "RBG Compute Shader",
      NULL,
      "Render rotated backgrounds on GPU compute.",
      NULL,
      "video",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "yabasanshiro_texcache_persist",
      "Texture Cache Persist",
      NULL,
      "Keep the texture atlas between frames.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "yabasanshiro_interlace_half",
      "Interlace Backgrounds",
      NULL,
      "Halve background lines in 448p+ modes (faster).",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "yabasanshiro_frameskip",
      "Auto Frameskip",
      NULL,
      "Skip drawing when a field misses its budget.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "auto",     NULL },
         { NULL, NULL },
      },
      "disabled"
   },

   /* ---- system -------------------------------------------------------- */
   {
      "yabasanshiro_sh2coretype",
      "SH2 Core (restart)",
      NULL,
      "Dynarec is faster, interpreter is slower.",
      NULL,
      "system",
      {
         { "dynarec",     "Dynarec (JIT)" },
         { "interpreter", "Interpreter" },
         { NULL, NULL },
      },
      "dynarec"
   },
   {
      "yabasanshiro_addon_cart",
      "Addon Cartridge (restart)",
      NULL,
      "Expansion RAM cartridge, None if unused.",
      NULL,
      "system",
      {
         { "4M_extended_ram", "4 MB Extended RAM" },
         { "1M_extended_ram", "1 MB Extended RAM" },
         { "none",            "None" },
         { NULL, NULL },
      },
      "4M_extended_ram"
   },
   {
      "yabasanshiro_force_hle_bios",
      "Force HLE BIOS (restart)",
      NULL,
      "Ignore the BIOS image, use the built-in one.",
      NULL,
      "system",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "yabasanshiro_system_language",
      "System Language (restart)",
      NULL,
      "SMPC language reported to software.",
      NULL,
      "system",
      {
         { "english",  "English" },
         { "german",   "Deutsch" },
         { "french",   "Français" },
         { "spanish",  "Español" },
         { "italian",  "Italiano" },
         { "japanese", "Japanese" },
         { NULL, NULL },
      },
      "english"
   },
   /* ---- audio --------------------------------------------------------- */
   {
      "yabasanshiro_scsp_dsp",
      "SCSP DSP",
      NULL,
      "SCSP effects (reverb); Off runs cooler.",
      NULL,
      "audio",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "yabasanshiro_cd_volume",
      "CD Music Volume",
      NULL,
      "CD-DA volume; 0 mutes CD audio.",
      NULL,
      "audio",
      {
         { "0",   "Off" },
         { "25",  NULL },
         { "50",  NULL },
         { "75",  NULL },
         { "100", NULL },
         { "125", NULL },
         { "150", NULL },
         { NULL, NULL },
      },
      "100"
   },
   {
      "yabasanshiro_sfx_volume",
      "Sound Effects Volume",
      NULL,
      "SCSP sound effects volume.",
      NULL,
      "audio",
      {
         { "0",   "Off" },
         { "25",  NULL },
         { "50",  NULL },
         { "75",  NULL },
         { "100", NULL },
         { "125", NULL },
         { "150", NULL },
         { NULL, NULL },
      },
      "100"
   },

   {
      "yabasanshiro_stv_region",
      "ST-V BIOS Region (restart)",
      NULL,
      "ST-V only: which region's BIOS from stvbios.zip to prefer. The cart "
      "header limits the choice, some Japanese titles only boot on JP.",
      NULL,
      "system",
      {
         { "EU", "Europe" },
         { "US", "USA" },
         { "JP", "Japan" },
         { "TW", "Taiwan" },
         { NULL, NULL },
      },
      "EU"
   },

   /* ---- input --------------------------------------------------------- */
   {
      "yabasanshiro_multitap_port1",
      "6Player Adaptor (Port 1)",
      NULL,
      "Enable a 6-player adaptor on controller port 1.",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "yabasanshiro_multitap_port2",
      "6Player Adaptor (Port 2)",
      NULL,
      "Enable a 6-player adaptor on controller port 2.",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "yabasanshiro_service_enabled",
      "Service Mode (ST-V)",
      NULL,
      "ST-V only: expose the cabinet Test/Service/Pause keys "
      "(L2/R2/L3 by default).",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },

   { NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL }
};

static struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/* Canonical setter, same fallback chain as the reference cores:
 * v2+intl -> v2 -> v1 core options -> SET_VARIABLES. */
static void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version = 0;

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      struct retro_core_options_v2_intl core_options_intl;
      core_options_intl.us    = &options_us;
      core_options_intl.local = NULL;

      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL,
            &core_options_intl);

      if (!*categories_supported)
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
   }
   else
   {
      size_t i, j;
      size_t option_index = 0;
      size_t num_options  = 0;
      struct retro_core_option_definition *option_defs_v1 = NULL;
      struct retro_variable *variables = NULL;
      char **values_buf = NULL;

      while (option_defs_us[num_options].key)
         num_options++;

      option_defs_v1 = (struct retro_core_option_definition *)
            calloc(num_options + 1, sizeof(struct retro_core_option_definition));
      if (!option_defs_v1)
         return;

      for (i = 0; i < num_options; i++)
      {
         option_defs_v1[i].key           = option_defs_us[i].key;
         option_defs_v1[i].desc          = option_defs_us[i].desc;
         option_defs_v1[i].info          = option_defs_us[i].info;
         option_defs_v1[i].default_value = option_defs_us[i].default_value;
         for (j = 0; j < 128; j++)
         {
            option_defs_v1[i].values[j] = option_defs_us[i].values[j];
            if (!option_defs_us[i].values[j].value)
               break;
         }
      }

      if (!environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_defs_v1))
      {
         variables  = (struct retro_variable *)calloc(num_options + 1, sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (variables && values_buf)
         {
            for (i = 0; i < num_options; i++)
            {
               size_t buf_len = strlen(option_defs_us[i].desc) + 1;

               for (j = 0; j < 128; j++)
               {
                  const char *value = option_defs_us[i].values[j].value;
                  if (!value) break;
                  buf_len += strlen(value) + 1;
               }

               values_buf[option_index] = (char *)malloc(buf_len + 1);
               if (values_buf[option_index])
               {
                  char *buf = values_buf[option_index];
                  buf[0] = '\0';
                  strcat(buf, option_defs_us[i].desc);
                  strcat(buf, "; ");
                  for (j = 0; j < 128; j++)
                  {
                     const char *value = option_defs_us[i].values[j].value;
                     if (!value) break;
                     strcat(buf, value);
                     if (option_defs_us[i].values[j + 1].value)
                        strcat(buf, "|");
                  }
                  variables[option_index].key   = option_defs_us[i].key;
                  variables[option_index].value = buf;
                  option_index++;
               }
            }
            environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
         }
      }

      for (i = 0; i < num_options; i++)
         if (values_buf && values_buf[i]) free(values_buf[i]);
      free(values_buf);
      free(variables);
      free(option_defs_v1);
   }
}

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_CORE_OPTIONS_H__ */
