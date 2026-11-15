/* Acclaim RAX sound board (Batman Forever, ST-V only).

   Batman Forever is the one ST-V cart that carries an extra sound board: an
   ADSP-2181 (16.67 MHz) with its own 16 MiB ROM (512 KiB program bank plus four
   2 MiB sample banks).  Both SH-2s reach it through the A-bus CS1 window at
   0x04800000..0x04800003 (a 32-bit write carries one 16-bit command word in its
   high half).  The board's DAC output goes straight to the speakers.

   This is a port of MAME's src/mame/shared/rax.cpp (BSD-3-Clause, Phil Bennett)
   onto the current tree: the MAME device framework is replaced by plain state,
   MAME's attotime timers become counters in DSP clock cycles driven by
   RaxAdvance(), and the 2x DMADAC pair becomes a stereo FIFO that the SCSP
   output adapter mixes at 44.1 kHz.

   Design notes (why it is shaped this way) are in
   .notes/CURRENT/RAX-PLAN-20261114.md.  The one rule that matters: the DSP is
   clocked by the same emulated machine time as the SH-2 and never runs ahead of
   the host's command stream (the sealed first attempt failed exactly there). */
#ifndef RAX_H
#define RAX_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the loaded cart has the board (batmanfr) and its ROM is present. */
int RaxIsPresent(void);

/* Copy one blob of the cart's RAX ROM image (offsets are MAME's).  Called by
   the ST-V romset loader before RaxInit(). */
void RaxLoadRom(unsigned int offset, const unsigned char *data, unsigned int length);

/* Build/reset the board from the blobs loaded so far.  Safe to call twice; a
   board without its program blob stays absent (RaxIsPresent() == 0). */
int RaxInit(void);

/* Drop everything (cart unload / core shutdown). */
void RaxShutdown(void);

/* Machine reset (SH-2 reset button): MAME's device_reset() -- reload the 32
   boot words from the ROM and reset the DSP core. */
void RaxReset(void);

/* Advance the DSP by the emulated time that `sh2_cycles` SH-2 cycles stand for
   (16.67 MHz against the SH-2's 28.63636 MHz).  Called once per emulation pass
   from YabauseEmulate(). */
void RaxAdvance(unsigned int sh2_cycles);

/* SH-2 -> RAX: the high 16 bits of a 32-bit write to CS1 0x04800000..3.
   Writes the latch and raises IRQL0; the word is timestamped and handed to the
   DSP worker, which delivers it when its own clock reaches that cycle (the
   synchronous fallback runs 83 cycles = MAME's perfect_quantum(5us) instead, so
   the command is consumed before the SH-2 continues). */
void RaxDataWrite(unsigned short data);
/* Same, but records whether the SH-2 used a 16-bit store (MAME ignores those). */
void RaxDataWriteEx(unsigned short data, int word16);

/* Add the board's DAC output to `frames` stereo frames of the SCSP's planar
   s32 buffers (the sndlibretro adapter's mix point).  Does nothing when the
   board is absent or its DAC is disabled. */
void RaxMix(int *left, int *right, unsigned int frames);

/* Savestate hook (phase 2; the current memory.c format has no RAX chunk). */
int RaxSaveState(void *fp);
int RaxLoadState(void *fp);

#ifdef __cplusplus
}
#endif

#endif
