/* MAME compatibility shim for the ADSP-21xx core.
 *
 * yabause/src/adsp2100/2100ops.hxx is MAME's opcode file, unchanged
 * (license: BSD-3-Clause, copyright-holders: Aaron Giles).  MAME compiles it
 * inside a device_t subclass; we keep the class name and the member names so
 * the file needs no edits at all, and this header supplies the handful of
 * MAME services it uses (logerror, util::sext, countl_zero/one, the memory
 * access objects and the device callbacks).
 *
 * Everything here is deliberately tiny: no MAME code, no device framework.
 */
#ifndef ADSP2100_MAME_COMPAT_H
#define ADSP2100_MAME_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* MAME's logerror() is a debug channel; ours prints nothing (the probe
 * functions in this tree use explicit printf() when they are asked to). */
#define logerror(...) do { } while (0)

#ifndef ATTR_COLD
#define ATTR_COLD
#endif


/* Bit helpers used all over the ops file.
 *
 * MAME's BIT() is a *template* (src/lib/util/coretmpl.h): the extracted field
 * keeps the width of the argument.  These used to be `uint32_t BIT(uint32_t...)`
 * overloads, which silently truncated 64-bit arguments.  The MAC overflow test
 * is `BIT(res, 31, 9)` with `int64_t res`, so a truncated argument turned
 * *every negative MAC result* into "bits 31..39 = 0x001" -- i.e. every negative
 * product looked like an overflow, `IF MV SAT MR` then clamped it to 0x8000 and
 * the RAX sample decoder produced a constant -32768 (silent/humming DAC).
 * Keep the type, exactly like MAME. */
template <typename T, typename U>
constexpr T BIT(T x, U n) noexcept { return (T)((x >> n) & (T)1); }

template <typename T, typename U, typename V>
constexpr T BIT(T x, U n, V w) noexcept
{
   return (T)(((uint64_t)x >> n) &
              ((w >= (V)(8 * sizeof(T))) ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1)));
}

/* ADSP-2181 interrupt indices (MAME: adsp2100.h), used by 2100ops.hxx. */
enum
{
   ADSP2181_IRQ0      = 0,
   ADSP2181_SPORT1_RX = 0,
   ADSP2181_IRQ1      = 1,
   ADSP2181_SPORT1_TX = 1,
   ADSP2181_IRQ2      = 2,
   ADSP2181_SPORT0_RX = 3,
   ADSP2181_SPORT0_TX = 4,
   ADSP2181_TIMER     = 5,
   ADSP2181_IRQE      = 6,
   ADSP2181_BDMA      = 7,
   ADSP2181_IRQL1     = 8,
   ADSP2181_IRQL0     = 9
};


/* The ops file also references the 2101/2100/2105/2115 index names in the
 * shared code paths; they are the same indices as the 2181 set. */
enum
{
   ADSP2100_IRQ0 = 0, ADSP2100_IRQ1 = 1, ADSP2100_IRQ2 = 2, ADSP2100_IRQ3 = 3,
   ADSP2101_IRQ0 = 0, ADSP2101_IRQ1 = 1, ADSP2101_IRQ2 = 2,
   ADSP2101_SPORT0_RX = 3, ADSP2101_SPORT0_TX = 4, ADSP2101_TIMER = 5,
   ADSP2105_IRQ0 = 0, ADSP2105_IRQ1 = 1, ADSP2105_IRQ2 = 2, ADSP2105_TIMER = 5,
   ADSP2115_IRQ0 = 0, ADSP2115_IRQ1 = 1, ADSP2115_IRQ2 = 2,
   ADSP2115_SPORT0_RX = 3, ADSP2115_SPORT0_TX = 4, ADSP2115_TIMER = 5
};

namespace util
{
   /* sign-extend the low `bits` bits of `val` (MAME: util::sext) */
   inline int32_t sext(int32_t val, int bits)
   {
      return (int32_t)((uint32_t)val << (32 - bits)) >> (32 - bits);
   }
}

/* MAME defines LSB_FIRST from emu/endianness.h on little-endian hosts, and
 * adsp2100_port.h's SR / MR register unions use it to pick the member order.
 * We have no such header, so derive it here: leaving it undefined selects the
 * big-endian layout and silently swaps SR0/SR1 (and the MR halves) -- the DSP
 * still runs, just with every shift/MAC result in the wrong 16-bit half. */
#if !defined(LSB_FIRST) && !defined(MSB_FIRST)
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LSB_FIRST 1
#else
#define MSB_FIRST 1
#endif
#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || \
      defined(__aarch64__) || defined(__arm__) || defined(__riscv)
#define LSB_FIRST 1
#endif
#endif

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

inline int adsp_countl_zero(uint32_t v)
{
   if (v == 0)
      return 32;
   int n = 0;
   while ((v & 0x80000000u) == 0) { v <<= 1; n++; }
   return n;
}

inline int adsp_countl_one(uint32_t v)
{
   return adsp_countl_zero(~v);
}

/* One memory space of the DSP.  The RAX board owns the actual RAM/ROM maps,
 * so every access forwards through these function pointers. */
struct adsp_mem_iface
{
   void *ctx;
   uint16_t (*rd_w)(void *ctx, uint32_t addr);
   void     (*wr_w)(void *ctx, uint32_t addr, uint16_t val);
   uint8_t  (*rd_b)(void *ctx, uint32_t addr);
   void     (*wr_b)(void *ctx, uint32_t addr, uint8_t val);
   uint32_t (*rd_d)(void *ctx, uint32_t addr);
   void     (*wr_d)(void *ctx, uint32_t addr, uint32_t val);

   uint8_t read_byte(uint32_t addr) const { return rd_b ? rd_b(ctx, addr) : (uint8_t)rd_w(ctx, addr); }
   uint16_t read_word(uint32_t addr) const { return rd_w(ctx, addr); }
   uint32_t read_dword(uint32_t addr) const { return rd_d ? rd_d(ctx, addr) : ((uint32_t)rd_w(ctx, addr) | ((uint32_t)rd_w(ctx, addr + 1) << 16)); }
   void write_byte(uint32_t addr, uint8_t val) const { if (wr_b) wr_b(ctx, addr, val); else wr_w(ctx, addr, val); }
   void write_word(uint32_t addr, uint16_t val) const { wr_w(ctx, addr, val); }
   /* MAME sizes each space by its entry width: the ADSP program space holds
    * 32-bit entries (one 24-bit opcode per address), the data space 16-bit
    * words.  write_dword therefore means "one entry" on the former and "two
    * consecutive words" on the latter -- hence the optional wr_d hook; without
    * it a PM write would be split across two program words. */
   void write_dword(uint32_t addr, uint32_t val) const { if (wr_d) wr_d(ctx, addr, val); else { wr_w(ctx, addr, (uint16_t)val); wr_w(ctx, addr + 1, (uint16_t)(val >> 16)); } }
};

#endif
