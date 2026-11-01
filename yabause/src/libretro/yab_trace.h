/* Compile-time probe levels for this libretro port.
 *
 * Every diagnostic print in the port is behind one of these macros, so a build
 * with YAB_TRACE=0 (the default) carries no probe cost at all: no branch, no
 * format string, no timing calls, no atomics.  Pick the level at build time:
 *
 *   make platform=arm64 FASTMATH=0 YAB_TRACE=0   (default) no probes
 *   make platform=arm64 FASTMATH=0 YAB_TRACE=1   state changes      < 1/s
 *   make platform=arm64 FASTMATH=0 YAB_TRACE=2   periodic summaries ~1/s
 *   make platform=arm64 FASTMATH=0 YAB_TRACE=3   per frame          50-60/s
 *   make platform=arm64 FASTMATH=0 YAB_TRACE=4   hot paths          >= 1000/s
 *
 * Level N includes all lower levels.  The split is by FREQUENCY and therefore
 * by cost:
 *
 *   L1  [res] resolution/mode changes, [YK] context+ring setup, [CFG] the
 *       effective-options dump, [CTX] context borrow/hand-back.  Cheap enough
 *       to leave on for a whole session.
 *   L2  [AUD] audio callback timing, [V1] the once-a-second VDP1 counters,
 *       [FT]/[MW] the machine/render split every N frames.  ~1/s each; costs a
 *       clock read per frame while enabled.
 *   L3  per-frame probes: [FCRC] frame CRCs (one glReadPixels per requested
 *       frame), [FT] with N=1.  Distorts pacing, use for captures.
 *   L4  reserved for genuinely hot paths (per line / per VDP1 command).  The
 *       engine-side ones live behind FRAME_DEBUG (debug.h) and are not part of
 *       this scale.
 *
 * Runtime env vars remain only where they carry a VALUE (YAB_FT=<frames> picks
 * the [FT] interval, YAB_FRAMECRC=<frames> picks which frames to hash); they
 * are filters on top of the level, never a substitute for it.
 */
#ifndef YAB_TRACE_H
#define YAB_TRACE_H

#include <stdio.h>

#ifndef YAB_TRACE_LEVEL
#define YAB_TRACE_LEVEL 0
#endif

#if YAB_TRACE_LEVEL >= 1
#define YAB_TR_L1(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define YAB_TR_L1(...) ((void)0)
#endif

#if YAB_TRACE_LEVEL >= 2
#define YAB_TR_L2(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define YAB_TR_L2(...) ((void)0)
#endif

#if YAB_TRACE_LEVEL >= 3
#define YAB_TR_L3(...) do { printf(__VA_ARGS__); } while (0)
#else
#define YAB_TR_L3(...) ((void)0)
#endif

#if YAB_TRACE_LEVEL >= 4
#define YAB_TR_L4(...) do { printf(__VA_ARGS__); } while (0)
#else
#define YAB_TR_L4(...) ((void)0)
#endif

#endif /* YAB_TRACE_H */
