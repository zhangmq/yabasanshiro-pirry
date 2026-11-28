/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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
   x86-64 backend: the helper library is dynalib_x64.S.

   The numbers below are byte counts measured from that file (nm -S); the
   emitter charges them before it emits anything, and it copies exactly
   PROLOGSIZE / EPILOGSIZE / SEPERATORSIZE_* / DELAYJUMPSIZE bytes, so a
   change in the assembly has to be mirrored here.

   Patch offsets point at the byte of the immediate that carries a value:
   the cycle count is a single byte inside the `add r13d,imm8` of a separator
   (hence the +3 of the instruction), the operand registers are imm32 at the
   start of the two `mov` instructions in the op snippet.
*/

#ifndef DYN_BACKEND_X64_H
#define DYN_BACKEND_X64_H

/* x86-64 is a TEST target.  It exists so the device's AArch64 dynarec can be
   reproduced on a PC host (RetroArch) through the same code paths: it is a
   bug-for-bug replica of the AArch64 backend, including the debug-path
   defects, so a difference seen on the host is a difference of the core and
   not of the host.  No release or installer ships it and it is not supported
   as a production backend; AArch64 remains the reference.  Build it only for
   testing: -DDYN_BACKEND_X64 plus dynalib_x64.S. */

#define PROLOGSIZE                   31
#define EPILOGSIZE                   22
#define SEPERATORSIZE_NORMAL         8
#define NORMAL_CLOCK_OFFSET          7     /* add r13d,imm8 in seperator_normal */
#define NORMAL_CLOCK_OFFSET_DEBUG    7
/* Bug-for-bug with the AArch64 backend: its debug fragments are 4 bytes
   longer than the copy sizes, so the tail instruction is never copied
   (seperator_d_normal loses the branch-decision restore, seperator_d_delay
   loses the -2).  The x64 fragments are cut to the same behaviour. */
#define SEPERATORSIZE_DEBUG          48
#define SEPERATORSIZE_DELAY_SLOT     41
#define SEPERATORSIZE_DELAY_AFTER    47
#define DALAY_CLOCK_OFFSET           11    /* add r13d,imm8 in seperator_delay_after */
/* Same bug as AArch64: the debug path patches the stack-restore instruction
   instead of the cycle slot, so a taken delayed branch leaks `cycle` bytes of
   stack.  The imm8 is at offset 44 (add rsp,0 right before jmp rax). */
#define DALAY_CLOCK_OFFSET_DEBUG     44
#define SEPERATORSIZE_DELAYD_DEBUG   61
#define DELAYJUMPSIZE                26    /* PageFlip / PageFlip_k */

/* Block-exit jumps: fragments in dynalib_x64.S.  internal_jmp and
   internal_delay_jmp end with a `jmp rel32` whose displacement the emitter
   patches at DYN_INTERNAL_*_JMP_TO_OFFSET (the byte after the e9 opcode). */
extern "C" {
void internal_jmp(void);
void internal_delay_jmp(void);
}

#define DYN_INTERNAL_JMP_SIZE             49
#define DYN_INTERNAL_DELAY_JMP_SIZE       44
#define DYN_INTERNAL_JMP_TO_OFFSET        13
#define DYN_INTERNAL_DELAY_JMP_TO_OFFSET  18

#endif /* DYN_BACKEND_X64_H */
