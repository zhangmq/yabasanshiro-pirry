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
   ARMv7 backend: dynalib_arm.s.

   The block-exit jump snippets do not exist in this helper library, so they
   stay placeholders (unchanged from the pre-backend code); the sizes kept here
   are the historical values and are not used by the ARMv7 path.
*/

#ifndef DYN_BACKEND_ARM32_H
#define DYN_BACKEND_ARM32_H

#include <stdio.h>

#define PROLOGSIZE		     16
#define SEPERATORSIZE_NORMAL 8
#define NORMAL_CLOCK_OFFSET 4
#define NORMAL_CLOCK_OFFSET_DEBUG 4
#define SEPERATORSIZE_DEBUG  36
#define SEPERATORSIZE_DELAY_SLOT  36
#define SEPERATORSIZE_DELAY_AFTER  20
#define DALAY_CLOCK_OFFSET 8
#define DALAY_CLOCK_OFFSET_DEBUG 8
#define SEPERATORSIZE_DELAYD_DEBUG 60
#define EPILOGSIZE		      12
#define DELAYJUMPSIZE	     32

extern "C" {
void internal_jmp(void){ printf("nope"); }
void internal_delay_jmp(void){ printf("nope"); }
}

#define DYN_INTERNAL_JMP_SIZE             (18*4)
#define DYN_INTERNAL_DELAY_JMP_SIZE       (15*4)
#define DYN_INTERNAL_JMP_TO_OFFSET        (4*4)
#define DYN_INTERNAL_DELAY_JMP_TO_OFFSET  (4*4)

#endif /* DYN_BACKEND_ARM32_H */
