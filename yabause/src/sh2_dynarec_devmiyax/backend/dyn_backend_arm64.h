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
   AArch64 backend: the helper library is dynalib_arm64.s.

   The sizes below are byte counts of the routines in that file and are part
   of the contract between it and the emitter: the block prologue is copied
   verbatim, the separators are the padding between two SH-2 instructions, and
   the clock offsets say where inside a separator the cycle counter sits.  If
   the assembly changes, these numbers change with it.
*/

#ifndef DYN_BACKEND_ARM64_H
#define DYN_BACKEND_ARM64_H

#define PROLOGSIZE		     (11*4)
#define SEPERATORSIZE_NORMAL (2*4)
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  (18*4)
#define SEPERATORSIZE_DELAY_SLOT  (15*4)
#define SEPERATORSIZE_DELAY_AFTER  (12*4)
#define DALAY_CLOCK_OFFSET (2*4)
#define DALAY_CLOCK_OFFSET_DEBUG (10*4)
#define SEPERATORSIZE_DELAYD_DEBUG (20*4)
#define EPILOGSIZE		      (10*4)
#define DELAYJUMPSIZE	     (11*4)

/* Block-exit jumps: real routines in dynalib_arm64.s (the emitter copies them
   and patches the destination at the offset below).  They are plain C symbols
   in the assembly, so the declaration has to say so: the emitter takes their
   address, and a mangled reference fails when the core is loaded, not when it
   is built. */
extern "C" {
void internal_jmp(void);
void internal_delay_jmp(void);
}

#define DYN_INTERNAL_JMP_SIZE             (18*4)
#define DYN_INTERNAL_DELAY_JMP_SIZE       (15*4)
#define DYN_INTERNAL_JMP_TO_OFFSET        (4*4)
#define DYN_INTERNAL_DELAY_JMP_TO_OFFSET  (4*4)

#endif /* DYN_BACKEND_ARM64_H */
