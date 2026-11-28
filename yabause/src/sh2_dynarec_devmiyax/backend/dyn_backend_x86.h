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
   32-bit x86 backend (the Windows build, helper library dynalib via NASM).

   The block-exit jump snippets have no implementation here, so they stay
   placeholders (unchanged from the pre-backend code); the sizes kept here are
   the historical values and are not used by this path.
*/

#ifndef DYN_BACKEND_X86_H
#define DYN_BACKEND_X86_H

#include <stdio.h>

#define PROLOGSIZE		     27
#define EPILOGSIZE		      3
#define SEPERATORSIZE	     10
#define SEPERATORSIZE_NORMAL 7
#define SEPERATORSIZE_DEBUG  24
#define SEPERATORSIZE_DELAY  7
#define SEPERATORSIZE_DELAY_SLOT  27
#define SEPERATORSIZE_DELAY_AFTER  10
#define SEPERATORSIZE_DELAYD_DEBUG 34
#define DELAYJUMPSIZE	     17
#define DALAY_CLOCK_OFFSET 6
#define DALAY_CLOCK_OFFSET_DEBUG 6
#define NORMAL_CLOCK_OFFSET 6
#define NORMAL_CLOCK_OFFSET_DEBUG 3

extern "C" {
void internal_jmp(void){ printf("nope"); }
void internal_delay_jmp(void){ printf("nope"); }
}

#define DYN_INTERNAL_JMP_SIZE             (18*4)
#define DYN_INTERNAL_DELAY_JMP_SIZE       (15*4)
#define DYN_INTERNAL_JMP_TO_OFFSET        (4*4)
#define DYN_INTERNAL_DELAY_JMP_TO_OFFSET  (4*4)

#endif /* DYN_BACKEND_X86_H */
