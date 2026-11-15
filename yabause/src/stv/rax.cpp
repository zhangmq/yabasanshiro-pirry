/* Acclaim RAX sound board -- see rax.h and .notes/CURRENT/RAX-PLAN-20261114.md.
 *
 * MAME reference (master 2da7fb596daf, 2026-11-14):
 *   src/mame/shared/rax.cpp                 board: host latches, BDMA, timers, SPORT autobuffer
 *   src/mame/sega/stv.cpp:786-795,1367-1375 only writes are wired; reads stay cart ROM
 *   src/devices/cpu/adsp2100/*              the DSP core, vendored in ../adsp2100
 *
 * Deliberate deviations from MAME, each because MAME would abort or overrun:
 *   - DMA to byte memory (dir=1) logs once and is ignored (MAME: fatalerror;
 *     Batman Forever does not reach it in practice);
 *   - the DAC half-buffer read clamps to 256 words (MAME's stack buffer is
 *     0x100 words and count is not bounded);
 *   - a BDMA whose source would leave the 16 MiB ROM is ignored with a log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "adsp2100_port.h"
#include "log.h"
#include "rax.h"

/* ------------------------------------------------------------------------- */

#define RAX_DSP_CLOCK    16670000u          /* ADSP-2181 XTAL */
#define RAX_SH2_CLOCK    28636364u          /* 28.636363 MHz: the ST-V SH-2 */

#define RAX_ROM_SIZE     (16u * 1024u * 1024u)
#define RAX_ROM_BANK     0x400000u          /* 4 MiB */
#define RAX_PRAM_WORDS   0x4000u
#define RAX_BANK_WORDS   0x2000u
#define RAX_BANKS        5u                 /* 0 = internal overlay, 1..4 = external */
#define RAX_IRAM_WORDS   0x1FE0u
#define RAX_CTRL_BASE    0x3FE0u
#define RAX_CTRL_WORDS   0x20u

#define RAX_FIFO_FRAMES  4096u              /* ~90 ms at 45 kHz: covers the SCSP thread lag */
/* Synchronous fallback only (RaxDataWrite without the host queue): how much
   DSP time the writer lets the board run so it can see the word at all.
   83 cycles = MAME's perfect_quantum(5 us) at 16.67 MHz. */
#define RAX_HOST_QUANTUM 83u
/* Pacing only: how many DSP cycles the worker executes between two trips back
   to the machine grant.  Board events (host word, BDMA completion, DAC half
   buffer) cut the loop at their exact cycle, so this value cannot change the
   emulated result -- it only decides how often the worker re-reads the grant.
   The run loop must NOT be cut on a fixed grid: doing that pinned every board
   event to the same 83-cycle phase, and the BDMA completion then landed
   systematically inside the DSP's command-wait tail (0x2839-0x283B), whose
   `IMASK = AR` is only safe if no interrupt is taken between 0x283A and
   0x283B.  MAME's scheduler has no such grid; it runs to the next event. */
#ifndef RAX_RUN_CHUNK
#define RAX_RUN_CHUNK   4096u
#endif
#ifdef RAX_TEST_TRACE
#define RAX_LAG_WARN     (1u << 30)
#define RAX_LAG_RESET    (1u << 30)         /* harness publishes a whole run at once */
#else
/* Lateness policy.  The worker normally runs inside a field of the machine's
   time; this is a game, so the sound board is NOT allowed to drift seconds
   behind and "skip time" to catch up.  Two thresholds only:
     - WARN  (~33 ms): log it, keep executing every cycle (no time is lost).
     - RESET (~0.5 s): a real stall (debugger, front end frozen).  Only then is
       the ledger jumped forward, because replaying half a second of DSP time
       would put the sound board out of the game's timeline for good. */
#define RAX_LAG_WARN     (2u * 277833u)
#define RAX_LAG_RESET    (30u * 277833u)
#endif

/* MAME rax.cpp control register offsets (data space 0x3FE0+). */
enum {
   REG_IDMA_CONTROL   = 0x00,
   REG_BDMA_INT_ADDR  = 0x01,
   REG_BDMA_EXT_ADDR  = 0x02,
   REG_BDMA_CONTROL   = 0x03,
   REG_BDMA_WORD_CNT  = 0x04,
   REG_PROG_FLAG_DATA = 0x05,
   REG_PROG_FLAG_CTRL = 0x06,
   REG_S1_AUTOBUF     = 0x0F,
   REG_S1_RFSDIV      = 0x10,
   REG_S1_SCLKDIV     = 0x11,
   REG_S1_CONTROL     = 0x12,
   REG_S0_AUTOBUF     = 0x13,
   REG_S0_RFSDIV      = 0x14,
   REG_S0_SCLKDIV     = 0x15,
   REG_S0_CONTROL     = 0x16,
   REG_TIMER_SCALE    = 0x1B,
   REG_TIMER_COUNT    = 0x1C,
   REG_TIMER_PERIOD   = 0x1D,
   REG_WAITSTATES     = 0x1E,
   REG_SYSCONTROL     = 0x1F
};

/* Which ROM blobs the ST-V loader has handed over (by offset). */
#define RAX_BLOB_PROGRAM  0x01u             /* 0x000000, 512 KiB */
#define RAX_BLOB_SAMPLE0  0x02u             /* 0x400000 */
#define RAX_BLOB_SAMPLE1  0x04u             /* 0x600000 */
#define RAX_BLOB_SAMPLE2  0x08u             /* 0x800000 */
#define RAX_BLOB_SAMPLE3  0x10u             /* 0xA00000 */

struct rax_state
{
   /* Board memories (MAME rax.cpp address maps). */
   uint32_t pram[RAX_PRAM_WORDS];   /* program space: 24-bit words in 32-bit entries (MAME) */
   uint16_t bank[RAX_BANKS][RAX_BANK_WORDS];
   uint16_t iram[RAX_IRAM_WORDS];
   uint16_t control[RAX_CTRL_WORDS];
   uint8_t  rom[RAX_ROM_SIZE];

   uint32_t rom_mask;
   uint32_t rom_bank;
   uint32_t data_bank;
   uint32_t dmovlay;

   /* Host latches (MAME m_data_in / m_data_out / m_adsp_snd_pf0). */
   uint16_t data_in;
   uint16_t data_out;
   uint8_t  pf0;

   /* SPORT0 autobuffer state (MAME m_ireg/m_incs/m_size/m_ireg_base, index 0). */
   uint32_t ireg;        /* index of the autobuffer I register (0..7) */
   uint32_t dac_ireg;    /* same index, latched for the DAC half-buffer read */
   int32_t  incs;
   uint32_t size;
   uint32_t ireg_base;

   /* MAME's two timers: 16.16 DSP clock cycles (DAC half buffer), whole DSP
      clock cycles (BDMA completion). */
   uint64_t reg_period_fp;
   uint64_t reg_left_fp;
   uint64_t dma_period;
   uint64_t dma_left;
   uint32_t dma_param;

   /* Machine-time ledger: DSP cycles granted by the emulated time so far, and
      the cycles actually executed.  The synchronous fallback in RaxDataWrite()
      runs a little ahead of the ledger; the next RaxAdvance() pays it back. */
   uint64_t due_acc;
   uint64_t dsp_run;
   uint64_t cycles_run;

   uint32_t bdma_count;
   uint32_t imask_stuck;
   uint32_t host_in;      /* words the SH-2 wrote */
   uint32_t host_in16;    /* of which came from 16-bit stores */
   uint32_t host_delivered; /* words handed to the DSP latch */
   uint32_t host_read;    /* words the DSP took from the latch */
   uint32_t cmd_bdma;     /* command-fetch transfers (cnt=2 -> 0x3A10) */
   uint64_t dac_frames;   /* frames the DAC produced */
   uint32_t dac_starts;

   uint8_t  present;
   uint8_t  inited;

   adsp21xx_device cpu;
};

static struct rax_state st;

/* Audio: the DSP pushes on the machine thread, the SCSP adapter mixes on the
   SCSP thread.  Only this queue is shared; the lock order is always
   ring_lock -> rax_audio_mtx (the producer never takes the ring lock). */
static pthread_mutex_t rax_audio_mtx = PTHREAD_MUTEX_INITIALIZER;
static int16_t  rax_fifo[RAX_FIFO_FRAMES * 2];
static uint32_t rax_fifo_head;
static uint32_t rax_fifo_fill;
static double   rax_fifo_pos;      /* fractional read position, in frames */
static double   rax_step = 1.0;    /* input frames per 44.1 kHz output frame */
static uint8_t  rax_dac_on;
static uint8_t  rax_fifo_primed;
static uint32_t rax_fifo_under;   /* output frames with an empty FIFO */
static uint64_t rax_pop_total;    /* source frames the consumer popped */
static uint64_t rax_out_total;    /* output frames the consumer asked for */
#define RAX_FIFO_PREFILL 2048u     /* ~65 ms at 31.5 kHz: start playing at this */
#define RAX_FIFO_TARGET  3072u     /* ~97 ms: what the rate trim steers to      */
#ifdef RAX_DUMP
/* Last 8 seconds of the DAC stream (31.5 kHz) and of what this core contributes
   at 44.1 kHz; written at shutdown for an offline spectrum/DC analysis. */
#define RAX_DUMP_DAC_FRAMES  (31572u * 8u)
#define RAX_DUMP_OUT_FRAMES  (44100u * 8u)
static int16_t rax_dump_dac[RAX_DUMP_DAC_FRAMES * 2];
static unsigned rax_dump_dac_pos;
static int16_t rax_dump_out[RAX_DUMP_OUT_FRAMES * 2];
static unsigned rax_dump_out_pos;
#endif
/* Per-second DAC quality statistics (RAX_DIAG). */
static int64_t rax_dac_sum[2];
static uint64_t rax_dac_sq[2];
static uint32_t rax_dac_nsamples;
static int      rax_warned[12];

/* ------------------------------------------------------------------------- */
/* Memory map (MAME rax.cpp adsp_program_map / adsp_data_map / adsp_io_map)   */

static uint16_t *data_bank_ptr(void)
{
   /* MAME update_data_ram_bank(): dmovlay == 0 selects the internal overlay
      (entry 0), otherwise the external bank 1 + ram_bank. */
   uint16_t *p = st.bank[st.dmovlay ? 1 + (st.data_bank & 3u) : 0];
   st.cpu.fast_bank_cur = p;      /* keep the core's inline map in sync */
   return p;
}

static uint16_t ctrl_rd(unsigned int off);
static void     ctrl_wr(unsigned int off, uint16_t data);

static uint32_t pram_rd_d(void *ctx, uint32_t addr)
{
   (void)ctx;
   return st.pram[addr & (RAX_PRAM_WORDS - 1)];
}

static void pram_wr_d(void *ctx, uint32_t addr, uint32_t val)
{
   (void)ctx;
   st.pram[addr & (RAX_PRAM_WORDS - 1)] = val & 0xFFFFFF;
}

static uint16_t pram_rd_w(void *ctx, uint32_t addr)
{
   (void)ctx;
   return (uint16_t)st.pram[addr & (RAX_PRAM_WORDS - 1)];
}

static void pram_wr_w(void *ctx, uint32_t addr, uint16_t val)
{
   (void)ctx;
   st.pram[addr & (RAX_PRAM_WORDS - 1)] = val;
}

static uint16_t data_rd(void *ctx, uint32_t addr)
{
   uint32_t a = addr & 0x3FFF;
   (void)ctx;

   if (a < RAX_BANK_WORDS)
      return data_bank_ptr()[a];
   if (a < RAX_CTRL_BASE)
      return st.iram[a - RAX_BANK_WORDS];
   return ctrl_rd(a - RAX_CTRL_BASE);
}

#ifdef RAX_TEST_TRACE
static void (*rax_test_mem_cb)(unsigned, unsigned, unsigned);
extern "C" void RaxTestSetMemTrace(void (*fn)(unsigned, unsigned, unsigned))
{
   rax_test_mem_cb = fn;
}
#endif

/* Diagnostics are small; keep them outside the RAX_DUMP guard so a harness
   build (no RAX_DUMP) still links the report function. */
static uint32_t rax_ring_hist[8];
static uint8_t  rax_ring_hit[0x400];
static int      rax_dac_trace;
static int      rax_dac_trace_n;
static void data_wr(void *ctx, uint32_t addr, uint16_t val)
{
   uint32_t a = addr & 0x3FFF;
   (void)ctx;
#ifdef RAX_TEST_TRACE
   if (rax_test_mem_cb)
      rax_test_mem_cb(st.cpu.pc(), a, val);
#endif
#ifdef RAX_TRACE_IMASK
   /* The command-wait loop uses DM(3AC6) as its iteration counter and
      DM(3B92) as the run flag; a pop/stray write to either ends the wait. */
   if (a == 0x3AC6 || a == 0x3B92)
      adsp_evt_note(8, (unsigned)a, (unsigned)val, (unsigned)st.cpu.pc());
#endif
#ifdef RAX_DUMP
   /* Which words of the DAC ring the DSP ever writes: the ring came out with
      every even word zero, so either the DSP writes at a stride of 2 or the
      autobuffer's incs/base is being decoded one word off. */
   if (a >= 0x3000u && a < 0x3400u) {
      rax_ring_hist[a & 7u]++;
      rax_ring_hit[a - 0x3000u] = 1;
   }
#endif

   if (a < RAX_BANK_WORDS)
      data_bank_ptr()[a] = val;
   else if (a < RAX_CTRL_BASE)
      st.iram[a - RAX_BANK_WORDS] = val;
   else
      ctrl_wr(a - RAX_CTRL_BASE, val);
}

static uint16_t io_rd(void *ctx, uint32_t addr)
{
   (void)ctx;
   switch (addr & 0x7FF) {
   case 0x0000: return (uint16_t)st.data_bank;
   case 0x0001: return (uint16_t)st.rom_bank;
   case 0x0003:
      /* MAME host_r(): reading the host latch clears IRQL0. */
      st.host_read++;
      adsp_evt_note(5, (unsigned)st.data_in, (unsigned)st.dsp_run, 0);
      st.cpu.set_irq_line(ADSP2181_IRQL0, 0);
      return st.data_in;
   default:
      return 0xFFFF;
   }
}

static void io_wr(void *ctx, uint32_t addr, uint16_t val)
{
   (void)ctx;
#ifdef RAX_TEST_TRACE
   if (rax_test_mem_cb)
      rax_test_mem_cb(st.cpu.pc(), 0x8000u | (addr & 0x7FF), val);
#endif
   switch (addr & 0x7FF) {
   case 0x0000: st.data_bank = val & 3u; (void)data_bank_ptr(); break;
   case 0x0001: st.rom_bank = val; break;
   case 0x0003:
      /* MAME host_w(): DSP -> host latch, clears PF0. */
      st.data_out = val;
      st.pf0 = 0;
      break;
   default:
      break;
   }
}

/* ------------------------------------------------------------------------- */
/* Audio: MAME's adsp_irq(0), recompute_sample_rate() and adsp_sound_tx_callback()  */

static void rax_audio_reset(void)
{
   pthread_mutex_lock(&rax_audio_mtx);
   rax_fifo_head = 0;
   rax_fifo_fill = 0;
   rax_fifo_pos = 0.0;
   rax_dac_on = 0;
   rax_fifo_primed = 0;
   rax_step = 1.0;
   pthread_mutex_unlock(&rax_audio_mtx);
}

/* MAME rax.cpp adsp_irq(0): the DSP's autobuffer half is full; take the words
   from DSP data memory and hand them to the two DAC channels as interleaved
   stereo.  Runs on the machine thread, inside rax_run_cycles(). */
static void rax_dac_irq(void)
{
   uint32_t reg, count, i;
   int incs;

   if (st.reg_period_fp == 0 || st.incs <= 0 || st.size == 0)
      return;

   incs = st.incs;
   count = st.size / (4u * (uint32_t)incs);
   if (count == 0)
      return;
   if (count > 0x100u) {
      /* MAME's int16_t buffer[0x100] would overflow; clamp instead. */
      if (!rax_warned[0]++)
         LOG_WARN("rax: DAC half-buffer %u words > 256, clamped", (unsigned)count);
      count = 0x100u;
   }

   pthread_mutex_lock(&rax_audio_mtx);
   if (!rax_dac_on) {
      pthread_mutex_unlock(&rax_audio_mtx);
      return;
   }

   {
      /* MAME: reg = state_int(ADSP2100_I0 + m_ireg[0]) -- st.ireg is the
         *index* of the autobuffer's I register (7 on this board), not the
         address.  Using it as the address left I7 stuck, so the DSP's sample
         loop never advanced and the DAC played whatever was at DM(7). */
      uint32_t idx = st.ireg & 7u;
      reg = st.cpu.dag_i((int)idx);
      st.dac_ireg = idx;
   }
   {
      static FILE *dumpf = NULL;
      static long dumped;
      static int dump_state = -1;
      if (dump_state < 0) {
         const char *e = getenv("RAX_DAC_DUMP");
         dump_state = (e && *e) ? 1 : 0;
         if (dump_state)
            dumpf = fopen(e, "wb");
      }
      if (dumpf != NULL && dumped < 31572L * 40) {   /* first 40 s only */
         static int16_t buf[512 * 2];
         unsigned n = 0;
         for (i = 0; i + 1 < count && n < 512; i += 2) {
            uint32_t a = (reg + (uint32_t)incs * (uint32_t)i) & 0x3FFF;
            uint32_t b = (a + (uint32_t)incs) & 0x3FFF;
            buf[n * 2 + 0] = (int16_t)data_rd(NULL, a);
            buf[n * 2 + 1] = (int16_t)data_rd(NULL, b);
            n++;
         }
         if (n) {
            fwrite(buf, sizeof(int16_t) * 2, n, dumpf);
            fflush(dumpf);
            dumped += (long)n;
         }
      }
   }
   for (i = 0; i + 1 < count; i += 2) {
      int16_t l = (int16_t)data_rd(NULL, reg);
      int16_t r;
      reg = (reg + (uint32_t)incs) & 0x3FFF;
      r = (int16_t)data_rd(NULL, reg);
      reg = (reg + (uint32_t)incs) & 0x3FFF;

#ifdef RAX_DUMP
      if (rax_dac_trace && rax_dac_trace_n < 32) {
         LOG_INFO("rax: dacrd[%d] ireg=%u base=%04X incs=%d l@%04X=%d r@%04X=%d",
                  rax_dac_trace_n, (unsigned)st.ireg, (unsigned)st.ireg_base,
                  (int)st.incs,
                  (unsigned)((reg - 2u * (uint32_t)incs) & 0x3FFF), (int)l,
                  (unsigned)((reg - (uint32_t)incs) & 0x3FFF), (int)r);
         rax_dac_trace_n++;
      }
#endif
      if (rax_fifo_fill >= RAX_FIFO_FRAMES) {
         /* Consumer is behind: drop the oldest frame. */
         rax_fifo_head = (rax_fifo_head + 1u) % RAX_FIFO_FRAMES;
         rax_fifo_fill--;
      }
      {
         uint32_t tail = (rax_fifo_head + rax_fifo_fill) % RAX_FIFO_FRAMES;
         rax_fifo[tail * 2 + 0] = l;
         rax_fifo[tail * 2 + 1] = r;
         rax_fifo_fill++;
#ifdef RAX_DUMP
         rax_dump_dac[rax_dump_dac_pos * 2 + 0] = l;
         rax_dump_dac[rax_dump_dac_pos * 2 + 1] = r;
         rax_dump_dac_pos = (rax_dump_dac_pos + 1u) % RAX_DUMP_DAC_FRAMES;
#endif
         rax_dac_sum[0] += (int32_t)(int16_t)l;
         rax_dac_sum[1] += (int32_t)(int16_t)r;
         rax_dac_sq[0] += (uint32_t)((int32_t)(int16_t)l * (int16_t)l);
         rax_dac_sq[1] += (uint32_t)((int32_t)(int16_t)r * (int16_t)r);
         rax_dac_nsamples++;
      }
   }

   st.dac_frames += count / 2;

   /* MAME's wrap test (only the "ran past the end" direction is handled). */
   if (reg >= st.ireg_base + st.size)
      reg = st.ireg_base;
   st.cpu.set_dag_i((int)st.dac_ireg, reg);

   pthread_mutex_unlock(&rax_audio_mtx);
}

/* MAME recompute_sample_rate(0). */
static void rax_recompute_rate(void)
{
   uint32_t div = st.control[REG_S0_SCLKDIV] & 0x3FFF;
   uint64_t frame_cycles = 16ull * (div + 1u);   /* DSP cycles per output frame */

   pthread_mutex_lock(&rax_audio_mtx);
   rax_step = ((double)RAX_DSP_CLOCK / (double)frame_cycles) / 44100.0;
   rax_dac_on = 1;
   pthread_mutex_unlock(&rax_audio_mtx);

   if (st.incs > 0 && st.size > 0) {
      uint64_t num = frame_cycles * (uint64_t)st.size;
      uint64_t den = 8ull * (uint64_t)st.incs;
      st.reg_period_fp = (num << 16) / den;
      st.reg_left_fp = st.reg_period_fp;
      st.cpu.board_event_in((uint32_t)(st.reg_left_fp >> 16));
   } else {
      st.reg_period_fp = 0;
      st.reg_left_fp = 0;
   }
}

static void rax_dac_stop(void)
{
   pthread_mutex_lock(&rax_audio_mtx);
   rax_dac_on = 0;
   rax_fifo_primed = 0;
   rax_fifo_head = 0;
   rax_fifo_fill = 0;
   rax_fifo_pos = 0.0;
   pthread_mutex_unlock(&rax_audio_mtx);
   st.reg_period_fp = 0;
   st.reg_left_fp = 0;
}

/* MAME adsp_sound_tx_callback(): the DSP wrote SPORT0's TX register. */
static void sport_tx(void *ctx, uint32_t which, uint32_t data, uint32_t mask)
{
   (void)ctx; (void)data; (void)mask;
   if (which != 0)
      return;

   if ((st.control[REG_SYSCONTROL] & 0x1000u) &&
       (st.control[REG_S0_AUTOBUF] & 0x0002u)) {
      uint32_t ireg = (st.control[REG_S0_AUTOBUF] >> 9) & 7u;
      uint32_t mreg = (st.control[REG_S0_AUTOBUF] >> 7) & 3u;

      mreg |= ireg & 0x04u;
      st.ireg = ireg;
      st.incs = st.cpu.dag_m((int)mreg);
      st.size = st.cpu.dag_l((int)ireg) & 0x3FFF;

      {
         uint32_t source = st.cpu.dag_i((int)ireg);
         source -= (uint32_t)st.incs;   /* MAME: back one so the first sample is not lost */
         st.cpu.set_dag_i((int)ireg, source);
         st.ireg_base = source;
      }
      rax_recompute_rate();
      st.dac_starts++;
      LOG_INFO("rax: DAC on, sclkdiv=%u size=%u incs=%d I%u=%04X, rate %.1f Hz",
               (unsigned)(st.control[REG_S0_SCLKDIV] & 0x3FFF), (unsigned)st.size,
               (int)st.incs, (unsigned)ireg, (unsigned)st.ireg_base,
               (double)RAX_DSP_CLOCK / (16.0 * ((st.control[REG_S0_SCLKDIV] & 0x3FFF) + 1u)));
      return;
   }

   /* MAME disables both DACs and drops the timer here. */
   if (!rax_warned[1]++)
      LOG_WARN("rax: SPORT0 TX without autobuffer (sys=%04X autobuf=%04X)",
               (unsigned)st.control[REG_SYSCONTROL], (unsigned)st.control[REG_S0_AUTOBUF]);
   rax_dac_stop();
}

static void dmovlay_cb(void *ctx, uint32_t bank)
{
   (void)ctx;
   if (bank > 1) {
      if (!rax_warned[2]++)
         LOG_WARN("rax: DMOVLAY with bank %u", (unsigned)bank);
      return;
   }
   st.dmovlay = bank;
   (void)data_bank_ptr();
}

/* ------------------------------------------------------------------------- */
/* BDMA and the control registers (MAME rax.cpp adsp_control_w)               */

static void rax_bdma_done(void)
{
   /* MAME dma_timer_callback(): publish the end address/page, then either
      reset the DSP or raise the BDMA interrupt. */
   st.control[REG_BDMA_WORD_CNT] = 0;
   st.control[REG_BDMA_EXT_ADDR] = (uint16_t)(st.dma_param & 0x3FFF);
   st.control[REG_BDMA_CONTROL] =
      (uint16_t)((st.control[REG_BDMA_CONTROL] & ~0xFF00u) |
                 (((st.dma_param >> 14) & 0xFFu) << 8));

   if (st.control[REG_BDMA_CONTROL] & 0x0008u) {
      st.cpu.reset();               /* MAME: pulse INPUT_LINE_RESET */
   } else {
      st.cpu.set_irq_line(ADSP2181_BDMA, 1);
      st.cpu.set_irq_line(ADSP2181_BDMA, 0);
   }

   st.dma_period = 0;
   st.dma_left = 0;
}

static void ctrl_wr(unsigned int off, uint16_t data)
{
   st.control[off & (RAX_CTRL_WORDS - 1)] = data;

   switch (off) {
   case REG_BDMA_INT_ADDR:
      st.control[REG_BDMA_INT_ADDR] = data & 0x3FFF;
      break;
   case REG_BDMA_EXT_ADDR:
      st.control[REG_BDMA_EXT_ADDR] = data & 0x3FFF;
      break;
   case REG_BDMA_CONTROL:
      st.control[REG_BDMA_CONTROL] = data & 0xFF0F;
      break;

   case REG_BDMA_WORD_CNT: {
      const uint8_t *rom;
      uint32_t page, dir, type, src, count, dst, dst0, rom_base;

      st.control[REG_BDMA_WORD_CNT] = data & 0x3FFF;

      page  = (st.control[REG_BDMA_CONTROL] >> 8) & 0xFF;
      dir   = (st.control[REG_BDMA_CONTROL] >> 2) & 1;
      type  = st.control[REG_BDMA_CONTROL] & 3u;
      src   = (page << 14) | st.control[REG_BDMA_EXT_ADDR];
      count = st.control[REG_BDMA_WORD_CNT];
      dst   = st.control[REG_BDMA_INT_ADDR];
      dst0  = dst;

      if (st.rom_bank > 3u) {
         if (!rax_warned[3]++)
            LOG_WARN("rax: BDMA with rom_bank %u, masked to 0", (unsigned)st.rom_bank);
         st.rom_bank &= 3u;
      }
      rom_base = st.rom_bank * RAX_ROM_BANK;
      rom = st.rom + rom_base;

      if (dir) {
         /* MAME: fatalerror("DMA to byte memory").  The board never does this
            in a run MAME survives; ignore it so a stray write cannot abort. */
         if (!rax_warned[4]++)
            LOG_WARN("rax: BDMA to byte memory ignored (page %u type %u addr %06X)",
                     (unsigned)page, (unsigned)type, (unsigned)src);
         break;
      }

      if (count && (uint64_t)rom_base + src +
                   (type == 0 ? 3ull : type == 1 ? 2ull : 1ull) * count > RAX_ROM_SIZE) {
         if (!rax_warned[5]++)
            LOG_WARN("rax: BDMA past the ROM (bank %u src %06X count %u type %u)",
                     (unsigned)st.rom_bank, (unsigned)src, (unsigned)count, (unsigned)type);
         break;
      }

#ifdef RAX_TEST_TRACE
      if (dst0 >= 0x2000u && dst0 < 0x2010u) {
         fprintf(stderr, "[BD] cyc=%llu pc=%04X type=%u cnt=%u src=%06X dst=%04X dmov=%u dbank=%u\n",
                 (unsigned long long)st.cycles_run, (unsigned)st.cpu.pc(), (unsigned)type,
                 (unsigned)(data & 0x3FFF), (unsigned)src, (unsigned)dst0,
                 (unsigned)st.dmovlay, (unsigned)st.data_bank);
      }
#endif
      if (type == 0) {
         while (count--) {
            uint32_t dword = ((uint32_t)rom[src] << 16) |
                             ((uint32_t)rom[src + 1] << 8) |
                             (uint32_t)rom[src + 2];
            pram_wr_d(NULL, dst, dword);
            src += 3;
            dst++;
            st.control[REG_BDMA_INT_ADDR] = (uint16_t)dst;   /* MAME advances it */
         }
      } else if (type == 1) {
         while (count--) {
            uint16_t word = (uint16_t)(((uint16_t)rom[src] << 8) | rom[src + 1]);
            data_wr(NULL, dst, word);
            src += 2;
            dst++;
            st.control[REG_BDMA_INT_ADDR] = (uint16_t)dst;
         }
      } else {
         int shift = (type == 2) ? 8 : 0;
         while (count--) {
            data_wr(NULL, dst, (uint16_t)((uint16_t)rom[src] << shift));
            src++;
            dst++;
            st.control[REG_BDMA_INT_ADDR] = (uint16_t)dst;
         }
      }

#ifdef RAX_TEST_TRACE
      /* Same line the MAME reference run printed (.notes/ref/rax-old/work/
         mame-bdma-*.txt); only the host-side harness compiles this. */
      printf("[BDMA] cyc=%llu pc=%04X type=%u dir=%u cnt=%u page=%u src=%06X dst=%04X dmovlay=%u dbank=%u rbank=%u abs=%07X\n",
             (unsigned long long)st.cycles_run, (unsigned)st.cpu.pc(),
             (unsigned)type, (unsigned)dir, (unsigned)(data & 0x3FFF), (unsigned)page,
             (unsigned)((page << 14) | st.control[REG_BDMA_EXT_ADDR]),
             (unsigned)dst0, (unsigned)st.dmovlay, (unsigned)st.data_bank,
             (unsigned)st.rom_bank,
             (unsigned)(st.rom_bank * RAX_ROM_BANK + ((page << 14) | st.control[REG_BDMA_EXT_ADDR])));
#endif

      /* MAME rax.cpp: the words are already in place, the completion timer
         only signals later -- `period = word_period * count`, i.e. one DSP
         cycle per word, with no extra constant. */
      st.dma_param = src;
      st.dma_period = (data & 0x3FFF);
      st.dma_left = st.dma_period;
      if (st.dma_period == 0) {
         /* MAME arms the completion timer with attotime::zero, i.e. it still
            fires (right after the current instruction).  Leaving the timer
            unarmed made a zero-word BDMA never complete, and a DSP waiting for
            that interrupt would stop taking host commands. */
         st.dma_period = 1;
         st.dma_left = 1;
      }

      if ((dst0 == 0x3A10 || dst0 == 0x3A0F) && (data & 0x3FFF) == 2)
         st.cmd_bdma++;
      /* Serviced on its deadline, like MAME's timer (see board_event_in). */
      if (st.dma_period)
         st.cpu.board_event_in((uint32_t)st.dma_left);
      if (++st.bdma_count <= 3)
         LOG_DEBUG("rax: BDMA #%u type %u count %u page %u src %06X -> %04X",
                   (unsigned)st.bdma_count, (unsigned)type, (unsigned)(data & 0x3FFF),
                   (unsigned)page, (unsigned)src, (unsigned)st.control[REG_BDMA_INT_ADDR]);
      break;
   }

   case REG_S1_AUTOBUF:
      /* MAME only disables the right-channel DMADAC here; this port runs one
         stereo stream, so nothing to do. */
      break;

   case REG_S0_AUTOBUF:
      if (!(data & 0x0002u))
         rax_dac_stop();
      break;

   case REG_S1_CONTROL:
      if (((data >> 4) & 3u) == 2u || ((data >> 4) & 3u) == 3u) {
         if (!rax_warned[6]++)
            LOG_WARN("rax: SPORT1 companding requested (%04X)", (unsigned)data);
      }
      break;

   case REG_PROG_FLAG_DATA:
      if (!rax_warned[7]++)
         LOG_DEBUG("rax: PFLAGS written %04X", (unsigned)data);
      break;

   default:
      break;
   }
}

static uint16_t ctrl_rd(unsigned int off)
{
   if (off == REG_PROG_FLAG_DATA)
      return st.pf0;         /* MAME: host read strobe state, not the stored latch */
   return st.control[off & (RAX_CTRL_WORDS - 1)];
}

/* ------------------------------------------------------------------------- */
/* The DSP run loop: timers are serviced at their deadlines                  */

static int  rax_host_peek(uint64_t *cyc);
static void rax_deliver_due(uint64_t now);

/* Where the worker's CPU goes: the interpreter call (st.cpu.run) versus the
   board glue around it (event slicing, delivery, timers, DAC pushes).  The
   generic timer is cheap enough to read twice per slice; slices are hundreds
   of cycles, so this is a few thousand reads per second. */
static inline uint64_t rax_ticks(void)
{
#if defined(__aarch64__)
   uint64_t v;
   __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
   return v;
#elif defined(__x86_64__)
   return __builtin_ia32_rdtsc();
#else
   return 0;
#endif
}
static uint64_t rax_tk_exec, rax_tk_all;

static void rax_run_cycles(uint32_t cycles)
{
   const uint32_t total = cycles;
   uint64_t tk_entry = rax_ticks();

   while (cycles > 0) {
      /* DSP cycle this iteration starts at.  st.dsp_run is only published by
         the caller after we return, so track our own progress. */
      uint64_t now = st.dsp_run + (uint64_t)(total - cycles);
      uint32_t slice = cycles;

      /* Deliver any host word whose timestamp has come, then run up to the
         next board event.  Every event has an absolute deadline, so the point
         where the DSP stops does not depend on how the machine paced its
         passes. */
      rax_deliver_due(now);

      /* Host words are delivered on their exact timestamp (MAME's data_w adds
         no delay either), so the run stops at the next one. */
      {
         uint64_t next;
         if (rax_host_peek(&next) && next > now && (next - now) < (uint64_t)slice)
            slice = (uint32_t)(next - now);
      }
      /* Board timers still fire on their exact deadline (that is what MAME's
         scheduler does). */
      if (st.reg_period_fp) {
         uint64_t left = st.reg_left_fp >> 16;
         if (left < slice)
            slice = (left == 0) ? 1u : (uint32_t)left;
      }
      if (st.dma_period && st.dma_left < slice)
         slice = (st.dma_left == 0) ? 1u : (uint32_t)st.dma_left;
      if (slice == 0)
         slice = 1;

#ifdef RAX_TRACE_IMASK
      adsp_evt_set_cycle(now);
#endif
      {
         uint64_t tk0 = rax_ticks();
         st.cpu.run((int)slice);
         rax_tk_exec += rax_ticks() - tk0;
      }
      /* execute_run() can come back early (for example when an interrupt is
         taken at the top of the loop), leaving m_icount unused.  Counting the
         requested cycles as executed would make the board clock depend on how
         the run happened to be chunked, which is exactly what this change is
         meant to remove: accounting uses what the CPU actually retired. */
      {
         int left = st.cpu.icount_left();
         if (left > 0)
            slice -= (uint32_t)left;
         if (slice == 0)
            slice = 1;
      }
      st.cycles_run += slice;
      cycles -= slice;

      if (st.reg_period_fp) {
         uint64_t dec = (uint64_t)slice << 16;
         if (st.reg_left_fp <= dec) {
            st.reg_left_fp = st.reg_period_fp;
            rax_dac_irq();
         } else {
            st.reg_left_fp -= dec;
         }
      }
      if (st.dma_period) {
         if (st.dma_left <= slice) {
            st.dma_left = 0;
            st.dma_period = 0;
            /* MAME rax.cpp: the completion timer fires on its own deadline and
               pulses the BDMA line; the DSP takes it at the next instruction
               boundary.  data_w() has already written every word, exactly as
               the board does. */
            rax_bdma_done();
         } else {
            st.dma_left -= slice;
         }
      }
   }

   rax_tk_all += rax_ticks() - tk_entry;
}

/* ------------------------------------------------------------------------- */
/* DSP worker thread                                                        */
/*                                                                          */
/* The interpreter costs roughly half an A53 core (measured on the device:   */
/* fields 70 -> 39 when it ran inside the machine thread), so it runs on its */
/* own core and is fed exactly like the machine-token SCSP thread: the       */
/* machine publishes the emulated CPU time it has advanced as a cycle grant, */
/* the worker executes exactly that much and no more.  Host writes are       */
/* timestamped with the machine cycle they happened at and delivered by the  */
/* worker when its own clock reaches that point, which preserves the MAME    */
/* ordering without ever making the machine thread wait.                     */

static pthread_mutex_t rax_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  rax_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t       rax_thread;
static uint64_t        rax_grant;        /* DSP cycles granted by machine time */
static uint64_t        rax_done;         /* cycles executed (published)         */
static int             rax_thread_up;
static int             rax_running;
static int             rax_reset_req;
static int             rax_reset_done;


/* Host->DSP words, timestamped with the machine cycle they arrived at.  The
   worker delivers each one when its own clock reaches that point, so the DSP
   sees exactly the order and the time MAME's data_w() gave it.  A small queue
   instead of a single latch: the machine thread does not wait for the DSP, and
   dropping words because the worker happened to be 100 us behind would lose
   commands MAME would have delivered. */
#define RAX_TOKEN_IDLE_US 200u   /* scsp.c SCSP_TOKEN_IDLE_US: nine audio samples */
#define RAX_HOSTQ 32
static uint16_t        rax_hostq_val[RAX_HOSTQ];
static uint64_t        rax_hostq_cyc[RAX_HOSTQ];
static unsigned        rax_hostq_head, rax_hostq_tail;
static uint32_t        rax_lost_words;
/* First host words of a session, for the "commands disappear" diagnosis. */
#define RAX_DBG_WORDS 512
static uint16_t rax_dbg_val[RAX_DBG_WORDS];
static uint64_t rax_dbg_cyc[RAX_DBG_WORDS];
static uint8_t  rax_dbg_w16[RAX_DBG_WORDS];
static unsigned rax_dbg_n;

static void rax_board_reset(void);

/* Cycle of the oldest queued host word, 1 if there is one. */
static int rax_host_peek(uint64_t *cyc)
{
   int have = 0;

   pthread_mutex_lock(&rax_mtx);
   if (rax_hostq_head != rax_hostq_tail) {
      *cyc = rax_hostq_cyc[rax_hostq_head];
      have = 1;
   }
   pthread_mutex_unlock(&rax_mtx);
   return have;
}

/* Deliver every host word whose timestamp is <= now, at that exact cycle.
   Board state is owned by the worker; the timestamp and value are published
   under rax_mtx so the machine thread never sees a half-written entry.

   One word per call: raising IRQL0 makes set_irq_line() take the interrupt
   immediately, so irq_depth becomes > 0 and the next word waits for the RTI.
   That deferral is deliberate (see the comment below) and is deterministic:
   the word is delivered the moment the handler returns, never "somewhere in
   the next 192 cycles". */
static void rax_deliver_due(uint64_t now)
{
   uint16_t val;

   pthread_mutex_lock(&rax_mtx);
   if (rax_hostq_head == rax_hostq_tail || now < rax_hostq_cyc[rax_hostq_head]) {
      pthread_mutex_unlock(&rax_mtx);
      return;
   }
   val = rax_hostq_val[rax_hostq_head];
   rax_hostq_head = (rax_hostq_head + 1u) % RAX_HOSTQ;
   pthread_mutex_unlock(&rax_mtx);

   /* MAME data_w(): latch + IRQL0. */
   adsp_evt_note(4, val, (unsigned)now, st.cpu.dbg_imask());
   st.host_delivered++;
   st.data_in = val;
   st.cpu.set_irq_line(ADSP2181_IRQL0, 1);
}

#ifdef RAX_DUMP
static void rax_log_dac_state(void);
#endif

/* Real-time budget probe (RAX_PERF=1): per DSP second, print the wall time and
   the worker's own CPU time, so "cannot keep up" can be told apart from "not
   scheduled".  Cached env lookup, never per instruction. */
static int rax_perf_on = -1;
static uint64_t rax_perf_dsp;
static uint64_t rax_perf_next;
static struct timespec rax_perf_wall0, rax_perf_cpu0;

static void rax_perf_report(void)
{
   struct timespec w, c;
   double wall_ms, cpu_ms, dsp_ms;

   clock_gettime(CLOCK_MONOTONIC, &w);
   clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
   if (rax_perf_wall0.tv_sec == 0 && rax_perf_wall0.tv_nsec == 0) {
      rax_perf_wall0 = w;
      rax_perf_cpu0 = c;
      rax_perf_dsp = 0;
      rax_perf_next = st.dsp_run + RAX_DSP_CLOCK;
      return;
   }
   wall_ms = (double)(w.tv_sec - rax_perf_wall0.tv_sec) * 1000.0
           + (double)(w.tv_nsec - rax_perf_wall0.tv_nsec) / 1e6;
   cpu_ms  = (double)(c.tv_sec - rax_perf_cpu0.tv_sec) * 1000.0
           + (double)(c.tv_nsec - rax_perf_cpu0.tv_nsec) / 1e6;
   dsp_ms  = (double)rax_perf_dsp * 1000.0 / (double)RAX_DSP_CLOCK;
   {
      /* Split of the worker's time: the interpreter call itself vs the board
         glue around it.  Both counters are in generic-timer ticks (24 MHz). */
      double tk = 24.0e3;   /* ticks per ms */
      LOG_INFO("rax: perf wall_ms=%.0f cpu_ms=%.0f dsp_ms=%.0f => %.2fx realtime, "
               "%.2f of a core, lag=%llu fifo=%u under=%u exec_ms=%.0f glue_ms=%.0f",
               wall_ms, cpu_ms, dsp_ms, dsp_ms / wall_ms, cpu_ms / wall_ms,
               (unsigned long long)(__atomic_load_n(&rax_grant, __ATOMIC_RELAXED) - st.dsp_run),
               (unsigned)rax_fifo_fill, (unsigned)rax_fifo_under,
               (double)rax_tk_exec / tk, (double)(rax_tk_all - rax_tk_exec) / tk);
      rax_tk_exec = rax_tk_all = 0;
   }
   rax_perf_wall0 = w;
   rax_perf_cpu0 = c;
   rax_perf_dsp = 0;
   rax_perf_next = st.dsp_run + RAX_DSP_CLOCK;
}

/* Run the DSP until it has caught up with the published grant.  Worker only. */
static void rax_run_worker(void)
{
   static uint64_t stuck_since;
   static uint64_t rax_lag_next_log;

   for (;;) {
      uint64_t target;

      if (__atomic_load_n(&rax_reset_req, __ATOMIC_RELAXED)) {
         pthread_mutex_lock(&rax_mtx);
         rax_reset_req = 0;
         rax_grant = 0;
         pthread_mutex_unlock(&rax_mtx);
         rax_board_reset();
         __atomic_store_n(&rax_done, 0, __ATOMIC_RELEASE);
         pthread_mutex_lock(&rax_mtx);
         rax_reset_done = 1;
         pthread_cond_broadcast(&rax_cv);
         pthread_mutex_unlock(&rax_mtx);
         continue;
      }

      rax_deliver_due(st.dsp_run);

      target = __atomic_load_n(&rax_grant, __ATOMIC_RELAXED);
      if (st.dsp_run >= target)
         return;

      if (target - st.dsp_run > RAX_LAG_WARN &&
          target - st.dsp_run <= RAX_LAG_RESET) {
         /* Jitter, not a stall: the worker is catching up and NO DSP time is
            dropped.  Rate-limit the log to once per second of DSP time. */
         if (st.dsp_run >= rax_lag_next_log) {
            rax_lag_next_log = st.dsp_run + RAX_DSP_CLOCK;
            LOG_WARN("rax: worker %llu cycles behind (catching up, no cycles lost)",
                     (unsigned long long)(target - st.dsp_run));
         }
      }
      if (target - st.dsp_run > RAX_LAG_RESET) {
         /* Real stall, not jitter: the emulated time in between cannot be
            replayed without desynchronising the sound board from the game. */
         if (!rax_warned[9]++)
            LOG_WARN("rax: worker %llu cycles behind (stall), dropping that time",
                     (unsigned long long)(target - st.dsp_run));
         st.dsp_run = target;
      }

      {
         uint32_t n = (uint32_t)(target - st.dsp_run);
         if (n > RAX_RUN_CHUNK)
            n = RAX_RUN_CHUNK;
         rax_run_cycles(n);
         st.dsp_run += n;

         /* Real-time budget: host CPU burned per second of DSP time, and how
            much of a core the worker actually got.  RAX_PERF=1, one line per
            DSP second.  Realtime < 1 means the board cannot keep up with the
            machine thread no matter what the FIFO does; cores < 1 while
            realtime < 1 means it is not being scheduled, not too slow. */
         if (rax_perf_on < 0) {
            const char *e = getenv("RAX_PERF");
            rax_perf_on = (e && *e && *e != '0') ? 1 : 0;
         }
         if (rax_perf_on) {
            rax_perf_dsp += n;
            if (st.dsp_run >= rax_perf_next)
               rax_perf_report();
         }
      }
      __atomic_store_n(&rax_done, st.dsp_run, __ATOMIC_RELEASE);
      /*
         Watchdog only: the DSP program has been seen to end up with IMASK=0
         while an interrupt is still asserted, which silences the board for
         good.  That was traced to running the DSP on a fixed 5 us grid (see
         rax_run_cycles); keep the report so a regression is visible, but never
         force the mask -- hiding it made the real path untraceable. */
      if (st.cpu.dbg_imask() == 0 && st.cpu.dbg_state(9) != 0) {
         if (stuck_since == 0)
            stuck_since = st.dsp_run;
         else if (st.dsp_run - stuck_since > RAX_DSP_CLOCK / 10) {
            st.imask_stuck++;
            if (st.imask_stuck <= 3)
               LOG_WARN("rax: IMASK stuck at 0 with an interrupt asserted (pc=%04X lastwr=%04X/%03X)",
                        (unsigned)st.cpu.pc(), (unsigned)st.cpu.imask_wr_pc(),
                        (unsigned)st.cpu.imask_wr_val());
            stuck_since = 0;
         }
      } else {
         stuck_since = 0;
      }

#ifdef RAX_DIAG
      {
         static uint64_t next_log;
         static uint64_t prev_cyc;
         static uint64_t prev_wall_us;
         uint64_t now_us;
         if (st.dsp_run >= next_log) {
            unsigned q;
            next_log = st.dsp_run + RAX_DSP_CLOCK;   /* once per DSP second */
            {
               struct timespec tw;
               clock_gettime(CLOCK_MONOTONIC, &tw);
               now_us = (uint64_t)tw.tv_sec * 1000000u + (uint64_t)tw.tv_nsec / 1000u;
            }
            pthread_mutex_lock(&rax_audio_mtx);
            q = rax_fifo_fill;
            pthread_mutex_unlock(&rax_audio_mtx);
            LOG_INFO("rax: t=%llus pc=%04X imask=%03X irq9=%u idle=%d in=%u deliv=%u "
                     "taken=%u q=%u/%u cmd=%u fifo=%u under=%u dc=%d/%d rms=%u/%u resc=%u iwpc=%04X/iwv=%03X su=%u supc=%04X",
                     (unsigned long long)(st.dsp_run / RAX_DSP_CLOCK),
                     (unsigned)st.cpu.pc(), (unsigned)st.cpu.dbg_imask(),
                     (unsigned)st.cpu.dbg_state(9), st.cpu.dbg_idle(),
                     (unsigned)st.host_in, (unsigned)st.host_delivered,
                     (unsigned)st.host_read, (unsigned)((rax_hostq_tail + RAX_HOSTQ - rax_hostq_head) % RAX_HOSTQ),
                     (unsigned)RAX_HOSTQ, (unsigned)st.cmd_bdma, q, (unsigned)rax_fifo_under,
                     rax_dac_nsamples ? (int)(rax_dac_sum[0] / (int64_t)rax_dac_nsamples) : 0,
                     rax_dac_nsamples ? (int)(rax_dac_sum[1] / (int64_t)rax_dac_nsamples) : 0,
                     rax_dac_nsamples ? (unsigned)sqrt((double)rax_dac_sq[0] / rax_dac_nsamples) : 0,
                     rax_dac_nsamples ? (unsigned)sqrt((double)rax_dac_sq[1] / rax_dac_nsamples) : 0,
                     (unsigned)st.imask_stuck, (unsigned)st.cpu.imask_wr_pc(),
                     (unsigned)st.cpu.imask_wr_val(),
                     (unsigned)st.cpu.stat_under_cnt(), (unsigned)st.cpu.stat_under_pc(),
                     (prev_wall_us && now_us > prev_wall_us)
                        ? (double)(st.cycles_run - prev_cyc) / (double)(now_us - prev_wall_us)
                        : 0.0);
#ifdef RAX_DUMP
            /* Report the DAC/ring state the first time the board is actually
               audible (1 s of produced frames with a real level).  No timed or
               injected trigger: the attract flow decides when. */
            {
               static int audible_done;
               unsigned lv = rax_dac_nsamples
                  ? (unsigned)sqrt((double)rax_dac_sq[0] / rax_dac_nsamples) : 0;
               if (!audible_done && lv > 200u) {
                  audible_done = 1;
                  rax_log_dac_state();
               }
               if (audible_done) {
                  /* Keep the ring picture coming while the board is audible:
                     the one-shot report above lands before the writes of that
                     second, which made it read "never written". */
                  unsigned e2 = 0, o2 = 0, i2;
                  for (i2 = 0; i2 < 0x400u; i2++)
                     if (rax_ring_hit[i2]) { if (i2 & 1u) o2++; else e2++; }
                  LOG_INFO("rax: ringp ev=%u od=%u I7=%04X w0=%04X w1=%04X w2=%04X w3=%04X even=%d odd=%d",
                           e2, o2, (unsigned)st.cpu.dag_i(st.ireg & 7),
                           (unsigned)(uint16_t)data_rd(NULL, 0x3000),
                           (unsigned)(uint16_t)data_rd(NULL, 0x3001),
                           (unsigned)(uint16_t)data_rd(NULL, 0x3002),
                           (unsigned)(uint16_t)data_rd(NULL, 0x3003),
                           (int)(int16_t)data_rd(NULL, 0x3000),
                           (int)(int16_t)data_rd(NULL, 0x3001));
               }
            }
#endif
            prev_cyc = st.cycles_run;
            prev_wall_us = now_us;
            rax_dac_sum[0] = rax_dac_sum[1] = 0;
            rax_dac_sq[0] = rax_dac_sq[1] = 0;
            rax_dac_nsamples = 0;
         }
      }
#endif
   }
}

static void *rax_worker_main(void *arg)
{
   (void)arg;

   for (;;) {
      if (!__atomic_load_n(&rax_running, __ATOMIC_RELAXED))
         break;
      if (__atomic_load_n(&rax_reset_req, __ATOMIC_RELAXED) ||
          __atomic_load_n(&rax_grant, __ATOMIC_RELAXED) >
             __atomic_load_n(&rax_done, __ATOMIC_RELAXED)) {
         rax_run_worker();
         continue;
      }
      {
         struct timespec ts;
         pthread_mutex_lock(&rax_mtx);
         clock_gettime(CLOCK_MONOTONIC, &ts);
         ts.tv_nsec += RAX_TOKEN_IDLE_US * 1000;  /* same 200 us as the SCSP loop */
         if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
         }
         pthread_cond_timedwait(&rax_cv, &rax_mtx, &ts);
         pthread_mutex_unlock(&rax_mtx);
      }
   }
   return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public interface                                                          */

extern "C" int RaxIsPresent(void)
{
   return st.present;
}

extern "C" void RaxLoadRom(unsigned int offset, const unsigned char *data,
                           unsigned int length)
{
   unsigned int avail;

   if (data == NULL || offset >= RAX_ROM_SIZE)
      return;
   avail = RAX_ROM_SIZE - offset;
   if (length > avail)
      length = avail;
   memcpy(st.rom + offset, data, length);

   if (offset == 0 && length >= 0x80000u)
      st.rom_mask |= RAX_BLOB_PROGRAM;
   else if (offset == 0x400000u && length >= 0x200000u)
      st.rom_mask |= RAX_BLOB_SAMPLE0;
   else if (offset == 0x600000u && length >= 0x200000u)
      st.rom_mask |= RAX_BLOB_SAMPLE1;
   else if (offset == 0x800000u && length >= 0x200000u)
      st.rom_mask |= RAX_BLOB_SAMPLE2;
   else if (offset == 0xA00000u && length >= 0x200000u)
      st.rom_mask |= RAX_BLOB_SAMPLE3;
}

static void rax_board_reset(void)
{
   unsigned int i;

   LOG_DEBUG("rax: board reset (cycles_run was %llu)", (unsigned long long)st.cycles_run);

   memset(st.pram, 0, sizeof(st.pram));
   memset(st.bank, 0, sizeof(st.bank));
   memset(st.iram, 0, sizeof(st.iram));
   memset(st.control, 0, sizeof(st.control));

   st.rom_bank = 0;
   st.data_bank = 0;
   st.dmovlay = 0;
   st.data_in = 0;
   st.data_out = 0;
   st.pf0 = 1;

   st.ireg = 0;
   st.incs = 0;
   st.size = 0;
   st.ireg_base = 0;

   st.reg_period_fp = 0;
   st.reg_left_fp = 0;
   st.dma_period = 0;
   st.dma_left = 0;
   st.dma_param = 0;

   st.dsp_run = 0;
   st.cycles_run = 0;
   st.bdma_count = 0;

   rax_audio_reset();

   /* MAME device_reset(): the ADSP-2181 boots from the first 32 program words
      of the ROM (96 bytes, 24-bit big-endian). */
   for (i = 0; i < 32; i++) {
      uint32_t word = ((uint32_t)st.rom[i * 3 + 0] << 16) |
                      ((uint32_t)st.rom[i * 3 + 1] << 8) |
                      (uint32_t)st.rom[i * 3 + 2];
      st.pram[i] = word & 0xFFFFFF;
   }

   st.cpu.reset();
   LOG_DEBUG("rax: reset (rom mask %02X)", (unsigned)st.rom_mask);
}

extern "C" int RaxInit(void)
{
   if (!(st.rom_mask & RAX_BLOB_PROGRAM)) {
      LOG_WARN("rax: program ROM missing (mask %02X), board absent", (unsigned)st.rom_mask);
      st.present = 0;
      return 0;
   }

   if (!st.inited) {
      st.cpu.m_program.ctx = NULL;
      st.cpu.m_program.rd_w = pram_rd_w;
      st.cpu.m_program.wr_w = pram_wr_w;
      st.cpu.m_program.rd_b = NULL;
      st.cpu.m_program.wr_b = NULL;
      st.cpu.m_program.rd_d = pram_rd_d;
      st.cpu.m_program.wr_d = pram_wr_d;

      st.cpu.m_data.ctx = NULL;
      st.cpu.m_data.rd_w = data_rd;
      st.cpu.m_data.wr_w = data_wr;
      st.cpu.m_data.rd_d = NULL;
      st.cpu.m_data.wr_d = NULL;

      st.cpu.m_io.ctx = NULL;
      st.cpu.m_io.rd_w = io_rd;
      st.cpu.m_io.wr_w = io_wr;
      st.cpu.m_io.rd_d = NULL;
      st.cpu.m_io.wr_d = NULL;

      st.cpu.m_cache = st.cpu.m_program;

      st.cpu.m_sport_tx_cb.fn = sport_tx;
      st.cpu.m_sport_tx_cb.ctx = NULL;
      st.cpu.m_sport_rx_cb.fn = NULL;
      st.cpu.m_sport_rx_cb.ctx = NULL;
      st.cpu.m_timer_fired_cb.fn = NULL;
      st.cpu.m_timer_fired_cb.ctx = NULL;
      st.cpu.m_dmovlay_cb.fn = dmovlay_cb;
      st.cpu.m_dmovlay_cb.ctx = NULL;

#ifndef RAX_TEST_TRACE
      st.cpu.fast_pram = st.pram;
      st.cpu.fast_iram = st.iram;
      st.cpu.fast_bank_cur = st.bank[0];
#else
      st.cpu.fast_pram = NULL;
      st.cpu.fast_iram = NULL;
      st.cpu.fast_bank_cur = NULL;
#endif
      st.cpu.start(adsp21xx_device::CHIP_TYPE_ADSP2181);
      st.inited = 1;
   }

   st.present = 1;
   st.due_acc = 0;
   rax_board_reset();

   /* No startup throughput benchmark here: at this point the DSP has only the
      32 boot words and drops straight into IDLE, so it measured nothing and
      printed nonsense (20.4 Gcyc/s).  RAX_PERF=1 reports the real thing while
      the game runs. */

#ifndef RAX_SINGLE_THREAD
   if (!rax_thread_up) {
      pthread_mutex_lock(&rax_mtx);
      rax_grant = 0;
      rax_done = 0;
      rax_hostq_head = rax_hostq_tail = 0;
      __atomic_store_n(&rax_running, 1, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&rax_mtx);
      if (pthread_create(&rax_thread, NULL, rax_worker_main, NULL) == 0) {
         rax_thread_up = 1;
         LOG_INFO("rax: DSP worker started");
      } else {
         LOG_WARN("rax: DSP worker failed to start, board will not run");
      }
   }
#endif

   LOG_INFO("rax: board ready (rom mask %02X)", (unsigned)st.rom_mask);
   return 1;
}

#ifdef RAX_DIAG_WRITEPC
/* Who writes the sound-comms port.  The same game sends 207 words on this
   build where MAME's run sends 14, so the caller's PC is the first thing a
   single device run has to answer. */
static struct { unsigned pc; unsigned n; uint16_t first, last; } rax_wpc[16];
static unsigned rax_wpc_n;

extern "C" void RaxTraceWritePC(unsigned port, unsigned short val, unsigned pc)
{
   unsigned i;
   (void)port;
   for (i = 0; i < rax_wpc_n; i++) {
      if (rax_wpc[i].pc == pc) {
         rax_wpc[i].n++;
         rax_wpc[i].last = val;
         return;
      }
   }
   if (rax_wpc_n < sizeof(rax_wpc) / sizeof(rax_wpc[0])) {
      rax_wpc[rax_wpc_n].pc = pc;
      rax_wpc[rax_wpc_n].n = 1;
      rax_wpc[rax_wpc_n].first = val;
      rax_wpc[rax_wpc_n].last = val;
      rax_wpc_n++;
   }
}
#else
extern "C" void RaxTraceWritePC(unsigned, unsigned short, unsigned) {}
#endif

static void rax_log_dac_state(void)
{
#ifdef RAX_DUMP
   rax_dac_trace = 1;         /* next reads get logged, see rax_dac_irq */
#endif
   {
      unsigned i, even = 0, odd = 0;
      for (i = 0; i < 0x400u; i++) {
         if (!rax_ring_hit[i])
            continue;
         if (i & 1u) odd++; else even++;
      }
      LOG_INFO("rax: ring 3000-33FF written even=%u odd=%u; hist mod8 %u %u %u %u %u %u %u %u",
               even, odd,
               (unsigned)rax_ring_hist[0], (unsigned)rax_ring_hist[1],
               (unsigned)rax_ring_hist[2], (unsigned)rax_ring_hist[3],
               (unsigned)rax_ring_hist[4], (unsigned)rax_ring_hist[5],
               (unsigned)rax_ring_hist[6], (unsigned)rax_ring_hist[7]);
      LOG_INFO("rax: dac ireg=%u base=%04X incs=%d size=%u, I7=%04X M4=%04X L7=%04X, autobuf=%04X sclk=%04X sys=%04X",
               (unsigned)st.ireg, (unsigned)st.ireg_base, (int)st.incs, (unsigned)st.size,
               (unsigned)st.cpu.dag_i(7), (unsigned)st.cpu.dag_m(4), (unsigned)st.cpu.dag_l(7),
               (unsigned)st.control[REG_S0_AUTOBUF], (unsigned)st.control[REG_S0_SCLKDIV],
               (unsigned)st.control[REG_SYSCONTROL]);
      {
         unsigned k;
         for (k = 0; k < 8; k++)
            LOG_INFO("rax: dag%d I=%04X M=%04X L=%04X", k,
                     (unsigned)st.cpu.dag_i((int)k), (unsigned)st.cpu.dag_m((int)k),
                     (unsigned)st.cpu.dag_l((int)k));
      }
   }
}

extern "C" void RaxShutdown(void)
{
#ifndef RAX_SINGLE_THREAD
   if (rax_thread_up) {
      __atomic_store_n(&rax_running, 0, __ATOMIC_RELEASE);
      pthread_mutex_lock(&rax_mtx);
      pthread_cond_signal(&rax_cv);
      pthread_mutex_unlock(&rax_mtx);
      pthread_join(rax_thread, NULL);
      rax_thread_up = 0;
   }
#endif
   {
      unsigned k;
      LOG_INFO("rax: dsp pc=%04X imask=%03X irq9=%u idle=%d",
               (unsigned)st.cpu.pc(), (unsigned)st.cpu.dbg_imask(),
               (unsigned)st.cpu.dbg_state(9), st.cpu.dbg_idle());
      for (k = 0; k < rax_dbg_n && k < 32; k++)
         LOG_INFO("rax: w[%u] cyc=%llu w%d val=%04X", k,
                  (unsigned long long)rax_dbg_cyc[k], rax_dbg_w16[k] ? 16 : 32,
                  (unsigned)rax_dbg_val[k]);
#ifdef RAX_DUMP
      {
         FILE *hf = fopen("/mnt/sdcard/rax-host.txt", "w");
         if (hf != NULL) {
            for (k = 0; k < rax_dbg_n; k++)
               fprintf(hf, "%llu %04X %d\n", (unsigned long long)rax_dbg_cyc[k],
                       (unsigned)rax_dbg_val[k], (int)rax_dbg_w16[k]);
            fclose(hf);
         }
         adsp_evt_dump("/mnt/sdcard/rax-evt.txt");
         LOG_INFO("rax: wrote %u host words + interrupt events", (unsigned)rax_dbg_n);
      }
#endif
   }
#ifdef RAX_DUMP
   {
      FILE *f = fopen("/mnt/sdcard/rax-dac.raw", "wb");
      if (f != NULL) {
         fwrite(rax_dump_dac, sizeof(int16_t), sizeof(rax_dump_dac) / sizeof(int16_t), f);
         fclose(f);
      }
      f = fopen("/mnt/sdcard/rax-out.raw", "wb");
      if (f != NULL) {
         fwrite(rax_dump_out, sizeof(int16_t), sizeof(rax_dump_out) / sizeof(int16_t), f);
         fclose(f);
      }
      /* Data-space dump: internal RAM (0x2000..0x3FDF) and the five banks, so
         the mixer state (0x3A0x decoders, 0x3B0x voices, 0x3E00 sample source)
         can be inspected offline. */
      f = fopen("/mnt/sdcard/rax-iram.raw", "wb");
      if (f != NULL) {
         fwrite(st.iram, sizeof(uint16_t), RAX_IRAM_WORDS, f);
         fclose(f);
      }
      f = fopen("/mnt/sdcard/rax-banks.raw", "wb");
      if (f != NULL) {
         fwrite(st.bank, sizeof(uint16_t), RAX_BANKS * RAX_BANK_WORDS, f);
         fclose(f);
      }
      /* Program memory holds the mixer staging block every half buffer. */
      f = fopen("/mnt/sdcard/rax-pram.raw", "wb");
      if (f != NULL) {
         fwrite(st.pram, sizeof(uint32_t), RAX_PRAM_WORDS, f);
         fclose(f);
      }
      {
         char b[512];
         int n = 0, k;
         for (k = 0; k < 8; k++)
            n += snprintf(b + n, sizeof(b) - n, " I%d=%04X", k, (unsigned)(st.cpu.dag_i(k) & 0x3FFF));
         LOG_INFO("rax: DAG I:%s", b);
         n = 0;
         for (k = 0; k < 8; k++)
            n += snprintf(b + n, sizeof(b) - n, " M%d=%d", k, (int)st.cpu.dag_m(k));
         LOG_INFO("rax: DAG M:%s", b);
         n = 0;
         for (k = 0; k < 8; k++)
            n += snprintf(b + n, sizeof(b) - n, " L%d=%u", k, (unsigned)st.cpu.dag_l(k));
         LOG_INFO("rax: DAG L:%s", b);
         {
            unsigned i0 = st.cpu.dag_i(0) & 0x3FFF;
            LOG_INFO("rax: @I0=%04X: %04X %04X %04X %04X %04X %04X %04X %04X",
                     i0, (unsigned)data_rd(NULL, i0), (unsigned)data_rd(NULL, i0 + 1),
                     (unsigned)data_rd(NULL, i0 + 2), (unsigned)data_rd(NULL, i0 + 3),
                     (unsigned)data_rd(NULL, i0 + 4), (unsigned)data_rd(NULL, i0 + 5),
                     (unsigned)data_rd(NULL, i0 + 6), (unsigned)data_rd(NULL, i0 + 7));
         }
      }
      LOG_INFO("rax: iram dumped, 3A02=%04X 3A05=%04X 3A06=%04X 3AC5=%04X 3AC6=%04X",
               (unsigned)st.iram[0x3A02 - 0x2000], (unsigned)st.iram[0x3A05 - 0x2000],
               (unsigned)st.iram[0x3A06 - 0x2000], (unsigned)st.iram[0x3AC5 - 0x2000],
               (unsigned)st.iram[0x3AC6 - 0x2000]);
      LOG_INFO("rax: dumped 8 s rings (dac %u pos, out %u pos)",
               (unsigned)rax_dump_dac_pos, (unsigned)rax_dump_out_pos);
   }
#endif
   LOG_INFO("rax: host words in=%u delivered=%u taken=%u, cmd bdma=%u, dac starts=%u, "
            "frames=%llu, lost=%u",
            (unsigned)st.host_in, (unsigned)st.host_delivered, (unsigned)st.host_read,
            (unsigned)st.cmd_bdma, (unsigned)st.dac_starts, (unsigned long long)st.dac_frames,
            (unsigned)rax_lost_words);
#ifdef RAX_DUMP
   rax_log_dac_state();
#endif
#ifdef RAX_DIAG_WRITEPC
   {
      unsigned i;
      for (i = 0; i < rax_wpc_n; i++)
         LOG_INFO("rax: sound-comms writes from pc=%08X n=%u first=%04X last=%04X",
                  rax_wpc[i].pc, rax_wpc[i].n, (unsigned)rax_wpc[i].first,
                  (unsigned)rax_wpc[i].last);
   }
#endif
   st.present = 0;
   st.rom_mask = 0;
   rax_audio_reset();
   memset(st.rom, 0, sizeof(st.rom));
}

/* Machine reset: the worker owns the board state, so ask it to reset.  At init
   time (no worker yet) the reset runs inline. */
extern "C" void RaxReset(void)
{
   if (!st.present)
      return;

   st.due_acc = 0;

   if (!rax_thread_up) {
      rax_board_reset();
      return;
   }

   pthread_mutex_lock(&rax_mtx);
   rax_reset_req = 1;
   rax_reset_done = 0;
   rax_grant = 0;
   rax_hostq_head = rax_hostq_tail = 0;
   pthread_mutex_unlock(&rax_mtx);
   pthread_cond_signal(&rax_cv);

   /* Wait for the worker to actually reset the board.  Without this, the SH-2
      could start writing host words while the reset was still pending, and
      whether they were lost depended on thread scheduling -- the "different
      state on every boot" the device showed. */
   {
      int spins = 0;
      pthread_mutex_lock(&rax_mtx);
      while (!rax_reset_done && spins++ < 500) {   /* <= 50 ms, then give up */
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         ts.tv_nsec += 100000;
         if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
         pthread_cond_timedwait(&rax_cv, &rax_mtx, &ts);
      }
      pthread_mutex_unlock(&rax_mtx);
   }
}

/* Machine thread: publish the emulated time this pass stands for as a DSP
   cycle grant.  Never runs the DSP and never waits for it. */
extern "C" void RaxAdvance(unsigned int sh2_cycles)
{
   uint64_t due;

   if (!st.present)
      return;

   /* The accumulator keeps the fraction, so the DSP and the SH-2 cannot drift. */
   st.due_acc += (uint64_t)sh2_cycles * RAX_DSP_CLOCK;
   due = st.due_acc / RAX_SH2_CLOCK;

#ifdef RAX_SINGLE_THREAD
   /* Host harness: no worker, run it here (the board state is ours).  The
      grant is still published so queued host words get a meaningful
      timestamp (RAX_HOSTQ_TEST builds deliver them at that cycle). */
   if (due > __atomic_load_n(&rax_grant, __ATOMIC_RELAXED))
      __atomic_store_n(&rax_grant, due, __ATOMIC_RELEASE);
   if (due > st.dsp_run) {
      rax_run_cycles((uint32_t)(due - st.dsp_run));
      st.dsp_run = due;
   }
#else
   /*
      Published as an atomic store: the machine thread must not take a lock and
      must not wake the worker on every emulation pass (15.7k/s).  The worker
      polls the grant at 1 ms and is signalled on host writes. */
   if (due > __atomic_load_n(&rax_grant, __ATOMIC_RELAXED))
      __atomic_store_n(&rax_grant, due, __ATOMIC_RELEASE);

   /* Pure token model, like the SCSP thread: the machine only publishes the
      emulated time it has advanced and never waits.  The DSP worker consumes
      exactly that much, and because the board's work is cheaper than the
      machine's it keeps up on its own (see RAX_TOKEN_IDLE_US). */
#endif
}

/* SH-2 -> RAX: latch the command word with the machine cycle it arrived at and
   let the worker deliver it when its own clock gets there.  No waiting. */
extern "C" void RaxDataWrite(unsigned short data)
{
   RaxDataWriteEx(data, 0);
}

/* word16 != 0: the SH-2 used a 16-bit store (the ST-V driver only ever does
   32-bit stores; MAME would ignore a 16-bit access there). */
extern "C" void RaxDataWriteEx(unsigned short data, int word16)
{
   if (!st.present)
      return;
   st.host_in16 += word16 ? 1u : 0u;
   /* Per-word trace of what the SH-2 side sends (off unless RAX_LOG_HOST is
      set).  Same shape as MAME's ADSP_TRACE_HOST line so the two streams can be
      diffed one by one: this is the only input the DSP ever gets. */
   {
      static int log_host = -1;
      if (log_host < 0) { const char *e = getenv("RAX_LOG_HOST"); log_host = (e && *e != '\0') ? 1 : 0; }
      if (log_host)
         LOG_INFO("rax: host cyc=%llu w%d val=%04X",
                  (unsigned long long)__atomic_load_n(&rax_grant, __ATOMIC_RELAXED),
                  word16 ? 16 : 32, (unsigned)data);
   }
   if (rax_dbg_n < RAX_DBG_WORDS) {
      rax_dbg_val[rax_dbg_n] = data;
      rax_dbg_cyc[rax_dbg_n] = __atomic_load_n(&rax_grant, __ATOMIC_RELAXED);
      rax_dbg_w16[rax_dbg_n] = (uint8_t)(word16 != 0);
      rax_dbg_n++;
   }

#if defined(RAX_SINGLE_THREAD) && !defined(RAX_HOSTQ_TEST)
   st.data_in = data;
   st.cpu.set_irq_line(ADSP2181_IRQL0, 1);
   rax_run_cycles(RAX_HOST_QUANTUM);
   st.dsp_run += RAX_HOST_QUANTUM;
#else
   st.host_in++;
   pthread_mutex_lock(&rax_mtx);
   {
      unsigned next = (rax_hostq_tail + 1u) % RAX_HOSTQ;
      if (next == rax_hostq_head) {
         /* Queue full: drop the oldest word (cannot happen at the game's rate). */
         rax_hostq_head = (rax_hostq_head + 1u) % RAX_HOSTQ;
         rax_lost_words++;
      }
      rax_hostq_val[rax_hostq_tail] = data;
      rax_hostq_cyc[rax_hostq_tail] = __atomic_load_n(&rax_grant, __ATOMIC_RELAXED);
      rax_hostq_tail = next;
   }
   pthread_mutex_unlock(&rax_mtx);
#ifndef RAX_SINGLE_THREAD
   pthread_cond_signal(&rax_cv);
#endif
#endif
}

/* Read-step for the FIFO consumers (device mix and the offline renderer use
   the same math).

   The nominal ratio rax_step = (DSP_CLOCK/frame_cycles)/44100 is not what the
   board actually produces: measured, it is 0.3% fast, so an open-loop reader
   drains the 2048-frame bridge in ~40 s and the next lag spike becomes a
   dropout (fill 2048 -> ~400, one field of silence every few seconds).  The
   fix is to derive the ratio from what the DAC really produced: frames per
   executed DSP cycle, re-averaged once per DSP second.  The residual slow trim
   towards the prefill level then only has to absorb jitter. */
static uint64_t rax_cal_frames;
static uint64_t rax_cal_out;
static double   rax_step_cal;

/* step = DAC frames produced per output frame asked for.  Measured, because
   neither side is exactly nominal: the board emits ~31.8k frames per DSP
   second and the SCSP thread asks RaxMix for ~44.9k output frames per second
   (+1.8% over 44.1k), so any ratio derived from the nominal clocks drains the
   FIFO and the dropouts follow.  With this the consumer pops exactly what the
   DAC pushes; the trim below only absorbs jitter. */
static void rax_calibrate_step(void)
{
   uint64_t df, dout;

   if (rax_out_total < rax_cal_out || rax_out_total - rax_cal_out < 88200u)
      return;                       /* 2 s window: long enough to be burst-free */
   df = (uint64_t)st.dac_frames - rax_cal_frames;
   dout = rax_out_total - rax_cal_out;
   rax_cal_frames = st.dac_frames;
   rax_cal_out = rax_out_total;
   if (df > 0 && dout > 0) {
      double v = (double)df / (double)dout;
      if (v > 0.3 && v < 1.5) {
         if (rax_step_cal <= 0.0) {
            /* First value: must be near the nominal ratio, otherwise the
               window covered the start-up (DAC not producing yet) and would
               set the step to nonsense (measured 0.50 for a second). */
            if (rax_step > 0.0 && v > rax_step * 0.9 && v < rax_step * 1.1)
               rax_step_cal = v;
         } else {
            /* Slew-limit: one misaligned window pulled the step by 8% and was
               an audible pitch drop, so never move it more than 0.5%/window. */
            double d = v - rax_step_cal;
            if (d > 0.005) d = 0.005;
            if (d < -0.005) d = -0.005;
            rax_step_cal += d;
         }
      }
   }
}

static double rax_read_step(void)
{
   double base = (rax_step_cal > 0.0) ? rax_step_cal : rax_step;
   double trim = 1.0 + ((double)rax_fifo_fill - (double)RAX_FIFO_TARGET) * 5e-6;
   if (trim < 0.995) trim = 0.995;
   if (trim > 1.005) trim = 1.005;
   if (base <= 0.0) return 1.0;
   return base * trim;
}

/* Resampling kernel for the 31.57 kHz DAC -> 44.1 kHz output (stereo, reads
   the FIFO the caller already holds the lock for).

   Catmull-Rom (4 taps) instead of linear interpolation.  Linear's response on
   a 31.57 kHz source is -2.9 dB at 10 kHz and -6 dB at 14 kHz -- that is the
   "the music has no detail" the device listener reported -- while cubic stays
   within ~0.5 dB up to 12 kHz.  Four taps per channel per output frame is
   still nothing next to the DSP. */
static inline double rax_hermite(double y0, double y1, double y2, double y3, double t)
{
   double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
   double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
   double c = 0.5 * (y2 - y0);

   return ((a * t + b) * t + c) * t + y1;
}

static inline void rax_fifo_get(int k, int32_t *l, int32_t *r)
{
   int n = (int)rax_fifo_fill;
   unsigned idx;

   if (n <= 0) { *l = 0; *r = 0; return; }
   if (k < 0) k = 0;
   else if (k >= n) k = n - 1;
   idx = (rax_fifo_head + (unsigned)k) % RAX_FIFO_FRAMES;
   *l = rax_fifo[idx * 2 + 0];
   *r = rax_fifo[idx * 2 + 1];
}

static void rax_interp_frame(uint32_t idx, double frac, int32_t *out_l, int32_t *out_r)
{
   int32_t m1l, m1r, y0l, y0r, y1l, y1r, y2l, y2r;

   rax_fifo_get((int)idx - 1, &m1l, &m1r);
   rax_fifo_get((int)idx,     &y0l, &y0r);
   rax_fifo_get((int)idx + 1, &y1l, &y1r);
   rax_fifo_get((int)idx + 2, &y2l, &y2r);
   *out_l = (int32_t)rax_hermite((double)m1l, (double)y0l, (double)y1l, (double)y2l, frac);
   *out_r = (int32_t)rax_hermite((double)m1r, (double)y0r, (double)y1r, (double)y2r, frac);
}

extern "C" void RaxMix(int *left, int *right, unsigned int frames)
{
   unsigned int i;

   if (!st.present || left == NULL || right == NULL)
      return;
   rax_out_total += frames;

   rax_calibrate_step();

   pthread_mutex_lock(&rax_audio_mtx);
   if (!rax_dac_on) {
      pthread_mutex_unlock(&rax_audio_mtx);
      return;
   }
   if (!rax_fifo_primed) {
      /* Startup cushion: the DSP fills in 128-frame bursts every ~4 ms while
         this consumer takes ~132 frames per quarter field in one go.  Starting
         at an empty FIFO made every field begin with a gap (measured: 24,689
         frames of silence in 42 s), which is the crackle under the music. */
      if (rax_fifo_fill < RAX_FIFO_PREFILL) {
         rax_fifo_under += frames;
         pthread_mutex_unlock(&rax_audio_mtx);
         return;
      }
      rax_fifo_primed = 1;
      LOG_INFO("rax: fifo primed at %u frames", (unsigned)rax_fifo_fill);
   }
   if (rax_fifo_fill == 0) {
      rax_fifo_under += frames;
      pthread_mutex_unlock(&rax_audio_mtx);
      return;
   }

   double step = rax_read_step();

   for (i = 0; i < frames; i++) {
      uint32_t idx = (uint32_t)rax_fifo_pos;
      double frac = rax_fifo_pos - (double)idx;
      int32_t ol, or_;

      if (idx >= rax_fifo_fill)
         break;                 /* short ring: the rest of this chunk is silence */

      rax_interp_frame(idx, frac, &ol, &or_);
      left[i]  += ol;
      right[i] += or_;
#ifdef RAX_DUMP
      rax_dump_out[rax_dump_out_pos * 2 + 0] = (int16_t)ol;
      rax_dump_out[rax_dump_out_pos * 2 + 1] = (int16_t)or_;
      rax_dump_out_pos = (rax_dump_out_pos + 1u) % RAX_DUMP_OUT_FRAMES;
#endif

      rax_fifo_pos += step;
      while (rax_fifo_pos >= 1.0 && rax_fifo_fill > 0) {
         rax_fifo_head = (rax_fifo_head + 1u) % RAX_FIFO_FRAMES;
         rax_fifo_fill--;
         rax_fifo_pos -= 1.0;
         rax_pop_total++;
      }
      if (rax_fifo_fill == 0)
         rax_fifo_pos = 0.0;
   }

   /* Rate accounting: production (DAC frames) vs consumption (source frames
      popped), one line per DSP second, so an imbalance is a number and not a
      guess. */
   {
      static uint64_t prev_dac, prev_pop, prev_out, next_log;
      if (rax_perf_on > 0 && st.dsp_run >= next_log) {
         next_log = st.dsp_run + RAX_DSP_CLOCK;
         LOG_INFO("rax: mixd out=%llu dac=%llu pop=%llu step=%.4f fill=%u",
                  (unsigned long long)(rax_out_total - prev_out),
                  (unsigned long long)(st.dac_frames - prev_dac),
                  (unsigned long long)(rax_pop_total - prev_pop),
                  step, (unsigned)rax_fifo_fill);
         prev_dac = st.dac_frames;
         prev_pop = rax_pop_total;
         prev_out = rax_out_total;
      }
   }

   pthread_mutex_unlock(&rax_audio_mtx);
}

extern "C" int RaxSaveState(void *fp)
{
   (void)fp;
   return -1;                    /* phase 2: no RAX chunk in the state format yet */
}

extern "C" int RaxLoadState(void *fp)
{
   (void)fp;
   return -1;
}

#ifdef RAX_TEST_TRACE
/* Host-side harness only: dump one memory region in MAME's `dumpd` text format
   so it can be diffed against the reference dumps in .notes/ref/rax-old/work.
   space: 0 = program (4 words/line, 8 hex digits), 1 = data space as if the
   board's current bank were selected (8 words/line, 4 hex digits). */
extern "C" void RaxTestDump(const char *path, int space, unsigned addr, unsigned count)
{
   FILE *f = fopen(path, "w");
   unsigned i;

   if (f == NULL)
      return;
   if (space == 0) {
      for (i = 0; i < count; i += 4) {
         unsigned j;
         fprintf(f, "%04X: ", (addr + i) & 0x3FFF);
         for (j = 0; j < 4; j++)
            fprintf(f, " %08X", st.pram[(addr + i + j) & (RAX_PRAM_WORDS - 1)]);
         fprintf(f, "\n");
      }
   } else {
      for (i = 0; i < count; i += 8) {
         unsigned j;
         fprintf(f, "%04X: ", (addr + i) & 0x3FFF);
         for (j = 0; j < 8; j++)
            fprintf(f, " %04X", data_rd(NULL, (addr + i + j) & 0x3FFF));
         fprintf(f, "\n");
      }
   }
   fclose(f);
}

extern "C" void RaxTestSetTrace(void (*fn)(void *, unsigned, unsigned), void *ctx)
{
   st.cpu.m_trace_cb = fn;
   st.cpu.m_trace_ctx = ctx;
}

extern "C" unsigned RaxTestIRQState(int n) { return st.cpu.dbg_state(n); }
extern "C" unsigned RaxTestIRQLatch(int n) { return st.cpu.dbg_latch(n); }
extern "C" unsigned RaxTestIMask(void) { return st.cpu.dbg_imask(); }
/* Render n 44.1 kHz stereo frames exactly like RaxMix, but into dst (harness). */
extern "C" void RaxTestRender(short *dst, unsigned frames)
{
   unsigned i;
   double step = rax_read_step();
   pthread_mutex_lock(&rax_audio_mtx);
   for (i = 0; i < frames; i++) {
      uint32_t idx = (uint32_t)rax_fifo_pos;
      double frac = rax_fifo_pos - (double)idx;
      int32_t ol = 0, or_ = 0;
      if (rax_fifo_fill > 0 && idx < rax_fifo_fill)
         rax_interp_frame(idx, frac, &ol, &or_);
      dst[2 * i]     = (short)ol;
      dst[2 * i + 1] = (short)or_;
      rax_fifo_pos += step;
      while (rax_fifo_pos >= 1.0 && rax_fifo_fill > 0) {
         rax_fifo_head = (rax_fifo_head + 1u) % RAX_FIFO_FRAMES;
         rax_fifo_fill--;
         rax_fifo_pos -= 1.0;
      }
      if (rax_fifo_fill == 0)
         rax_fifo_pos = 0.0;
   }
   pthread_mutex_unlock(&rax_audio_mtx);
}

extern "C" unsigned RaxTestDag(int kind, int idx)
{
   if (kind == 0) return st.cpu.dag_i(idx) & 0x3FFF;
   if (kind == 1) return (unsigned)(int)st.cpu.dag_m(idx);
   return st.cpu.dag_l(idx);
}

extern "C" unsigned RaxTestCounters(unsigned *in, unsigned *taken, unsigned *deliv, unsigned *cmd)
{
   if (in) *in = st.host_in;
   if (taken) *taken = st.host_read;
   if (deliv) *deliv = st.host_delivered;
   if (cmd) *cmd = st.cmd_bdma;
   return rax_hostq_head;
}

extern "C" unsigned long long RaxTestGrant(void)
{
#ifdef RAX_SINGLE_THREAD
   return st.dsp_run;
#else
   return __atomic_load_n(&rax_grant, __ATOMIC_RELAXED);
#endif
}

extern "C" void RaxTestSync(void)
{
   int spins = 0;
   while (spins++ < 20000) {
      uint64_t d, g;
      struct timespec ts;
      pthread_mutex_lock(&rax_mtx);
      d = rax_done;
      g = rax_grant;
      pthread_mutex_unlock(&rax_mtx);
      if (d >= g) {
         return;
      }
      if (spins < 3)
         LOG_INFO("rax: sync d=%llu g=%llu", (unsigned long long)d, (unsigned long long)g);
      ts.tv_sec = 0;
      ts.tv_nsec = 500000;
      nanosleep(&ts, NULL);
   }
}

extern "C" void RaxTestSavePramBin(const char *path)
{
   FILE *f = fopen(path, "wb");
   if (f != NULL) {
      fwrite(st.pram, sizeof(st.pram[0]), RAX_PRAM_WORDS, f);
      fclose(f);
   }
}

extern "C" unsigned RaxTestPC(void) { return st.cpu.pc(); }
extern "C" void RaxTestDacState(unsigned *base, unsigned *i7, int *incs, unsigned *size)
{
   if (base) *base = st.ireg_base;
   if (i7)   *i7   = st.cpu.dag_i(st.ireg & 7);
   if (incs) *incs = st.incs;
   if (size) *size = st.size;
}

extern "C" unsigned RaxTestDAC(double *step, unsigned *fill)
{
   unsigned on;
   pthread_mutex_lock(&rax_audio_mtx);
   on = rax_dac_on;
   if (step) *step = rax_step;
   if (fill) *fill = rax_fifo_fill;
   pthread_mutex_unlock(&rax_audio_mtx);
   return on;
}
extern "C" unsigned RaxTestBDMACount(void) { return st.bdma_count; }
extern "C" unsigned long long RaxTestCycles(void) { return st.cycles_run; }
#endif
