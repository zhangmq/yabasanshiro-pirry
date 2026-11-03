/* PORT-TO-PIRRY-2026-11-01.md §5: platform helpers of our fork that have no
 * counterpart in pirry's core (yet).  Everything here is deliberately a no-op:
 *  - the frame protocol of our fork (seq/J1/J2) is replaced by pirry's own
 *    Vdp1JoinPendingRenders/Vdp1MarkRenderRetired; the frontend heartbeat lives
 *    in libretro.c (yab_front_beat, pause-on-menu watchdog);
 *  - VDP1 framebuffer readback and the CPU-clock option are not ported. */
#include "yabause.h"

void yab_frame_abort(void)
{
   /* Release the dispatch joins: the machine may be waiting for a retire that
    * only retro_run produces, and the frontend is about to stop calling it
    * (pause, savestate load, resize, shutdown). */
   extern void Vdp2AbortJoins(int);
   Vdp2AbortJoins(1);
}

/* Counterpart of yab_frame_abort(), called when the frontend resumes.  Without
 * it the abort stayed set for the rest of the session: the joins stopped
 * waiting, Vdp2JoinPendingVIN() dropped its token every time, and the machine
 * ran away from the presenter (measured: far fewer presents after any pause,
 * and a savestate load - which pauses - wedged). */
void yab_frame_resume(void)
{
   extern void Vdp2AbortJoins(int);
   Vdp2AbortJoins(0);
}
void YabSetRenderThread(void) {}
void YabauseApplyCpuClock(void) {}

int  YglVdp1ServiceReadback(void) { return 0; }
void YglVdp1ReadbackAbort(void) {}
void YglVdp1ReadbackResume(void) {}
void YglVdp1ReadbackReset(void) {}
void YglVdp1PublishCpu(int df) { (void)df; }
