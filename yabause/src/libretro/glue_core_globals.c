/* Core globals that our libretro glue references but pirry's core does not have.
 * They are per-build switches, not emulation state: see PORT-TO-PIRRY-2026-11-01.md. */
#include "yabause.h"

/* The renderer always runs on the VdpProc thread in this port, so the glue's
 * "async renderer" branches are the live ones. */
int g_vdp_async = 1;
/* VDP1 framebuffer readback is not ported (off by default and no title in the
 * library uses it). */
int yab_vdp1_fb_readback = 0;
/* CPU clock scaling (our overclock option) is not ported; 100 = stock. */
int yab_cpu_clock_pct = 100;
int yab_cpu_clock_env = 0;

/* Only the standalone's frontend files reference this (retro_arena/main.cpp,
 * nx/main.cpp, MenuScreen.cpp) -- the engine itself does not, so this definition
 * exists only to satisfy the link.  A libretro core boots a real BIOS (0). */
int g_emulated_bios = 0;
