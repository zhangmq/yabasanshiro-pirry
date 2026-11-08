/*  YabaSanshiro — libretro port: logging layer (implementation).

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

#include "log.h"

#ifdef LOG_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MSG_MAX    1024
#define LOG_PREFIX_MAX 64

static LogSinkFn s_sink;
static int s_min_level = LOG_LVL_INFO;
static int s_prefix_len;
static char s_prefix[LOG_PREFIX_MAX + 1];
static int s_ready;

static const char *level_name(int level)
{
   switch (level)
   {
      case LOG_LVL_WARN:
         return "warn";
      case LOG_LVL_DEBUG:
         return "debug";
      default:
         return "info";
   }
}

/* Reads LOG_LEVEL and LOG_PREFIX once. Both are only consulted here, i.e.
   only by a LOG_ENABLED build. */
static void log_init(void)
{
   const char *env;

   env = getenv("LOG_LEVEL");
   if (env != NULL)
   {
      if (strcmp(env, "warn") == 0)
         s_min_level = LOG_LVL_WARN;
      else if (strcmp(env, "debug") == 0)
         s_min_level = LOG_LVL_DEBUG;
      else
         s_min_level = LOG_LVL_INFO; /* unset, "info" or invalid */
   }
   else
      s_min_level = LOG_LVL_INFO;

   env = getenv("LOG_PREFIX");
   s_prefix_len = 0;
   s_prefix[0] = '\0';
   if (env != NULL)
   {
      size_t n = strlen(env);

      if (n > LOG_PREFIX_MAX)
         n = LOG_PREFIX_MAX;
      memcpy(s_prefix, env, n);
      s_prefix[n] = '\0';
      s_prefix_len = (int)n;
   }

   s_ready = 1;
}

void LogSetSink(LogSinkFn fn)
{
   if (!s_ready)
      log_init();
   s_sink = fn;
}

int LogLevelEnabled(int level)
{
   if (!s_ready)
      log_init();
   return level >= s_min_level;
}

void LogVPrintf(int level, const char *fmt, va_list ap)
{
   char msg[LOG_MSG_MAX];
   char line[LOG_MSG_MAX + LOG_PREFIX_MAX + 32];

   if (!LogLevelEnabled(level))
      return;

   if (fmt == NULL)
      fmt = "(null)";
   vsnprintf(msg, sizeof(msg), fmt, ap);

   if (s_prefix_len > 0)
      snprintf(line, sizeof(line), "[%s][%s] %s\n", s_prefix, level_name(level), msg);
   else
      snprintf(line, sizeof(line), "[%s] %s\n", level_name(level), msg);

   if (s_sink != NULL)
      s_sink(level, line);
   else
   {
      fputs(line, stdout);
      fflush(stdout);
   }
}

void LogPrintf(int level, const char *fmt, ...)
{
   va_list ap;

   if (!LogLevelEnabled(level))
      return;

   va_start(ap, fmt);
   LogVPrintf(level, fmt, ap);
   va_end(ap);
}

#else /* LOG_ENABLED */

/* LOG_ENABLED off: nothing to compile, no symbols exported. */
typedef int log_module_disabled;

#endif /* LOG_ENABLED */
