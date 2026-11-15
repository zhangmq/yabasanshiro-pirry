#include <stdlib.h>
/* ADSP-21xx (ADSP-2181) interpreter for the Acclaim RAX sound board.
 *
 * The opcode engine is MAME's 2100ops.hxx, vendored unmodified (BSD-3-Clause,
 * copyright-holders: Aaron Giles); this file is the same port of MAME's
 * adsp2100.cpp with the device framework taken out:
 *   - no address spaces: program/data/IO forward to adsp_mem_iface callbacks
 *     that the RAX board installs (MAME's rax.cpp owns those maps too);
 *   - no scheduler/timers (the board drives those, as in MAME);
 *   - no savestates, debugger or disassembler.
 * Semantics are meant to be identical to MAME's core: when the RAX board is
 * wired up, the DSP should execute the same instruction stream.
 */
#include "adsp2100_port.h"

#if defined(RAX_TRACE_MIX) && defined(RAX_TEST_TRACE)
/* Board time ledger from rax.cpp; only the offline harness links it. */
extern "C" unsigned long long RaxTestGrant(void);
#endif

#ifdef RAX_TRACE_IMASK
/* Ring of the last ADSP_EVT_N interrupt events (see adsp2100_port.h).  A device
   run that goes silent can then show the exact instruction that masked IRQL0
   and whether the status stack still matched it. */
namespace {
struct adsp_evt_entry { unsigned long long seq; unsigned long long cyc; int kind; unsigned a, b, c; };
const int ADSP_EVT_N = 65536;
adsp_evt_entry g_evt[ADSP_EVT_N];
unsigned long long g_evt_seq;
}
unsigned long long g_evt_cycle;
void adsp_evt_set_cycle(unsigned long long c) { g_evt_cycle = c; }

void adsp_evt_note(int kind, unsigned a, unsigned b, unsigned c)
{
	unsigned long long s = g_evt_seq++;
	adsp_evt_entry &e = g_evt[s % ADSP_EVT_N];
	e.seq = s; e.cyc = g_evt_cycle; e.kind = kind; e.a = a; e.b = b; e.c = c;
}

void adsp_evt_dump(const char *path)
{
	FILE *f = fopen(path, "w");
	unsigned long long first;
	if (f == NULL)
		return;
	first = (g_evt_seq > ADSP_EVT_N) ? (g_evt_seq - ADSP_EVT_N) : 0;
	for (unsigned long long s = first; s < g_evt_seq; s++) {
		const adsp_evt_entry &e = g_evt[s % ADSP_EVT_N];
		fprintf(f, "%llu %d %04X %04X %04X %llu\n", e.seq, e.kind, e.a, e.b, e.c, e.cyc);
	}
	fclose(f);
}
#else
void adsp_evt_note(int, unsigned, unsigned, unsigned) {}
void adsp_evt_dump(const char *) {}
void adsp_evt_set_cycle(unsigned long long) {}
#endif

void adsp21xx_device::create_tables()
{
	// initialize the bit reversing table
	for (int i = 0; i < 0x4000; i++)
	{
		uint16_t data = 0;

		data |= (i >> 13) & 0x0001;
		data |= (i >> 11) & 0x0002;
		data |= (i >> 9)  & 0x0004;
		data |= (i >> 7)  & 0x0008;
		data |= (i >> 5)  & 0x0010;
		data |= (i >> 3)  & 0x0020;
		data |= (i >> 1)  & 0x0040;
		data |= (i << 1)  & 0x0080;
		data |= (i << 3)  & 0x0100;
		data |= (i << 5)  & 0x0200;
		data |= (i << 7)  & 0x0400;
		data |= (i << 9)  & 0x0800;
		data |= (i << 11) & 0x1000;
		data |= (i << 13) & 0x2000;

		m_reverse_table[i] = data;
	}

	// initialize the mask table
	for (int i = 0; i < 0x4000; i++)
	{
		if (i > 0x2000)      m_mask_table[i] = 0x0000;
		else if (i > 0x1000) m_mask_table[i] = 0x2000;
		else if (i > 0x0800) m_mask_table[i] = 0x3000;
		else if (i > 0x0400) m_mask_table[i] = 0x3800;
		else if (i > 0x0200) m_mask_table[i] = 0x3c00;
		else if (i > 0x0100) m_mask_table[i] = 0x3e00;
		else if (i > 0x0080) m_mask_table[i] = 0x3f00;
		else if (i > 0x0040) m_mask_table[i] = 0x3f80;
		else if (i > 0x0020) m_mask_table[i] = 0x3fc0;
		else if (i > 0x0010) m_mask_table[i] = 0x3fe0;
		else if (i > 0x0008) m_mask_table[i] = 0x3ff0;
		else if (i > 0x0004) m_mask_table[i] = 0x3ff8;
		else if (i > 0x0002) m_mask_table[i] = 0x3ffc;
		else if (i > 0x0001) m_mask_table[i] = 0x3ffe;
		else                 m_mask_table[i] = 0x3fff;
	}

	// initialize the condition table
	for (int i = 0; i < 0x100; i++)
	{
		int az = ((i & ZFLAG) != 0);
		int an = ((i & NFLAG) != 0);
		int av = ((i & VFLAG) != 0);
		int ac = ((i & CFLAG) != 0);
		int mv = ((i & MVFLAG) != 0);
		int as = ((i & SFLAG) != 0);

		m_condition_table[i | 0x000] = az;
		m_condition_table[i | 0x100] = !az;
		m_condition_table[i | 0x200] = !((an ^ av) | az);
		m_condition_table[i | 0x300] = (an ^ av) | az;
		m_condition_table[i | 0x400] = an ^ av;
		m_condition_table[i | 0x500] = !(an ^ av);
		m_condition_table[i | 0x600] = av;
		m_condition_table[i | 0x700] = !av;
		m_condition_table[i | 0x800] = ac;
		m_condition_table[i | 0x900] = !ac;
		m_condition_table[i | 0xa00] = as;
		m_condition_table[i | 0xb00] = !as;
		m_condition_table[i | 0xc00] = mv;
		m_condition_table[i | 0xd00] = !mv;
		m_condition_table[i | 0xf00] = 1;
	}
}



/***************************************************************************
    CORE EXECUTION LOOP
***************************************************************************/

//-------------------------------------------------
//  execute_min_cycles - return minimum number of
//  cycles it takes for one instruction to execute
//-------------------------------------------------


inline uint16_t adsp21xx_device::data_read(uint32_t addr)
{
	if (fast_pram != NULL)
	{
		addr &= 0x3fff;
		if (addr < 0x2000)
			return fast_bank_cur[addr];
		if (addr < 0x3fe0)
			return fast_iram[addr - 0x2000];
	}
	return m_data.read_word(addr);
}

inline void adsp21xx_device::data_write(uint32_t addr, uint16_t data)
{
#ifdef RAX_TRACE_MIX
	/* Window log of the per-sample-phase tables, with the same format as the
	   probe in MAME's adsp2100.cpp (ADSP_WINLOG), so the two streams can be
	   diffed event by event.  Time base: board DSP time / XTAL. */
	{
		static int win = -1;
		if (win < 0) { const char *e = getenv("RAX_WINLOG"); win = (e && *e != '\0') ? 1 : 0; }
		if (win) {
			const double t = (double)RaxTestGrant() / 16670000.0;
			const unsigned aa = addr & 0x3fff;
			{
				/* Interpolation loop per-iteration state (same output as MAME's
				   ADSP_DECPROBE probe). */
				if (m_pc == 0x3E67 && t >= 20.215 && t <= 20.235) {
					fprintf(stderr, "[D] t=%.6f it=%u AR=%04X MX0=%04X MF=%04X MR=%02X%04X%04X ASTAT=%04X MSTAT=%04X\n",
						t, (unsigned)m_cntr, (unsigned)(m_core.ar.u & 0xffff),
						(unsigned)(m_core.mx0.u & 0xffff), (unsigned)(m_core.mf.u & 0xffff),
						(unsigned)(m_core.mr.mrx.mr2.u & 0xff), (unsigned)(m_core.mr.mrx.mr1.u & 0xffff),
						(unsigned)(m_core.mr.mrx.mr0.u & 0xffff), (unsigned)m_astat, (unsigned)m_mstat);
				}
				if (m_pc == 0x3E68 && t >= 20.215 && t <= 20.235) {
					fprintf(stderr, "[C] t=%.6f it=%u MR=%02X%04X%04X ASTAT=%04X\n",
						t, (unsigned)m_cntr,
						(unsigned)(m_core.mr.mrx.mr2.u & 0xff), (unsigned)(m_core.mr.mrx.mr1.u & 0xffff),
						(unsigned)(m_core.mr.mrx.mr0.u & 0xffff), (unsigned)m_astat);
				}
				/* Who fills the decoded-sample buffer at 0x2000/0x2400? */
				static int n20, n24;
				{
					static int n3e;
					if (aa >= 0x3e00 && aa < 0x3e08 && data == 0x8000 && n3e < 20) {
						fprintf(stderr, "[E] t=%.6f pc=%04X a=%04X v=%04X\n", t, (unsigned)m_ppc, aa, (unsigned)data);
						n3e++;
					}
				}
				if (aa == 0x2000 && data != 0 && n20 < 12) {
					fprintf(stderr, "[B] t=%.6f pc=%04X a=%04X v=%04X\n", t, (unsigned)m_ppc, aa, (unsigned)data);
					n20++;
				}
				if (aa == 0x2400 && data != 0 && n24 < 12) {
					fprintf(stderr, "[B] t=%.6f pc=%04X a=%04X v=%04X\n", t, (unsigned)m_ppc, aa, (unsigned)data);
					n24++;
				}
			}
			if (aa >= 0x3f28 && aa < 0x3f30)
				fprintf(stderr, "[S] t=%.6f pc=%04X a=%04X v=%04X\n",
					t, (unsigned)m_ppc, aa, (unsigned)data);
			if (t >= 24.90 && t <= 25.06 &&
				((aa >= 0x3b00 && aa < 0x3b60) || aa == 0x3bdb || aa == 0x3bdc))
				fprintf(stderr, "[W] t=%.6f pc=%04X a=%04X v=%04X sr0=%04X\n",
					t, (unsigned)m_ppc, aa, (unsigned)data, (unsigned)(m_core.sr.sr & 0xffff));
		}
	}
	{
		static int shown_trap;
		unsigned aa = addr & 0x3fff;
		if ((aa == 0x3B00u || aa == 0x3B0Eu) && shown_trap < 6) {
			fprintf(stderr, "TRAP addr=%04X pc=%04X val=%d\n", aa, (unsigned)m_ppc, (int)(int16_t)data);
			shown_trap++;
		}
	}
	/* 0x3CF4 is inside the "voice idle" path of 0x3C35 (it stores 0x28 into
	   the staging).  Counting it against the number of 0x3C35 calls tells
	   whether the DSP really idles more than MAME or just samples differ. */
	if (m_ppc == 0x3CF4) {
		static unsigned long idle_n, call_n;
		static int reg2;
		idle_n++;
		if (!reg2) { reg2 = 1; (void)call_n; atexit([](){
			fprintf(stderr, "IDLEP idle_stores=%lu\n", idle_n); }); }
	}
	/* Which instructions write the DAC ring at all (counts per PC). */
	{
		static struct { unsigned pc; unsigned long n; } tab[32];
		static int tab_n;
		static int dumped;
		if (!dumped) { dumped = 1; atexit([](){
			for (int i = 0; i < tab_n; i++)
				fprintf(stderr, "RINGPC pc=%04X n=%lu\n", tab[i].pc, tab[i].n);
		}); }
		if ((addr & 0x3fff) >= 0x3000 && (addr & 0x3fff) < 0x3400) {
			int i;
			for (i = 0; i < tab_n; i++) if (tab[i].pc == m_ppc) break;
			if (i == tab_n && tab_n < 32) { tab[tab_n].pc = m_ppc; tab[tab_n].n = 0; tab_n++; }
			if (i < tab_n) tab[i].n++;
		}
	}
#endif
	if (fast_pram != NULL)
	{
		addr &= 0x3fff;
		if (addr < 0x2000)
		{
			fast_bank_cur[addr] = data;
			return;
		}
		if (addr < 0x3fe0)
		{
			fast_iram[addr - 0x2000] = data;
			return;
		}
	}
	m_data.write_word(addr, data);
}

inline uint16_t adsp21xx_device::io_read(uint32_t addr)
{
	return m_io.read_word(addr);
}

inline void adsp21xx_device::io_write(uint32_t addr, uint16_t data)
{
	m_io.write_word(addr, data);
}

inline uint32_t adsp21xx_device::program_read(uint32_t addr)
{
	if (fast_pram != NULL)
		return fast_pram[addr & 0x3fff];
	return m_program.read_dword(addr);
}

inline void adsp21xx_device::program_write(uint32_t addr, uint32_t data)
{
#ifdef RAX_TRACE_PM
	{
		static struct { unsigned pc; unsigned long n; } tab[32];
		static int tab_n, dumped, shown;
		unsigned a = addr & 0x3fff;
		if (!dumped) { dumped = 1; atexit([](){
			for (int i = 0; i < tab_n; i++) fprintf(stderr, "PMPC pc=%04X n=%lu\n", tab[i].pc, tab[i].n); }); }
		/* One pass over the staging block starts when the base word is written. */
		if (a == 0x31a3) {
			static struct { unsigned pc; unsigned long n; } pt[8];
			static int pt_n, pd;
			int k;
			if (!pd) { pd = 1; atexit([](){
				for (int i = 0; i < pt_n; i++) fprintf(stderr, "PASS pc=%04X n=%lu\n", pt[i].pc, pt[i].n); }); }
			for (k = 0; k < pt_n; k++) if (pt[k].pc == m_ppc) break;
			{
				static int shown2;
				if (shown2 < 24) {
					fprintf(stderr, "PASSAT pc=%04X ret=%04X imask=%03X\n",
						(unsigned)m_ppc, (unsigned)pc_stack_top(), (unsigned)m_imask);
					shown2++;
				}
			}
			if (k == pt_n && pt_n < 8) { pt[pt_n].pc = m_ppc; pt[pt_n].n = 0; pt_n++; }
			if (k < pt_n) pt[k].n++;
		}
		if (a >= 0x31a3 && a < 0x33a3) {
			int i;
			for (i = 0; i < tab_n; i++) if (tab[i].pc == m_ppc) break;
			if (i == tab_n && tab_n < 32) { tab[tab_n].pc = m_ppc; tab[tab_n].n = 0; tab_n++; }
			if (i < tab_n) tab[i].n++;
			if (shown < 24) { fprintf(stderr, "PMW pc=%04X addr=%04X val=%06X\n", (unsigned)m_ppc, a, data & 0xffffff); shown++; }
		}
	}
#endif
	if (fast_pram != NULL)
	{
		fast_pram[addr & 0x3fff] = data & 0xffffff;
		return;
	}
	m_program.write_dword(addr, data & 0xffffff);
}

inline uint32_t adsp21xx_device::opcode_read()
{
	if (fast_pram != NULL)
		return fast_pram[m_pc & 0x3fff];
	return m_cache.read_dword(m_pc);
}


/***************************************************************************
    IMPORT CORE UTILITIES
***************************************************************************/

#include <cstdio>
#include <stdlib.h>
#include <cstdlib>
#include <cstdlib>

#include "2100ops.hxx"



/***************************************************************************
    IRQ HANDLING
***************************************************************************/




bool adsp21xx_device::generate_irq(int which, int indx)
{
	{   /* YAB_ADSPIRQ=1: which interrupt actually got taken (diagnostic) */
		static int on = -1;
		if (on < 0) { const char *e = getenv("YAB_ADSPIRQ"); on = (e && *e && *e != '0'); }
		if (on && (m_imask & (0x200 >> indx)))
			printf("[IRQ] which=%d indx=%d vec=%02X pc=%04X imask=%03X icntl=%02X\n",
			       which, indx, 0x04 + indx * 4, (unsigned)m_pc, (unsigned)m_imask, (unsigned)m_icntl);
	}
	// skip if masked
	if (!(m_imask & (0x200 >> indx)))
		return false;

#ifdef RAX_TRACE_IMASK
	adsp_evt_note(2, (unsigned)indx, (unsigned)m_pc, (unsigned)m_imask);
#endif

	// clear the latch
	m_irq_latch[which] = 0;

	// push the PC and the status
	pc_stack_push();
	stat_stack_push();

	// vector to location & stop idling
	m_pc = 0x04 + indx * 4;
	m_idle = 0;

	// mask other interrupts based on the nesting bit
	if (m_icntl & 0x10) m_imask &= ~(0x3ff >> indx);
	else m_imask &= ~0x3ff;

	m_irq_depth++;

	return true;
}



void adsp21xx_device::check_irqs()
{
	uint8_t check;

	// check IRQ2
	check = (m_icntl & 4) ? m_irq_latch[ADSP2181_IRQ2] : m_irq_state[ADSP2181_IRQ2];
	if (check && generate_irq(ADSP2181_IRQ2, 0))
		return;

	// check IRQL1
	check = m_irq_state[ADSP2181_IRQL1];
	if (check && generate_irq(ADSP2181_IRQL1, 1))
		return;

	// check IRQL0
	check = m_irq_state[ADSP2181_IRQL0];
	if (check && generate_irq(ADSP2181_IRQL0, 2))
		return;

	// check SPORT0 transmit
	check = m_irq_latch[ADSP2181_SPORT0_TX];
	if (check && generate_irq(ADSP2181_SPORT0_TX, 3))
		return;

	// check SPORT0 receive
	check = m_irq_latch[ADSP2181_SPORT0_RX];
	if (check && generate_irq(ADSP2181_SPORT0_RX, 4))
		return;

	// check IRQE
	check = m_irq_latch[ADSP2181_IRQE];
	if (check && generate_irq(ADSP2181_IRQE, 5))
		return;

	// check BDMA interrupt
	check = m_irq_latch[ADSP2181_BDMA];
	if (check && generate_irq(ADSP2181_BDMA, 6))
		return;

	// check IRQ1/SPORT1 transmit
	check = (m_icntl & 2) ? m_irq_latch[ADSP2181_IRQ1] : m_irq_state[ADSP2181_IRQ1];
	if (check && generate_irq(ADSP2181_IRQ1, 7))
		return;

	// check IRQ0/SPORT1 receive
	check = (m_icntl & 1) ? m_irq_latch[ADSP2181_IRQ0] : m_irq_state[ADSP2181_IRQ0];
	if (check && generate_irq(ADSP2181_IRQ0, 8))
		return;

	// check timer
	check = m_irq_latch[ADSP2181_TIMER];
	if (check && generate_irq(ADSP2181_TIMER, 9))
		return;

	/* Nothing could be taken.  Do NOT rescan the whole table here: the board
	   keeps IRQL0 asserted for as long as a host word is latched, so a masked
	   request used to leave m_irq_pending set for ever and every instruction
	   paid for a full check_irqs() (measured: 97.4M calls for 126M DSP
	   instructions, ~40% of the board's host CPU).  Hardware samples the
	   request lines at instruction boundaries, but a request can only become
	   takeable when something changes, and every such change re-arms the flag:
	   set_irq_line() (new/cleared request), an IMASK write, an ICNTL write and
	   the RTI status-stack restore all call check_irqs() or set the flag. */
	m_irq_pending = 0;
}



/***************************************************************************
    INITIALIZATION AND SHUTDOWN
***************************************************************************/


#ifdef RAX_PROF
/* Poor man's profiler for the interpreter.  The AArch64 generic timer read
   costs ~1-2 cycles and ticks at 24 MHz, which is coarse (an instruction is
   ~1 us) but fine for relative attribution over millions of instructions.
   RAX_PROF=1 enables it; a window summary goes to stderr every 8M
   instructions.  Buckets are the switch's top byte, i.e. the instruction
   family. */
static int rax_prof_on = -1;
static uint64_t rax_prof_tab[256];
static uint64_t rax_prof_cnt[256];
static uint64_t rax_prof_n;

static inline uint64_t rax_prof_clock(void)
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

static void rax_prof_report(void)
{
   unsigned i, k;
   uint64_t total = 0;

   for (i = 0; i < 256; i++)
      total += rax_prof_tab[i];
   if (total != 0)
      fprintf(stderr, "PROF n=%llu ticks=%llu\n",
              (unsigned long long)rax_prof_n, (unsigned long long)total);
   for (k = 0; k < 10 && total != 0; k++) {
      unsigned best = 0;
      uint64_t bv = 0;
      for (i = 0; i < 256; i++)
         if (rax_prof_tab[i] > bv) { bv = rax_prof_tab[i]; best = i; }
      if (bv == 0)
         break;
      fprintf(stderr, "PROF %02X %5.1f%% n=%llu ticks/instr=%.1f\n", best,
              100.0 * (double)bv / (double)total,
              (unsigned long long)rax_prof_cnt[best],
              rax_prof_cnt[best] ? (double)bv / (double)rax_prof_cnt[best] : 0.0);
      rax_prof_tab[best] = 0;
      rax_prof_cnt[best] = 0;
   }
}
#endif

void adsp21xx_device::execute_run()
{
#ifdef RAX_PROF
   if (rax_prof_on < 0) {
      const char *e = getenv("RAX_PROF");
      rax_prof_on = (e && *e && *e != '0') ? 1 : 0;
   }
#endif
   check_irqs();

	do
	{
		// debugging
		m_ppc = m_pc;   // copy PC to previous PC
#ifdef RAX_PROF
		uint64_t prof_t0 = rax_prof_on ? rax_prof_clock() : 0;
#endif
#ifdef RAX_TRACE_MIX
		{
			static int pt = -1;
			if (pt < 0) { const char *e = getenv("RAX_PCTRACE"); pt = (e && *e != '\0') ? 1 : 0; }
			if (pt) {
				const double t = (double)RaxTestGrant() / 16670000.0;
				if (t >= 20.1640 && t <= 20.1800) fprintf(stderr, "P %04X\n", (unsigned)m_pc);
			}
		}
		/* The staging-fill gate: 0x35C5 is "IF LT" on DM(I1,M2). */
		{
			/* Entry counters for the DAC/mixer chain, same names as MAME's. */
			static unsigned long n35aa, n3c35, n00ae, n3627, n0062;
			static int r8;
			if (m_pc == 0x35AA) n35aa++;
			if (m_pc == 0x3C35) n3c35++;
			if (m_pc == 0x00A8) n00ae++;     /* ring copy routine entry */
			if (m_pc == 0x3627) n3627++;     /* staging fill */
			if (m_pc == 0x0062) n0062++;
			{
				static int shown_2b; 
				if ((m_pc == 0x2B58 || m_pc == 0x2B59) && shown_2b < 4) {
					fprintf(stderr, "M2AT pc=%04X M2=%d I2=%04X L2=%u\n", (unsigned)m_pc,
						(int)m_m[2], (unsigned)(m_i[2] & 0x3fff), (unsigned)m_l[2]);
					shown_2b++;
				}
			}
			{
				static int shown_m2;
				if (m_pc == 0x36C1 && shown_m2 < 2) {
					fprintf(stderr, "M2CHK M2=%d(0x%04X) M3=%d I1=%04X\n",
						(int)m_m[2], (unsigned)(m_m[2] & 0xffff), (int)m_m[3],
						(unsigned)(m_i[1] & 0x3fff));
					shown_m2++;
				}
			}
			{
				static unsigned long a2b57, a36bb, a3679;
				static int r9;
				if (m_pc == 0x2B57) a2b57++;
				if (m_pc == 0x36BB) a36bb++;
				if (m_pc == 0x3679) a3679++;
				if (!r9) { r9 = 1; atexit([](){
					fprintf(stderr, "TBL init3B00=%lu init3B50=%lu i3679=%lu\n", a2b57, a36bb, a3679); }); }
			}
			if (!r8) { r8 = 1; atexit([](){
				fprintf(stderr, "CHAIN aa=%lu c35=%lu copy=%lu fill=%lu mix=%lu\n",
					n35aa, n3c35, n00ae, n3627, n0062); }); }
		}
		if (m_pc == 0x35AA) {   /* entry of the per-channel routine */
			static unsigned long c2[32]; static int r7;
			c2[(m_core.sr.sr) & 0x1f]++;
			if (!r7) { r7 = 1; atexit([](){
				fprintf(stderr, "ENTRY_SR0");
				for (int i = 0; i < 32; i++) if (c2[i]) fprintf(stderr, " %d:%lu", i, c2[i]);
				fprintf(stderr, "\n"); }); }
		}
		if (m_pc == 0x007F) {   /* CALL $35AA with SR0 = DM(3BDB) */
			static unsigned long cnt[16];
			static int reg6;
			unsigned v = (uint32_t)data_read(0x3BDB) & 0xf;
			cnt[v]++;
			if (!reg6) { reg6 = 1; atexit([](){
				fprintf(stderr, "CNTHIST");
				for (int i = 0; i < 16; i++) fprintf(stderr, " %d:%lu", i, cnt[i]);
				fprintf(stderr, "\n"); }); }
		}
		if (m_pc == 0x35CE) {   /* CALL $3627: the fill; I4 = 0x3490 + n*256 */
			static unsigned long hist[64];
			static int reg4;
			unsigned i4 = (unsigned)m_i[4] & 0x3fff;
			int n = (int)i4 - 0x3490;
			if (n >= 0) { n >>= 8; if (n >= 0 && n < 64) hist[n]++; }
			if (!reg4) { reg4 = 1; atexit([](){
				fprintf(stderr, "FILLPAGE");
				for (int i = 0; i < 64; i++) if (hist[i]) fprintf(stderr, " %d:%lu", i, hist[i]);
				fprintf(stderr, "\n"); }); }
		}
		if (m_pc == 0x35C5) {
			static unsigned long neg, pos;
			static int reg;
			{
				static int shown3;
				if (shown3 < 8) {
					fprintf(stderr, "GATEV n=%d sr0=%04X ar=%d\n", shown3,
						(unsigned)m_core.sr.sr, (int)m_core.ar.s);
					shown3++;
				}
			}
			if (m_core.ar.s < 0) neg++; else pos++;
			if (!reg) { reg = 1; atexit([](){
				fprintf(stderr, "GATE neg=%lu pos=%lu\n", neg, pos); }); }
		}
#endif
#ifdef RAX_TRACE_IMASK
		/* Cycle-stamped marker for the DSP's command-wait window: the
		   0x2810-0x2848 routine kicks a BDMA, enables its interrupt and then
		   spins; the 0x2839-0x283B tail must not be interrupted. */
		if (m_ppc >= 0x2829 && m_ppc <= 0x2840 &&
		    (m_ppc == 0x2829 || m_ppc == 0x282D || m_ppc == 0x2834 ||
		     m_ppc == 0x2839 || m_ppc == 0x283B))
			adsp_evt_note(7, (unsigned)m_ppc, (unsigned)m_imask, 0);
#endif
		if (m_irq_pending)
			check_irqs();
		// instruction fetch
		uint32_t op = opcode_read();
#ifdef RAX_TEST_TRACE
		if (m_trace_cb)
			m_trace_cb(m_trace_ctx, m_ppc, op);
#endif

		// advance to the next instruction
		if (m_pc != m_loop)
			m_pc++;

		// handle looping
		else
		{
			// condition not met, keep looping
			if (condition(m_loop_condition))
				m_pc = pc_stack_top();

			// condition met; pop the PC and loop stacks and fall through
			else
			{
				loop_stack_pop();
				pc_stack_pop_val();
				m_pc++;
			}
		}

		// parse the instruction
		uint32_t temp;
		{
			unsigned form8 = BIT(op, 16, 8);
			/* 0x60-0x7f is one uniform family: ALU/MAC plus one data-memory
			   access (DAG1/DAG2, read/write) and a register operand from
			   bits 4-7.  Running it through one handler instead of 32 case
			   bodies keeps the hot text small; the bodies were byte for byte
			   the same calls in a different order. */
			if (form8 >= 0x60 && form8 <= 0x7f) {
				exec_mem_op(op, form8 - 0x60);
				goto inst_done;
			}
		}
		switch (BIT(op, 16, 8))
		{
			case 0x00:
				// 00000000 00000000 00000000  NOP
				break;
			case 0x01:
				// 00000001 0xxxxxxx xxxxxxxx  dst = IO(x)
				// 00000001 1xxxxxxx xxxxxxxx  IO(x) = dst
				// ADSP-218x only
				if (m_chip_type >= CHIP_TYPE_ADSP2181)
				{
					if (!BIT(op, 15))
						write_reg0(BIT(op, 0, 4), io_read(BIT(op, 4, 11)));
					else
						io_write(BIT(op, 4, 11), read_reg0(BIT(op, 0, 4)));
				}
				break;
			case 0x02:
				// 00000010 0000xxxx xxxxxxxx  modify flag out
				// 00000010 10000000 00000000  idle
				// 00000010 10000000 0000xxxx  idle (n)
				if (BIT(op, 15))
				{
					m_idle = 1;
					m_icount = 0;
				}
				else
				{
					if (condition(BIT(op, 0, 4)))
					{
						if (BIT(op, 5)) m_flagout = 0;
						if (BIT(op, 4)) m_flagout ^= 1;
						if (m_chip_type >= CHIP_TYPE_ADSP2101)
						{
							if (BIT(op, 7)) m_fl0 = 0;
							if (BIT(op, 6)) m_fl0 ^= 1;
							if (BIT(op, 9)) m_fl1 = 0;
							if (BIT(op, 8)) m_fl1 ^= 1;
							if (BIT(op, 11)) m_fl2 = 0;
							if (BIT(op, 10)) m_fl2 ^= 1;
						}
					}
				}
				break;
			case 0x03:
				// 00000011 xxxxxxxx xxxxxxxx  call or jump on flag in
				if (BIT(op, 1) ? m_flagin : !m_flagin)
				{
					if (BIT(op, 0))
						pc_stack_push();
					m_pc = BIT(op, 4, 12) | BIT(op, 2, 2) << 12;
				}
				break;
			case 0x04:
				// 00000100 00000000 000xxxxx  stack control
				if (BIT(op, 4)) pc_stack_pop_val();
				if (BIT(op, 3)) loop_stack_pop();
				if (BIT(op, 2)) cntr_stack_pop();
				if (BIT(op, 1))
				{
					if (BIT(op, 0)) stat_stack_pop();
					else stat_stack_push();
				}
				break;
			case 0x05:
				// 00000101 00000000 00000000  saturate MR
				if (GET_MV)
				{
					if (m_core.mr.mrx.mr2.u & 0x80)
						m_core.mr.mrx.mr2.u = 0xffff, m_core.mr.mrx.mr1.u = 0x8000, m_core.mr.mrx.mr0.u = 0x0000;
					else
						m_core.mr.mrx.mr2.u = 0x0000, m_core.mr.mrx.mr1.u = 0x7fff, m_core.mr.mrx.mr0.u = 0xffff;
				}
				break;
			case 0x06:
				// 00000110 000xxxxx 00000000  DIVS
				{
					int xop = BIT(op, 8, 3);
					int yop = BIT(op, 11, 2);

					xop = ALU_GETXREG_UNSIGNED(xop);
					yop = ALU_GETYREG_UNSIGNED(yop);

					temp = xop ^ yop;
					m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
					m_core.af.u = (yop << 1) | (m_core.ay0.u >> 15);
					m_core.ay0.u = (m_core.ay0.u << 1) | (temp >> 15);
				}
				break;
			case 0x07:
				// 00000111 00010xxx 00000000  DIVQ
				{
					int xop = BIT(op, 8, 3);
					int res;

					xop = ALU_GETXREG_UNSIGNED(xop);

					if (GET_Q)
						res = m_core.af.u + xop;
					else
						res = m_core.af.u - xop;

					temp = res ^ xop;
					m_astat = (m_astat & ~QFLAG) | ((temp >> 10) & QFLAG);
					m_core.af.u = (res << 1) | (m_core.ay0.u >> 15);
					m_core.ay0.u = (m_core.ay0.u << 1) | ((~temp >> 15) & 0x0001);
				}
				break;
			case 0x08:
				// 00001000 00000000 0000xxxx  reserved
				break;
			case 0x09:
				// 00001001 00000000 000xxxxx  modify address register
				temp = BIT(op, 2, 3);
				modify_address(temp, (temp & 4) | (op & 3));
				break;
			case 0x0a:
				// 00001010 00000000 000xxxxx  conditional return
				if (condition(BIT(op, 0, 4)))
				{
					pc_stack_pop();

					// RTI case
					if (BIT(op, 4))
					{
						stat_stack_pop();
						if (m_irq_depth > 0)
							m_irq_depth--;
					}
				}
				break;
			case 0x0b:
				// 00001011 00000000 xxxxxxxx  conditional jump (indirect address)
				if (condition(BIT(op, 0, 4)))
				{
					if (BIT(op, 4))
						pc_stack_push();
					m_pc = m_i[4 + BIT(op, 6, 2)] & 0x3fff;
				}
				break;
			case 0x0c:
				// 00001100 xxxxxxxx xxxxxxxx  mode control
				if (m_chip_type >= CHIP_TYPE_ADSP2101)
				{
					if (BIT(op, 3)) m_mstat = BIT(op, 2) ? (m_mstat | MSTAT_GOMODE) : (m_mstat & ~MSTAT_GOMODE);
					if (BIT(op, 13)) m_mstat = BIT(op, 12) ? (m_mstat | MSTAT_INTEGER) : (m_mstat & ~MSTAT_INTEGER);
					if (BIT(op, 15)) m_mstat = BIT(op, 14) ? (m_mstat | MSTAT_TIMER) : (m_mstat & ~MSTAT_TIMER);
				}
				if (BIT(op, 5)) m_mstat = BIT(op, 4) ? (m_mstat | MSTAT_BANK) : (m_mstat & ~MSTAT_BANK);
				if (BIT(op, 7)) m_mstat = BIT(op, 6) ? (m_mstat | MSTAT_REVERSE) : (m_mstat & ~MSTAT_REVERSE);
				if (BIT(op, 9)) m_mstat = BIT(op, 8) ? (m_mstat | MSTAT_STICKYV) : (m_mstat & ~MSTAT_STICKYV);
				if (BIT(op, 11)) m_mstat = BIT(op, 10) ? (m_mstat | MSTAT_SATURATE) : (m_mstat & ~MSTAT_SATURATE);
				update_mstat();
				break;
			case 0x0d:
				// 00001101 0000xxxx xxxxxxxx  internal data move
				switch (BIT(op, 8, 4))
				{
					case 0x00:  write_reg0(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
					case 0x01:  write_reg0(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
					case 0x02:  write_reg0(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
					case 0x03:  write_reg0(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
					case 0x04:  write_reg1(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
					case 0x05:  write_reg1(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
					case 0x06:  write_reg1(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
					case 0x07:  write_reg1(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
					case 0x08:  write_reg2(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
					case 0x09:  write_reg2(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
					case 0x0a:  write_reg2(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
					case 0x0b:  write_reg2(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
					case 0x0c:  write_reg3(BIT(op, 4, 4), read_reg0(BIT(op, 0, 4))); break;
					case 0x0d:  write_reg3(BIT(op, 4, 4), read_reg1(BIT(op, 0, 4))); break;
					case 0x0e:  write_reg3(BIT(op, 4, 4), read_reg2(BIT(op, 0, 4))); break;
					case 0x0f:  write_reg3(BIT(op, 4, 4), read_reg3(BIT(op, 0, 4))); break;
				}
				break;
			case 0x0e:
				// 00001110 0xxxxxxx xxxxxxxx  conditional shift
				if (condition(BIT(op, 0, 4))) shift_op(op);
				break;
			case 0x0f:
				// 00001111 0xxxxxxx xxxxxxxx  shift immediate
				shift_op_imm(op);
				break;
			case 0x10:
				// 00010000 0xxxxxxx xxxxxxxx  shift with internal data register move
				shift_op(op);
				temp = read_reg0(BIT(op, 0, 4));
				write_reg0(BIT(op, 4, 4), temp);
				break;
			case 0x11:
				// 00010001 xxxxxxxx xxxxxxxx  shift with pgm memory read/write
				if (BIT(op, 15))
				{
					pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
					shift_op(op);
				}
				else
				{
					shift_op(op);
					write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
				}
				break;
			case 0x12:
				// 00010010 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG1
				if (BIT(op, 15))
				{
					data_write_dag1(op, read_reg0(BIT(op, 4, 4)));
					shift_op(op);
				}
				else
				{
					shift_op(op);
					write_reg0(BIT(op, 4, 4), data_read_dag1(op));
				}
				break;
			case 0x13:
				// 00010011 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG2
				if (BIT(op, 15))
				{
					data_write_dag2(op, read_reg0(BIT(op, 4, 4)));
					shift_op(op);
				}
				else
				{
					shift_op(op);
					write_reg0(BIT(op, 4, 4), data_read_dag2(op));
				}
				break;
			case 0x14: case 0x15: case 0x16: case 0x17:
				// 000101xx xxxxxxxx xxxxxxxx  do until
				loop_stack_push(op & 0x3ffff);
				pc_stack_push();
				break;
			case 0x18: case 0x19: case 0x1a: case 0x1b:
				// 000110xx xxxxxxxx xxxxxxxx  conditional jump (immediate addr)
				if (condition(BIT(op, 0, 4)))
				{
					m_pc = BIT(op, 4, 14);
					// check for a busy loop
					if (m_pc == m_ppc)
						m_icount = 0;
				}
				break;
			case 0x1c: case 0x1d: case 0x1e: case 0x1f:
				// 000111xx xxxxxxxx xxxxxxxx  conditional call (immediate addr)
				if (condition(BIT(op, 0, 4)))
				{
					pc_stack_push();
					m_pc = BIT(op, 4, 14);
				}
				break;
			case 0x20: case 0x21:
				// 0010000x xxxxxxxx xxxxxxxx  conditional MAC to MR
				if (condition(BIT(op, 0, 4)))
				{
					if (m_chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x0018f0) == 0x000010)
						mac_op_mr_xop(op);
					else
						mac_op_mr(op);
				}
				break;
			case 0x22: case 0x23:
				// 0010001x xxxxxxxx xxxxxxxx  conditional ALU to AR
				if (condition(BIT(op, 0, 4)))
				{
					if (m_chip_type >= CHIP_TYPE_ADSP2181 && BIT(op, 4))
						alu_op_ar_const(op);
					else
						alu_op_ar(op);
				}
				break;
			case 0x24: case 0x25:
				// 0010010x xxxxxxxx xxxxxxxx  conditional MAC to MF
				if (condition(BIT(op, 0, 4)))
				{
					if (m_chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x0018f0) == 0x000010)
						mac_op_mf_xop(op);
					else
						mac_op_mf(op);
				}
				break;
			case 0x26: case 0x27:
				// 0010011x xxxxxxxx xxxxxxxx  conditional ALU to AF
				if (condition(BIT(op, 0, 4)))
				{
					if (m_chip_type >= CHIP_TYPE_ADSP2181 && BIT(op, 4))
						alu_op_af_const(op);
					else
						alu_op_af(op);
				}
				break;
			case 0x28: case 0x29:
				// 0010100x xxxxxxxx xxxxxxxx  MAC to MR with internal data register move
				temp = read_reg0(BIT(op, 0, 4));
				mac_op_mr(op);
				write_reg0(BIT(op, 4, 4), temp);
				break;
			case 0x2a: case 0x2b:
				// 0010101x xxxxxxxx xxxxxxxx  ALU to AR with internal data register move
				if (m_chip_type >= CHIP_TYPE_ADSP2181 && BIT(op, 0, 8) == 0xaa)
					alu_op_none(op);
				else
				{
					temp = read_reg0(BIT(op, 0, 4));
					alu_op_ar(op);
					write_reg0(BIT(op, 4, 4), temp);
				}
				break;
			case 0x2c: case 0x2d:
				// 0010110x xxxxxxxx xxxxxxxx  MAC to MF with internal data register move
				temp = read_reg0(BIT(op, 0, 4));
				mac_op_mf(op);
				write_reg0(BIT(op, 4, 4), temp);
				break;
			case 0x2e: case 0x2f:
				// 0010111x xxxxxxxx xxxxxxxx  ALU to AF with internal data register move
				temp = read_reg0(BIT(op, 0, 4));
				alu_op_af(op);
				write_reg0(BIT(op, 4, 4), temp);
				break;
			case 0x30: case 0x31: case 0x32: case 0x33:
				// 001100xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 0)
				write_reg0(BIT(op, 0, 4), util::sext(op >> 4, 14));
				break;
			case 0x34: case 0x35: case 0x36: case 0x37:
				// 001101xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 1)
				write_reg1(BIT(op, 0, 4), util::sext(op >> 4, 14));
				break;
			case 0x38: case 0x39: case 0x3a: case 0x3b:
				// 001110xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 2)
				write_reg2(BIT(op, 0, 4), util::sext(op >> 4, 14));
				break;
			case 0x3c: case 0x3d: case 0x3e: case 0x3f:
				// 001111xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 3)
				write_reg3(BIT(op, 0, 4), util::sext(op >> 4, 14));
				break;
			case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
			case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
				// 0100xxxx xxxxxxxx xxxxxxxx  load data register immediate
				write_reg0(BIT(op, 0, 4), BIT(op, 4, 16));
				break;
			case 0x50: case 0x51:
				// 0101000x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory read
				mac_op_mr(op);
				write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
				break;
			case 0x52: case 0x53:
				// 0101001x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory read
				alu_op_ar(op);
				write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
				break;
			case 0x54: case 0x55:
				// 0101010x xxxxxxxx xxxxxxxx  MAC to MF with pgm memory read
				mac_op_mf(op);
				write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
				break;
			case 0x56: case 0x57:
				// 0101011x xxxxxxxx xxxxxxxx  ALU to AF with pgm memory read
				alu_op_af(op);
				write_reg0(BIT(op, 4, 4), pgm_read_dag2(op));
				break;
			case 0x58: case 0x59:
				// 0101100x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory write
				pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				mac_op_mr(op);
				break;
			case 0x5a: case 0x5b:
				// 0101101x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory write
				pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				alu_op_ar(op);
				break;
			case 0x5c: case 0x5d:
				// 0101110x xxxxxxxx xxxxxxxx  ALU to MR with pgm memory write
				pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				mac_op_mf(op);
				break;
			case 0x5e: case 0x5f:
				// 0101111x xxxxxxxx xxxxxxxx  ALU to MF with pgm memory write
				pgm_write_dag2(op, read_reg0(BIT(op, 4, 4)));
				alu_op_af(op);
				break;
			case 0x80: case 0x81: case 0x82: case 0x83:
				// 100000xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 0
				write_reg0(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
				break;
			case 0x84: case 0x85: case 0x86: case 0x87:
				// 100001xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 1
				write_reg1(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
				break;
			case 0x88: case 0x89: case 0x8a: case 0x8b:
				// 100010xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 2
				write_reg2(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
				break;
			case 0x8c: case 0x8d: case 0x8e: case 0x8f:
				// 100011xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 3
				write_reg3(BIT(op, 0, 4), data_read(BIT(op, 4, 14)));
				break;
			case 0x90: case 0x91: case 0x92: case 0x93:
				// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 0
				data_write(BIT(op, 4, 14), read_reg0(BIT(op, 0, 4)));
				break;
			case 0x94: case 0x95: case 0x96: case 0x97:
				// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 1
				data_write(BIT(op, 4, 14), read_reg1(BIT(op, 0, 4)));
				break;
			case 0x98: case 0x99: case 0x9a: case 0x9b:
				// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 2
				data_write(BIT(op, 4, 14), read_reg2(BIT(op, 0, 4)));
				break;
			case 0x9c: case 0x9d: case 0x9e: case 0x9f:
				// 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 3
				data_write(BIT(op, 4, 14), read_reg3(BIT(op, 0, 4)));
				break;
			case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
			case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
				// 1010xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG1
				data_write_dag1(op, BIT(op, 4, 16));
				break;
			case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
				// 1011xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG2
				data_write_dag2(op, BIT(op, 4, 16));
				break;
			case 0xc0: case 0xc1:
				// 1100000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY0
				mac_op_mr(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xc2: case 0xc3:
				// 1100001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY0
				alu_op_ar(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xc4: case 0xc5:
				// 1100010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY0
				mac_op_mr(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xc6: case 0xc7:
				// 1100011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY0
				alu_op_ar(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xc8: case 0xc9:
				// 1100100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY0
				mac_op_mr(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xca: case 0xcb:
				// 1100101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY0
				alu_op_ar(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xcc: case 0xcd:
				// 1100110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY0
				mac_op_mr(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xce: case 0xcf:
				// 1100111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY0
				alu_op_ar(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.ay0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xd0: case 0xd1:
				// 1101000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY1
				mac_op_mr(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xd2: case 0xd3:
				// 1101001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY1
				alu_op_ar(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xd4: case 0xd5:
				// 1101010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY1
				mac_op_mr(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xd6: case 0xd7:
				// 1101011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY1
				alu_op_ar(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xd8: case 0xd9:
				// 1101100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY1
				mac_op_mr(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xda: case 0xdb:
				// 1101101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY1
				alu_op_ar(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xdc: case 0xdd:
				// 1101110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY1
				mac_op_mr(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xde: case 0xdf:
				// 1101111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY1
				alu_op_ar(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.ay1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xe0: case 0xe1:
				// 1110000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY0
				mac_op_mr(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xe2: case 0xe3:
				// 1110001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY0
				alu_op_ar(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xe4: case 0xe5:
				// 1110010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY0
				mac_op_mr(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xe6: case 0xe7:
				// 1110011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY0
				alu_op_ar(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xe8: case 0xe9:
				// 1110100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY0
				mac_op_mr(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xea: case 0xeb:
				// 1110101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY0
				alu_op_ar(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xec: case 0xed:
				// 1110110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY0
				mac_op_mr(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xee: case 0xef:
				// 1110111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY0
				alu_op_ar(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.my0.u = pgm_read_dag2(op >> 4);
				break;
			case 0xf0: case 0xf1:
				// 1111000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY1
				mac_op_mr(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xf2: case 0xf3:
				// 1111001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY1
				alu_op_ar(op);
				m_core.ax0.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xf4: case 0xf5:
				// 1111010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY1
				mac_op_mr(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xf6: case 0xf7:
				// 1111011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY1
				alu_op_ar(op);
				m_core.ax1.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xf8: case 0xf9:
				// 1111100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY1
				mac_op_mr(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xfa: case 0xfb:
				// 1111101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY1
				alu_op_ar(op);
				m_core.mx0.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xfc: case 0xfd:
				// 1111110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY1
				mac_op_mr(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
			case 0xfe: case 0xff:
				// 1111111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY1
				alu_op_ar(op);
				m_core.mx1.u = data_read_dag1(op);
				m_core.my1.u = pgm_read_dag2(op >> 4);
				break;
		}

#ifdef RAX_PROF
		if (rax_prof_on) {
			rax_prof_tab[BIT(op, 16, 8)] += rax_prof_clock() - prof_t0;
			rax_prof_cnt[BIT(op, 16, 8)]++;
			if (++rax_prof_n % 8000000u == 0)
				rax_prof_report();
		}
#endif
inst_done:
		m_icount--;
		if (m_board_stop >= 0 && m_icount <= m_board_stop)
			m_icount = 0;
	} while (m_icount > 0);
	m_board_stop = -1;
}

/* 011xxxxx: ALU/MAC with one data-memory access.
   f = form - 0x60: bit4 = DAG2, bit3 = write, bits 2-1 = operation
   (MAC->MR, ALU->AR, MAC->MF, ALU->AF).  The order of the calls is the one the
   32 original case bodies used: the ALU/MAC first on a read, last on a write. */
void adsp21xx_device::exec_mem_op(uint32_t op, unsigned f)
{
	uint32_t idx = BIT(op, 4, 4);

	if (f & 8) {                    /* write */
		uint32_t v = read_reg0(idx);
		if (f & 16) data_write_dag2(op, v);
		else        data_write_dag1(op, v);
		switch ((f >> 1) & 3) {
			case 0: mac_op_mr(op); break;
			case 1: alu_op_ar(op); break;
			case 2: mac_op_mf(op); break;
			default: alu_op_af(op); break;
		}
	} else {                        /* read */
		switch ((f >> 1) & 3) {
			case 0: mac_op_mr(op); break;
			case 1: alu_op_ar(op); break;
			case 2: mac_op_mf(op); break;
			default: alu_op_af(op); break;
		}
		write_reg0(idx, (f & 16) ? data_read_dag2(op) : data_read_dag1(op));
	}
}

/* -------------------------------------------------------------------------
 * Life cycle.  MAME's device_start() configures address spaces, allocates the
 * register maps and starts the timer; the memory maps belong to the board here
 * and there is no timer, so this only builds the tables and clears the state.
 * ---------------------------------------------------------------------- */
void adsp21xx_device::start(int chiptype)
{
	m_chip_type = (uint32_t)chiptype;
	memset(&m_core, 0, sizeof(m_core));
	memset(&m_alt, 0, sizeof(m_alt));
	memset(&m_i, 0, sizeof(m_i));
	memset(&m_m, 0, sizeof(m_m));
	memset(&m_l, 0, sizeof(m_l));
	memset(&m_lmask, 0, sizeof(m_lmask));
	memset(&m_base, 0, sizeof(m_base));
	memset(&m_loop_stack, 0, sizeof(m_loop_stack));
	memset(&m_cntr_stack, 0, sizeof(m_cntr_stack));
	memset(&m_pc_stack, 0, sizeof(m_pc_stack));
	memset(&m_stat_stack, 0, sizeof(m_stat_stack));
	memset(&m_irq_state, 0, sizeof(m_irq_state));
	memset(&m_irq_latch, 0, sizeof(m_irq_latch));
	m_halted = 0;
	m_irq_depth = 0;
	m_irq_pending = 0;
	m_trace_cb = NULL;
	m_trace_ctx = NULL;

	create_tables();

	/* MAME does this in its constructor; the opcode engine reaches
	 * registers through these tables, so they must be filled before any run. */
	// set up read register group 0 pointers
	m_read0_ptr[0x00] = &m_core.ax0.s;
	m_read0_ptr[0x01] = &m_core.ax1.s;
	m_read0_ptr[0x02] = &m_core.mx0.s;
	m_read0_ptr[0x03] = &m_core.mx1.s;
	m_read0_ptr[0x04] = &m_core.ay0.s;
	m_read0_ptr[0x05] = &m_core.ay1.s;
	m_read0_ptr[0x06] = &m_core.my0.s;
	m_read0_ptr[0x07] = &m_core.my1.s;
	m_read0_ptr[0x08] = &m_core.si.s;
	m_read0_ptr[0x09] = &m_core.se.s;
	m_read0_ptr[0x0a] = &m_core.ar.s;
	m_read0_ptr[0x0b] = &m_core.mr.mrx.mr0.s;
	m_read0_ptr[0x0c] = &m_core.mr.mrx.mr1.s;
	m_read0_ptr[0x0d] = &m_core.mr.mrx.mr2.s;
	m_read0_ptr[0x0e] = &m_core.sr.srx.sr0.s;
	m_read0_ptr[0x0f] = &m_core.sr.srx.sr1.s;

	// set up read register group 1 + 2 pointers
	for (int index = 0; index < 4; index++)
	{
		m_read1_ptr[0x00 + index] = &m_i[0 + index];
		m_read1_ptr[0x04 + index] = (uint32_t *)&m_m[0 + index];
		m_read1_ptr[0x08 + index] = &m_l[0 + index];
		m_read1_ptr[0x0c + index] = &m_l[0 + index];
		m_read2_ptr[0x00 + index] = &m_i[4 + index];
		m_read2_ptr[0x04 + index] = (uint32_t *)&m_m[4 + index];
		m_read2_ptr[0x08 + index] = &m_l[4 + index];
		m_read2_ptr[0x0c + index] = &m_l[4 + index];
	}

	// set up ALU register pointers
	m_alu_xregs[0] = &m_core.ax0;
	m_alu_xregs[1] = &m_core.ax1;
	m_alu_xregs[2] = &m_core.ar;
	m_alu_xregs[3] = &m_core.mr.mrx.mr0;
	m_alu_xregs[4] = &m_core.mr.mrx.mr1;
	m_alu_xregs[5] = &m_core.mr.mrx.mr2;
	m_alu_xregs[6] = &m_core.sr.srx.sr0;
	m_alu_xregs[7] = &m_core.sr.srx.sr1;
	m_alu_yregs[0] = &m_core.ay0;
	m_alu_yregs[1] = &m_core.ay1;
	m_alu_yregs[2] = &m_core.af;
	m_alu_yregs[3] = &m_core.zero;

	// set up MAC register pointers
	m_mac_xregs[0] = &m_core.mx0;
	m_mac_xregs[1] = &m_core.mx1;
	m_mac_xregs[2] = &m_core.ar;
	m_mac_xregs[3] = &m_core.mr.mrx.mr0;
	m_mac_xregs[4] = &m_core.mr.mrx.mr1;
	m_mac_xregs[5] = &m_core.mr.mrx.mr2;
	m_mac_xregs[6] = &m_core.sr.srx.sr0;
	m_mac_xregs[7] = &m_core.sr.srx.sr1;
	m_mac_yregs[0] = &m_core.my0;
	m_mac_yregs[1] = &m_core.my1;
	m_mac_yregs[2] = &m_core.mf;
	m_mac_yregs[3] = &m_core.zero;

	// set up shift register pointers
	m_shift_xregs[0] = &m_core.si;
	m_shift_xregs[1] = &m_core.si;
	m_shift_xregs[2] = &m_core.ar;
	m_shift_xregs[3] = &m_core.mr.mrx.mr0;
	m_shift_xregs[4] = &m_core.mr.mrx.mr1;
	m_shift_xregs[5] = &m_core.mr.mrx.mr2;
	m_shift_xregs[6] = &m_core.sr.srx.sr0;
	m_shift_xregs[7] = &m_core.sr.srx.sr1;

	reset();
}

void adsp21xx_device::reset()
{
	int i;

	// initialize the core registers
	m_core.ax0.u = m_core.ax1.u = 0;
	m_core.ay0.u = m_core.ay1.u = 0;
	m_core.ar.u  = m_core.af.u  = 0;
	m_core.mx0.u = m_core.mx1.u = 0;
	m_core.my0.u = m_core.my1.u = 0;
	m_core.mr.mr = m_core.mf.u = 0;
	m_core.si.u  = m_core.se.u  = m_core.sb.u = 0;
	m_core.sr.sr = 0;
	m_core.zero.u = 0;

	// initialize the alternate registers as well
	m_alt = m_core;

	// initialize the rest of the registers
	m_pc = 0;
	m_ppc = 0;
	m_loop = 0;
	m_loop_condition = 0;
	m_cntr = 0;
	m_astat = 0;
	m_sstat = 0;
	m_mstat = 0;
	m_mstat_prev = 0;
	m_astat_clear = 0;
	m_idle = 0;
	m_px = 0;
	m_pmovlay = 0;
	m_dmovlay = 0;
	m_flagout = 0;
	m_flagin = 0;
	m_fl0 = m_fl1 = m_fl2 = 0;
	m_idma_addr = 0;
	m_idma_cache = 0;
	m_idma_offs = 0;
	m_imask = 0;
	m_icntl = 0;
	m_ifc = 0;
	m_icount = 0;

	// initialize the DAG registers
	for (i = 0; i < 8; i++)
	{
		m_i[i] = 0;
		m_m[i] = 0;
		m_l[i] = 1;
		m_lmask[i] = 0;
		m_base[i] = 0;
	}

	for (i = 0; i < 10; i++)
	{
		m_irq_state[i] = 0;
		m_irq_latch[i] = 0;
	}

	m_irq_depth = 0;
	m_irq_pending = 0;

	// reset the stacks
	/* MAME's device_reset(): the stack pointers start at 0, not -1 (with -1 the
	 * first pop reads m_pc_stack[-1] and the DSP returns to address 0). */
	m_pc_sp = m_cntr_sp = m_stat_sp = m_loop_sp = 0;
	memset(m_loop_stack, 0, sizeof(m_loop_stack));
	memset(m_cntr_stack, 0, sizeof(m_cntr_stack));
	memset(m_pc_stack, 0, sizeof(m_pc_stack));
	memset(m_stat_stack, 0, sizeof(m_stat_stack));

	/* MAME's constructor: 0x3ff for the 2181 (16 IMASK bits exist but only
	 * ten interrupts), and 0x7f for MSTAT on every part from the 2101 up --
	 * 0x3f silently drops MSTAT bits 6/7 (timer enable / GoMode) and the
	 * engine's timer setup then never takes effect. */
	m_imask_mask = (m_chip_type >= CHIP_TYPE_ADSP2181) ? 0x3ff :
	               (m_chip_type >= CHIP_TYPE_ADSP2101) ? 0x3f : 0x0f;
	m_mstat_mask = (m_chip_type >= CHIP_TYPE_ADSP2101) ? 0x7f : 0x0f;
	m_mstat_prev = 0;
	m_idle = 0;
}

bool adsp21xx_device::chiptype_is_2181() const
{
	return m_chip_type == CHIP_TYPE_ADSP2181;
}

void adsp21xx_device::board_event_in(uint32_t cycles)
{
	int at = m_icount - (int)cycles;
	if (at < 0)
		at = 0;
	if (m_board_stop < 0 || at < m_board_stop)
		m_board_stop = at;
}

void adsp21xx_device::set_irq_line(int which, int state)
{
	/* MAME's execute_set_input(): a CLEAR->ASSERT edge on a latched line sets
	 * m_irq_latch, which check_irqs() consumes when it takes the vector. */
	if (state != 0 && m_irq_state[which] == 0)
		m_irq_latch[which] = 1;
	m_irq_state[which] = state;
	/* Hardware samples the interrupt requests on every instruction boundary;
	   MAME only does it once per execute_run() slice, which makes the DSP's
	   behaviour depend on how the caller chunked the run (measured: the same
	   command stream reached different states with a 37/64/192-cycle slice).
	   Polling here restores the hardware behaviour and makes the result
	   independent of the slice size. */
	m_irq_pending = 1;
}

/* -------------------------------------------------------------------------
 * Save/restore of the interpreter state.
 *
 * Everything the interpreter reads back after a savestate load is written
 * here.  The register-pointer maps (m_read0_ptr/m_alu_xregs/...) are built
 * once in start() and point at members of this object; they are the same in
 * any process after start() (same build/layout), so they are not saved.
 * ---------------------------------------------------------------------- */
#define YAB_ST_SAVE(field) do { \
   if (save) fwrite(&(field), sizeof(field), 1, fp); \
   else      fread(&(field), sizeof(field), 1, fp); \
} while (0)

void adsp21xx_device::save_state(FILE *fp) const
{
   adsp21xx_device *self = const_cast<adsp21xx_device *>(this);
   int save = 1;
   YAB_ST_SAVE(self->m_pc);
   YAB_ST_SAVE(self->m_ppc);
   YAB_ST_SAVE(self->m_loop);
   YAB_ST_SAVE(self->m_loop_condition);
   YAB_ST_SAVE(self->m_cntr);
   YAB_ST_SAVE(self->m_astat);
   YAB_ST_SAVE(self->m_sstat);
   YAB_ST_SAVE(self->m_mstat);
   YAB_ST_SAVE(self->m_mstat_prev);
   YAB_ST_SAVE(self->m_astat_clear);
   YAB_ST_SAVE(self->m_idle);
   YAB_ST_SAVE(self->m_core);
   YAB_ST_SAVE(self->m_alt);
   YAB_ST_SAVE(self->m_i);
   YAB_ST_SAVE(self->m_m);
   YAB_ST_SAVE(self->m_l);
   YAB_ST_SAVE(self->m_lmask);
   YAB_ST_SAVE(self->m_base);
   YAB_ST_SAVE(self->m_px);
   YAB_ST_SAVE(self->m_pmovlay);
   YAB_ST_SAVE(self->m_dmovlay);
   YAB_ST_SAVE(self->m_loop_stack);
   YAB_ST_SAVE(self->m_cntr_stack);
   YAB_ST_SAVE(self->m_pc_stack);
   YAB_ST_SAVE(self->m_stat_stack);
   YAB_ST_SAVE(self->m_pc_sp);
   YAB_ST_SAVE(self->m_cntr_sp);
   YAB_ST_SAVE(self->m_stat_sp);
   YAB_ST_SAVE(self->m_loop_sp);
   YAB_ST_SAVE(self->m_flagout);
   YAB_ST_SAVE(self->m_flagin);
   YAB_ST_SAVE(self->m_fl0);
   YAB_ST_SAVE(self->m_fl1);
   YAB_ST_SAVE(self->m_fl2);
   YAB_ST_SAVE(self->m_idma_addr);
   YAB_ST_SAVE(self->m_idma_cache);
   YAB_ST_SAVE(self->m_idma_offs);
   YAB_ST_SAVE(self->m_imask);
   YAB_ST_SAVE(self->m_icntl);
   YAB_ST_SAVE(self->m_ifc);
   YAB_ST_SAVE(self->m_irq_state);
   YAB_ST_SAVE(self->m_irq_latch);
   YAB_ST_SAVE(self->m_icount);
   YAB_ST_SAVE(self->m_mstat_mask);
   YAB_ST_SAVE(self->m_imask_mask);
   YAB_ST_SAVE(self->m_halted);
}

void adsp21xx_device::load_state(FILE *fp)
{
   adsp21xx_device *self = this;
   int save = 0;
   YAB_ST_SAVE(self->m_pc);
   YAB_ST_SAVE(self->m_ppc);
   YAB_ST_SAVE(self->m_loop);
   YAB_ST_SAVE(self->m_loop_condition);
   YAB_ST_SAVE(self->m_cntr);
   YAB_ST_SAVE(self->m_astat);
   YAB_ST_SAVE(self->m_sstat);
   YAB_ST_SAVE(self->m_mstat);
   YAB_ST_SAVE(self->m_mstat_prev);
   YAB_ST_SAVE(self->m_astat_clear);
   YAB_ST_SAVE(self->m_idle);
   YAB_ST_SAVE(self->m_core);
   YAB_ST_SAVE(self->m_alt);
   YAB_ST_SAVE(self->m_i);
   YAB_ST_SAVE(self->m_m);
   YAB_ST_SAVE(self->m_l);
   YAB_ST_SAVE(self->m_lmask);
   YAB_ST_SAVE(self->m_base);
   YAB_ST_SAVE(self->m_px);
   YAB_ST_SAVE(self->m_pmovlay);
   YAB_ST_SAVE(self->m_dmovlay);
   YAB_ST_SAVE(self->m_loop_stack);
   YAB_ST_SAVE(self->m_cntr_stack);
   YAB_ST_SAVE(self->m_pc_stack);
   YAB_ST_SAVE(self->m_stat_stack);
   YAB_ST_SAVE(self->m_pc_sp);
   YAB_ST_SAVE(self->m_cntr_sp);
   YAB_ST_SAVE(self->m_stat_sp);
   YAB_ST_SAVE(self->m_loop_sp);
   YAB_ST_SAVE(self->m_flagout);
   YAB_ST_SAVE(self->m_flagin);
   YAB_ST_SAVE(self->m_fl0);
   YAB_ST_SAVE(self->m_fl1);
   YAB_ST_SAVE(self->m_fl2);
   YAB_ST_SAVE(self->m_idma_addr);
   YAB_ST_SAVE(self->m_idma_cache);
   YAB_ST_SAVE(self->m_idma_offs);
   YAB_ST_SAVE(self->m_imask);
   YAB_ST_SAVE(self->m_icntl);
   YAB_ST_SAVE(self->m_ifc);
   YAB_ST_SAVE(self->m_irq_state);
   YAB_ST_SAVE(self->m_irq_latch);
   YAB_ST_SAVE(self->m_icount);
   YAB_ST_SAVE(self->m_mstat_mask);
   YAB_ST_SAVE(self->m_imask_mask);
   YAB_ST_SAVE(self->m_halted);
}

void adsp21xx_device::run(int cycles)
{
	m_icount = cycles;
	m_board_stop = -1;
	if (m_halted)
	{
		m_icount = 0;
		return;
	}
	execute_run();
}
