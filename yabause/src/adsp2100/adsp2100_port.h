/* ADSP-21xx (ADSP-2181) interpreter, ported from MAME's adsp2100.cpp.
 *
 * The opcode file 2100ops.hxx is MAME's, unmodified (BSD-3-Clause,
 * copyright-holders: Aaron Giles); it defines adsp21xx_device:: members, so the
 * class keeps MAME's name and member names.  What is different from MAME:
 *   - no device framework: start()/reset()/run() instead of device_start(),
 *     device_reset(), execute_run();
 *   - the program/data/IO spaces forward to function pointers owned by the
 *     caller (the RAX board), instead of MAME address spaces;
 *   - no savestates, no debugger, no disassembler, no timers (the board owns
 *     the timers, as MAME's rax.cpp does).
 */
#ifndef ADSP2100_PORT_H
#define ADSP2100_PORT_H

#include <stdio.h>
#include "mame_compat.h"

/* Device callbacks.  MAME's devcb_* become context+function pairs; the arity
 * of each call site is fixed by 2100ops.hxx / rax.cpp, so each one is its own
 * small functor:
 *   m_sport_tx_cb(which, data, mask)   -> devcb_write32 slot
 *   m_sport_rx_cb(which)               -> devcb_read32 slot
 *   m_timer_fired_cb(state)            -> devcb_write_line
 *   m_dmovlay_cb(bank)                 -> devcb_write32, single argument
 */
struct adsp_tx_cb
{
   void (*fn)(void *ctx, uint32_t which, uint32_t data, uint32_t mask);
   void *ctx;
   void operator()(uint32_t which, uint32_t data, uint32_t mask) const { if (fn) fn(ctx, which, data, mask); }
};
struct adsp_rx_cb
{
   uint32_t (*fn)(void *ctx, uint32_t which);
   void *ctx;
   uint32_t operator()(uint32_t which) const { return fn ? fn(ctx, which) : 0u; }
};
struct adsp_line_cb
{
   void (*fn)(void *ctx, int state);
   void *ctx;
   void operator()(int state) const { if (fn) fn(ctx, state); }
};
struct adsp_bank_cb
{
   void (*fn)(void *ctx, uint32_t bank);
   void *ctx;
   void operator()(uint32_t bank) const { if (fn) fn(ctx, bank); }
};

class adsp21xx_device
{
public:
   enum
   {
      CHIP_TYPE_ADSP2100,
      CHIP_TYPE_ADSP2101,
      CHIP_TYPE_ADSP2104,
      CHIP_TYPE_ADSP2105,
      CHIP_TYPE_ADSP2115,
      CHIP_TYPE_ADSP2181
   };

   /* --- our API (no MAME device framework) --- */
   void start(int chiptype);
   void reset();
   void run(int cycles);
   void set_input_halt(int state) { m_halted = state; }
   /* Save/restore the interpreter state (registers, stacks, IRQ latches,
    * timers).  The register-pointer maps (m_read0_ptr...) are set once in
    * start() and point into m_core; they are valid in any process after
    * start(), so they are deliberately NOT serialized. */
   void save_state(FILE *fp) const;
   void load_state(FILE *fp);
   void set_irq_line(int which, int state);
   /* The board's next event (BDMA completion, DAC half buffer) lands `cycles`
      DSP cycles from the instruction that armed it.  MAME's scheduler runs a
      device in quanta short enough that the event is serviced on its deadline;
      the RAX board asks the core for the same by ending run() at that point.
      Without it an event armed inside a run waits for the whole grant (the
      command stream then drains many times slower than MAME's). */
   void board_event_in(uint32_t cycles);
   uint32_t pc() const { return m_pc; }
   /* Number of interrupts taken and not yet returned from (RTI).  The board
      uses it to keep a level-triggered host IRQ from nesting inside the DSP's
      own handlers. */
   int irq_depth() const { return m_irq_depth; }
   /* Last write to IMASK (diagnostic): which instruction and what value. */
   unsigned imask_wr_pc() const { return m_imask_wr_pc; }
   unsigned imask_wr_val() const { return m_imask_wr_val; }
   unsigned stat_under_pc() const { return m_stat_under_pc; }
   unsigned stat_under_cnt() const { return m_stat_under; }
   /* Emergency re-enable used by the board when the DSP masks every interrupt
      and never restores the mask (see rax.cpp). */
   void force_irq_enable(unsigned bits) { m_imask |= (uint16_t)bits; }
   int icount_left() const { return m_icount; }
   /* optional instruction trace hook (used by the board / the host rig) */
   void (*m_trace_cb)(void *ctx, uint32_t pc, uint32_t op);
   void *m_trace_ctx;
   uint32_t dbg_imask() const { return m_imask; }
   uint32_t dbg_icntl() const { return m_icntl; }
   int dbg_idle() const { return m_idle; }
   int dbg_latch(int n) const { return m_irq_latch[n]; }
   int dbg_state(int n) const { return m_irq_state[n]; }
   uint32_t dbg_loop() const { return m_loop; }
   uint32_t dbg_loop_cond() const { return m_loop_condition; }
   int32_t dbg_cntr() const { return m_cntr; }
   uint32_t dbg_astat() const { return m_astat; }
   int dbg_pc_sp() const { return m_pc_sp; }

   /* Fast memory map.  The board hands its arrays over directly so the hot
      accessors (one data access + one opcode fetch per instruction) do not go
      through a function pointer; the callbacks above are then only used for
      the control-register / IO region, which is rare.  When these are NULL the
      generic callbacks are used (host harness / tracing). */
   uint32_t *fast_pram;        /* 0x4000 entries, 24-bit words              */
   uint16_t *fast_iram;        /* 0x1FE0 entries (data space 0x2000..0x3FDF) */
   uint16_t *fast_bank_cur;    /* the 0x2000-word bank currently selected    */

   /* spaces + callbacks, wired by the board before start() */
   adsp_mem_iface m_program;
   adsp_mem_iface m_data;
   adsp_mem_iface m_io;
   adsp_mem_iface m_cache;
   adsp_tx_cb   m_sport_tx_cb;
   adsp_rx_cb   m_sport_rx_cb;
   adsp_line_cb m_timer_fired_cb;
   adsp_bank_cb m_dmovlay_cb;

   /* helpers used by 2100ops.hxx */
   void create_tables();
   inline void update_mstat();
   inline uint32_t pc_stack_top();
   inline void set_pc_stack_top(uint32_t top);
   inline void pc_stack_push();
   inline void pc_stack_push_val(uint32_t val);
   inline void pc_stack_pop();
   inline uint32_t pc_stack_pop_val();
   inline uint32_t cntr_stack_top();
   inline void cntr_stack_push();
   inline void cntr_stack_pop();
   inline uint32_t loop_stack_top();
   inline void loop_stack_push(uint32_t value);
   inline void loop_stack_pop();
   inline void stat_stack_push();
   inline void stat_stack_pop();
   int slow_condition();
   inline void modify_address(uint32_t ireg, uint32_t mreg);
   inline void data_write_dag1(uint32_t op, int32_t val);
   inline uint32_t data_read_dag1(uint32_t op);
   inline void data_write_dag2(uint32_t op, int32_t val);
   inline uint32_t data_read_dag2(uint32_t op);
   inline void pgm_write_dag2(uint32_t op, int32_t val);
   inline uint32_t pgm_read_dag2(uint32_t op);
   void alu_op_ar(uint32_t op);
   void alu_op_ar_const(uint32_t op);
   void alu_op_af(uint32_t op);
   void alu_op_af_const(uint32_t op);
   void alu_op_none(uint32_t op);
   void mac_op_mr(uint32_t op);
   void mac_op_mr_xop(uint32_t op);
   void mac_op_mf(uint32_t op);
   void mac_op_mf_xop(uint32_t op);
   void shift_op(uint32_t op);
   void shift_op_imm(uint32_t op);

   inline uint16_t data_read(uint32_t addr);
   inline void data_write(uint32_t addr, uint16_t data);
   inline uint16_t io_read(uint32_t addr);
   inline void io_write(uint32_t addr, uint16_t data);
   inline uint32_t program_read(uint32_t addr);
   inline void program_write(uint32_t addr, uint32_t data);
   inline uint32_t opcode_read();

   inline void update_i(int which);
   inline void update_l(int which);
   inline void update_dmovlay();
   inline void write_reg0(int regnum, int32_t val);
   inline void write_reg1(int regnum, int32_t val);
   inline void write_reg2(int regnum, int32_t val);
   inline void write_reg3(int regnum, int32_t val);
   inline int32_t read_reg0(int regnum);
   inline int32_t read_reg1(int regnum);
   inline int32_t read_reg2(int regnum);
   inline int32_t read_reg3(int regnum);
   inline uint32_t get_ibase(int index) { return m_base[index]; }

   /* DAG register access for the board (MAME uses state_int(ADSP2100_I0+n)). */
   uint32_t dag_i(int n) const { return m_i[n & 7]; }
   void set_dag_i(int n, uint32_t v) { m_i[n & 7] = v; }
   int32_t dag_m(int n) const { return m_m[n & 7]; }
   uint32_t dag_l(int n) const { return m_l[n & 7]; }

   /* 'which' is the MAME IRQ index (0..9); indx selects the IMASK bit */
   bool generate_irq(int which, int indx = 0);
   bool chiptype_is_2181() const;
   void execute_run();
   void exec_mem_op(uint32_t op, unsigned f);
   void check_irqs();

	static const int PC_STACK_DEPTH     = 16;
	static const int CNTR_STACK_DEPTH   = 4;
	static const int STAT_STACK_DEPTH   = 4;
	static const int LOOP_STACK_DEPTH   = 4;

	// 16-bit registers that can be loaded signed or unsigned
	union adsp_reg16
	{
		uint16_t  u;
		int16_t   s;
	};

	// the SHIFT result register is 32 bits
	union adsp_shift
	{
#ifdef LSB_FIRST
		struct { adsp_reg16 sr0, sr1; } srx;
#else
		struct { adsp_reg16 sr1, sr0; } srx;
#endif
		uint32_t sr;
	};

	// the MAC result register is 40 bits
	union adsp_mac
	{
#ifdef LSB_FIRST
		struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx;
		struct { uint32_t mr0, mr1; } mry;
#else
		struct { adsp_reg16 mrzero, mr2, mr1, mr0; } mrx;
		struct { uint32_t mr1, mr0; } mry;
#endif
		uint64_t mr;
	};

	// core registers which are replicated
	struct adsp_core
	{
		// ALU registers
		adsp_reg16  ax0, ax1;
		adsp_reg16  ay0, ay1;
		adsp_reg16  ar;
		adsp_reg16  af;

		// MAC registers
		adsp_reg16  mx0, mx1;
		adsp_reg16  my0, my1;
		adsp_mac    mr;
		adsp_reg16  mf;

		// SHIFT registers
		adsp_reg16  si;
		adsp_reg16  se;
		adsp_reg16  sb;
		adsp_shift  sr;

		// dummy registers
		adsp_reg16  zero;
	};

	// configuration
	uint32_t                        m_chip_type;

	// other CPU registers
	uint32_t            m_pc;
	uint32_t            m_ppc;
	uint32_t            m_loop;
	uint32_t            m_loop_condition;
	uint32_t            m_cntr;

	// status registers
	uint32_t            m_astat;
	uint32_t            m_sstat;
	uint32_t            m_mstat;
	uint32_t            m_mstat_prev;
	uint32_t            m_astat_clear;
	uint32_t            m_idle;

	// live set of core registers
	adsp_core           m_core;

	// memory addressing registers
	uint32_t            m_i[8];
	int32_t             m_m[8];
	uint32_t            m_l[8];
	uint32_t            m_lmask[8];
	uint32_t            m_base[8];
	uint8_t             m_px;
	uint32_t            m_pmovlay; // External Program Space overlay
	uint32_t            m_dmovlay; // External Data Space overlay

	// stacks
	uint32_t            m_loop_stack[LOOP_STACK_DEPTH];
	uint32_t            m_cntr_stack[CNTR_STACK_DEPTH];
	uint32_t            m_pc_stack[PC_STACK_DEPTH];
	uint16_t            m_stat_stack[STAT_STACK_DEPTH][3];
	int32_t             m_pc_sp;
	int32_t             m_cntr_sp;
	int32_t             m_stat_sp;
	int32_t             m_loop_sp;

	// external I/O
	uint8_t             m_flagout;
	uint8_t             m_flagin;
	uint8_t             m_fl0;
	uint8_t             m_fl1;
	uint8_t             m_fl2;
	uint16_t            m_idma_addr;
	uint16_t            m_idma_cache;
	uint8_t             m_idma_offs;

	// interrupt handling
	uint16_t            m_imask;
	uint8_t             m_icntl;
	uint16_t            m_ifc;
	uint8_t             m_irq_state[10];
	uint8_t             m_irq_latch[10];

	// other internal states
	int                 m_icount;
	int                 m_mstat_mask;
	int                 m_imask_mask;

	// register maps
	int16_t *           m_read0_ptr[16];
	uint32_t *          m_read1_ptr[16];
	uint32_t *          m_read2_ptr[16];
	void *              m_alu_xregs[8];
	void *              m_alu_yregs[4];
	void *              m_mac_xregs[8];
	void *              m_mac_yregs[4];
	void *              m_shift_xregs[8];

	// alternate core registers (at end for performance)
	adsp_core           m_alt;

	// address spaces

	// tables
	uint8_t               m_condition_table[0x1000];
	uint16_t              m_mask_table[0x4000];
	uint16_t              m_reverse_table[0x4000];



	// flag definitions
	static const int SSFLAG     = 0x80;
	static const int MVFLAG     = 0x40;
	static const int QFLAG      = 0x20;
	static const int SFLAG      = 0x10;
	static const int CFLAG      = 0x08;
	static const int VFLAG      = 0x04;
	static const int NFLAG      = 0x02;
	static const int ZFLAG      = 0x01;

private:
   int m_halted;
   int m_irq_depth;
   int m_irq_pending;
   int m_board_stop;           /* m_icount at which the current run must end */
   unsigned m_imask_wr_pc;
   unsigned m_imask_wr_val;
   unsigned m_stat_under_pc;
   unsigned m_stat_under;
};

/* Interrupt-trace ring (RAX_TRACE_IMASK builds only): every IMASK write, every
   interrupt taken and every status-stack underflow, so a device run can show
   which instruction turned an interrupt off and never restored it. */
void adsp_evt_note(int kind, unsigned a, unsigned b, unsigned c);
void adsp_evt_dump(const char *path);
void adsp_evt_set_cycle(unsigned long long cycles);

#endif
