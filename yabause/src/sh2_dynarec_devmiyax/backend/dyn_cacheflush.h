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
   Instruction cache flush for freshly emitted JIT code.

   The AArch64 version does the work by hand instead of using
   __builtin___clear_cache: on big.LITTLE the compiler caches the line sizes it
   read on one core and then applies them to another, which is wrong, and the
   store/instruction maintenance has to be the minimum of both cores' line
   sizes.  "dc civac" rather than "cvau" is the suggested workaround for
   Cortex-A53 errata 819472, 826319, 827319 and 824069.

   x86 needs no flush (coherent instruction cache); the generic branch is what
   a new host starts from.
*/

#ifndef DYN_CACHEFLUSH_H
#define DYN_CACHEFLUSH_H

#include <stdint.h>

#if !defined(_WINDOWS)
#if defined(__arm__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{ 
    const int syscall = 0xf0002;
      __asm __volatile (
     "mov   r0, %0\n"      
     "mov   r1, %1\n"      
     "mov   r7, %2\n"      
     "mov     r2, #0x0\n"      
     "svc     0x00000000\n"      
     :      
     : "r" (begin), "r" (end), "r" (syscall)      
     : "r0", "r1", "r7"      
    );      
}      
#elif defined(__aarch64__)
void cacheflush(uintptr_t begin, uintptr_t end, int flag )
{
    // Don't rely on GCC's __clear_cache implementation, as it caches
    // icache/dcache cache line sizes, that can vary between cores on
    // big.LITTLE architectures.
    uint64_t addr, ctr_el0;
    static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
    size_t isize, dsize;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    isize = 4 << ((ctr_el0 >> 0) & 0xf);
    dsize = 4 << ((ctr_el0 >> 16) & 0xf);

    // use the global minimum cache line size
    icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
    dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

    addr = (uint64_t)begin & ~(uint64_t)(dsize - 1);
    for (; addr < (uint64_t)end; addr += dsize)
        // use "civac" instead of "cvau", as this is the suggested workaround for
        // Cortex-A53 errata 819472, 826319, 827319 and 824069.
            __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    addr = (uint64_t)begin & ~(uint64_t)(isize - 1);
    for (; addr < (uint64_t)end; addr += isize)
            __asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
}
#else
void cacheflush(uintptr_t begin, uintptr_t end, int flag ){
  __builtin___clear_cache((void*)begin,(void*)end);
}
#endif
#endif

#endif /* DYN_CACHEFLUSH_H */
