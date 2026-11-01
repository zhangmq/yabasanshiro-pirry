/* PORT-TO-PIRRY-2026-11-01.md §5: platform helpers of our fork that have no
 * counterpart in pirry's core (yet).  Everything here is deliberately a no-op:
 *  - the frame/pause protocol (our seq/J1/J2 + frontend heartbeat) is replaced by
 *    pirry's own Vdp1JoinPendingRenders/Vdp1MarkRenderRetired; the pause-on-menu
 *    behaviour is not ported yet, so the machine keeps running while the
 *    frontend is away (pirry's standalone does the same until VdpRevoke);
 *  - VDP1 framebuffer readback and the CPU-clock option are not ported. */
#include "yabause.h"

void yab_frame_abort(void) {}
void yab_frame_resume(void) {}
void yab_front_beat(void) {}
void YabSetRenderThread(void) {}
void YabauseApplyCpuClock(void) {}

int  YglVdp1ServiceReadback(void) { return 0; }
void YglVdp1ReadbackAbort(void) {}
void YglVdp1ReadbackResume(void) {}
void YglVdp1ReadbackReset(void) {}
void YglVdp1PublishCpu(int df) { (void)df; }
