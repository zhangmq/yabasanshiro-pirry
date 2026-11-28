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
   SH-2 dynarec: host backend selection.

   Everything that depends on the host lives in one of the headers below:

     - the byte sizes of the routines in the host helper library (dynalib_*).
       The emitter charges them while it checks whether a block still fits,
       i.e. before any code is emitted;
     - the block-exit jump snippets (internal_jmp / internal_delay_jmp), which
       exist as assembly on AArch64 and only as placeholders elsewhere;
     - the instruction cache flush (dyn_cacheflush.h).

   The emitter itself (DynarecSh2.cpp) is written against these names, so
   supporting another host means adding one header here plus its helper
   library.  A host without a backend must fail at this point instead of
   silently compiling with another host's numbers: those numbers are byte
   counts of hand-written assembly, and a mismatch corrupts the emitted code
   rather than failing to build.
*/

#ifndef DYN_BACKEND_H
#define DYN_BACKEND_H

/* DYN_BACKEND_X64 is set by the build system next to the x86-64 helper
   library, not derived from __x86_64__: compiling the dynarec on a host whose
   helper library is not linked (a front end that does not ship dynalib_x64.S)
   must stop here, not silently use another host's byte counts. */
#if defined(_WINDOWS)
#include "dyn_backend_x86.h"
#elif defined(AARCH64)
#include "dyn_backend_arm64.h"
#elif defined(__arm__)
#include "dyn_backend_arm32.h"
#elif defined(DYN_BACKEND_X64)
#include "dyn_backend_x64.h"
#else
#error "SH-2 dynarec: no host backend for this target (see backend/)"
#endif

#include "dyn_cacheflush.h"

#endif /* DYN_BACKEND_H */
