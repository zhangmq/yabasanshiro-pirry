/*  YabaSanshiro — libretro port: logging layer.

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
 * Logging for the libretro build only. The standalone build does not define
 * LOG_ENABLED and compiles every call site away (zero cost).
 *
 * Compile time: LOG_ENABLED. When it is not defined this header expands the
 * macros to nothing and log.c compiles to an empty translation unit.
 *
 * Run time (only read by a LOG_ENABLED build):
 *   LOG_LEVEL   warn | info | debug, anything else means info (default info)
 *   LOG_PREFIX  custom text placed in front of the level
 *
 * Output format, one line per message:
 *   [<LOG_PREFIX>][warn|info|debug] message
 * With LOG_PREFIX unset or empty the first bracket is omitted:
 *   [info] message
 *
 * Output target is a sink: the libretro front end installs one that forwards
 * to its log_cb; when no front end is running (standalone emulator launch) the
 * default sink writes to standard output.
 */

#ifndef LIBRETRO_LOG_H
#define LIBRETRO_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Severity levels. The numeric values are the ones enum retro_log_level uses,
   so a sink can hand the level to log_cb unchanged. Higher is more severe. */
#define LOG_LVL_DEBUG 0
#define LOG_LVL_INFO  1
#define LOG_LVL_WARN  2

/* line is NUL terminated, includes the trailing newline, and is only valid
   for the duration of the call. */
typedef void (*LogSinkFn)(int level, const char *line);

#ifdef LOG_ENABLED

/* Installs the output sink. NULL restores the default (standard output). */
void LogSetSink(LogSinkFn fn);

/* Non-zero when messages of this level pass the LOG_LEVEL filter. */
int LogLevelEnabled(int level);

void LogVPrintf(int level, const char *fmt, va_list ap);
void LogPrintf(int level, const char *fmt, ...);

#define LOG_DEBUG(...) LogPrintf(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  LogPrintf(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_WARN(...)  LogPrintf(LOG_LVL_WARN, __VA_ARGS__)

#else /* LOG_ENABLED */

#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...)  ((void)0)
#define LOG_WARN(...)  ((void)0)
#define LogLevelEnabled(level) 0

#endif /* LOG_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_LOG_H */
