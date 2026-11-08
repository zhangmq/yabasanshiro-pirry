/* Port-owned globals and helpers the engine expects (retro_arena/main.cpp does
   this for the standalone).  Kept minimal: input/audio stay dummy for the
   emulation+render bring-up. */
#include <stddef.h>
#include "yabause.h"

int g_emulated_bios = 1;
int g_scsp_sync = 1;
int g_frame_skip = 1;

int YabauseThread_IsUseBios(void) { return g_emulated_bios; }
const char *YabauseThread_getBackupPath(void) { return NULL; }
void YabauseThread_setUseBios(int use) { (void)use; }
void YabauseThread_setBackupPath(const char *buf) { (void)buf; }
void YabauseThread_coldBoot(void) {}
void YabauseThread_resetPlaymode(void) {}

/* OSD hook the engine calls every frame when the retro_arena port defines it. */
void OSDNanovgDrawRAOverlay(void) {}

/* musashi disassembler hooks (trace only). */
unsigned int m68k_read_disassembler_16(unsigned int addr) { (void)addr; return 0; }
unsigned int m68k_read_disassembler_32(unsigned int addr) { (void)addr; return 0; }
