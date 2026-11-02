/* Sega 315-5838 / 317-0229 / 317-0230 / 317-0231 compression + encryption
 *
 * Ported from MAME src/mame/sega/315-5838_317-0229_comp.cpp (David Haywood,
 * Samuel Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard).  The ST-V ROM
 * board puts this chip on the A-bus CS0 window; the SH-2 feeds it encrypted
 * source words and reads decompressed bytes back.  Without it the cartridge
 * data reads as garbage (DecAthlete: full-screen noise).
 */
#ifndef STVCOMP_H
#define STVCOMP_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
   STVCOMP_315_5838 = 0,   /* DecAthlete (decathlt / decathlto) */
   STVCOMP_317_0229 = 1,   /* Dead or Alive, Name Club 1/2 */
   STVCOMP_317_0230 = 2,   /* Print Club Pooh 2/3, Nightmare Before Christmas */
   STVCOMP_317_0231 = 3    /* Print Club Love Love 1/2, Sony Creative 5/6 */
};

/* The chip pulls its encrypted words from the cart image through this
   callback: word_addr is the 16-bit word index inside the current 8 MiB
   bank; the caller reads the cart image and returns the big-endian word. */
typedef u16 (*STVCompSourceFn)(u32 word_addr);
void STVCompSetSourceReader(STVCompSourceFn fn);

void STVCompEnable(int variant);   /* reset the chip and enable it for this game */
void STVCompDisable(void);
int  STVCompIsEnabled(void);

void STVCompSetBank(int bank);     /* selected by bits 23-24 of a CS0 write */
int  STVCompGetBank(void);

void STVCompSetControl(u16 val);   /* 0x04200000; bit 1 = decoder output */
int  STVCompDecodeEnabled(void);

void STVCompSourceWrite(u32 data, u32 mem_mask); /* 0x...7FFFF0 (long, or two 16-bit halves) */
void STVCompTableWrite(u32 offset, u16 data); /* 0x...7FFFF4, offset 0/1 */
u16  STVCompDataRead(void);                 /* 0x...7FFFF8 */

#ifdef __cplusplus
}
#endif

#endif
