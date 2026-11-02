/* PORT-TO-PIRRY-2026-11-01.md §5: platform helpers of our fork that have no
 * counterpart in pirry's core (yet).  Everything here is deliberately a no-op:
 *  - the frame protocol of our fork (seq/J1/J2) is replaced by pirry's own
 *    Vdp1JoinPendingRenders/Vdp1MarkRenderRetired; the frontend heartbeat lives
 *    in libretro.c (yab_front_beat, pause-on-menu watchdog);
 *  - VDP1 framebuffer readback and the CPU-clock option are not ported. */
#include "yabause.h"

void yab_frame_abort(void) {}
void yab_frame_resume(void) {}
void YabSetRenderThread(void) {}
void YabauseApplyCpuClock(void) {}

int  YglVdp1ServiceReadback(void) { return 0; }
void YglVdp1ReadbackAbort(void) {}
void YglVdp1ReadbackResume(void) {}
void YglVdp1ReadbackReset(void) {}
void YglVdp1PublishCpu(int df) { (void)df; }
