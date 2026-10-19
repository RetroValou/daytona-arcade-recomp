// license:BSD-3-Clause
// copyright-holders:Ville Linde
// The ADSP-21062 SHARC, lifted from MAME's interpreter (src/devices/cpu/sharc,
// commit dddd736806, BSD-3-Clause; THIRD_PARTY.md): the Model 2B copro's
// reference core, and the instruction semantics the recompiled SHARC program
// calls (tools/m2sharcrecomp). Changes from upstream sharc.h: not a MAME
// device (no address maps, DRC, debugger, save states or scheduler); the
// program and data spaces are the internal memory plus a Bus for the board's
// external map (MAME model2b copro_sharc_map); the flag outputs are dropped
// (nothing reads them on Model 2B); the delayed IOP write counts instructions
// instead of a scheduler timer. The declarations below are upstream's.
#pragma once

#include "runtime/board/fast_paths.h"
#include "runtime/sharc/sharc_profile.h"
#include "runtime/tgp/word_fifo.h"
#include "runtime/sharc/sharc_shim.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace rt::sharc {

// The board's side of the SHARC's data space above its internal memory
// (word addresses): Model 2B maps the copro FIFOs, buffer RAM and the data ROM.
struct Bus {
    virtual ~Bus() = default;
    virtual uint32_t read(uint32_t address) = 0;
    virtual void write(uint32_t address, uint32_t data) = 0;
};

class adsp21062_device
{
public:
	enum sharc_boot_mode
	{
		BOOT_MODE_EPROM,
		BOOT_MODE_HOST,
		BOOT_MODE_LINK,
		BOOT_MODE_NOBOOT
	};

    explicit adsp21062_device(Bus &bus);
    ~adsp21062_device();

    // MAME's device_reset (host boot), and the i960's side of the boot.
    void reset();
    void external_iop_write(offs_t address, uint32_t data);
    void external_dma_write(offs_t address, uint64_t data);
    // set_flag_input, applied at once (MAME defers it to the SHARC's next
    // timeslice when another CPU sets it: the FIFO flags then lag by up to a
    // slice; only the number of polls depends on it).
    void set_flag_input(int flag_num, int state);
    void set_halted(bool halted) { m_halted = halted; }
    bool halted() const { return m_halted; }

    // MAME's execute_run, one instruction at a time (the interpreter loop's body).
    void step();
    uint32_t pc() const;
    uint32_t daddr() const;
    uint64_t opcode() const;
    // The registers MAME's per-instruction log prints (M2TRACE_SHARCPC).
    uint32_t astat() const;
    uint32_t mode1() const;
    uint32_t r(int i) const;
    bool idle() const;
    uint64_t instructions() const { return m_instructions; }
    // The recompiled program (m2sharcrecomp's output, which compiles this core
    // into its own file): runs up to `budget` instructions; true when it stopped
    // because the SHARC waits (idle, halted, or polling its flags in a one-nop
    // do-until loop), false when the budget ran out.
    bool run_generated(uint64_t budget);
    // Called from the bus during run_generated: it returns (false, as on its
    // budget) at its next check: a plain run's end, a branch taken, a full step.
    void end_run() { m_budget = 0; }
#ifdef M2_FAST_SHARC
    // Model 2B's copro map (MAME model2b copro_sharc_map) for the recompiled
    // code's direct accesses: TgpBoard's FIFOs, buffer RAM, the copro data
    // ROM, its need_output flag. Unset (in null), the data space's bus serves
    // them (m2sharccheck's log).
    struct DirectBoard {
        WordFifo *in = nullptr, *out = nullptr;
        uint32_t *buffer = nullptr;            // 0x8000 words
        const uint32_t *rom = nullptr;         // rom_mask + 1 words
        uint32_t rom_mask = 0;
        const bool *stop_on_output = nullptr;  // end_run on an output word
    };
    void set_direct_board(const DirectBoard &b) { m_direct = b; }
#endif
    // M2_SHARC_PROFILE builds: executions per instruction of the program (the
    // recompiled code counts into it; nullptr: none).
    void set_profile(uint32_t *counts) { m_profile = counts; }
    // Called after each opcode fetch, before the instruction runs (m2sharccheck).
    void set_trace(void (*trace)(void *, const adsp21062_device &), void *ctx) { m_trace = trace, m_trace_ctx = ctx; }

    template <unsigned N> uint64_t pm_r(offs_t offset);
    template <unsigned N> void pm_w(offs_t offset, uint64_t data, uint64_t mem_mask = ~0);
    template <unsigned N> uint32_t dm_short_r(offs_t offset);
    template <unsigned N> uint32_t dm_short_se_r(offs_t offset);
    template <unsigned N> void dm_short_w(offs_t offset, uint32_t data);
    uint32_t iop_r(offs_t offset);
    void iop_w(offs_t offset, uint32_t data);

	enum ASTAT_SHIFT
	{
		AZ_SHIFT = 0,
		AV_SHIFT = 1,
		AN_SHIFT = 2,
		AC_SHIFT = 3,
		AS_SHIFT = 4,
		AI_SHIFT = 5,
		MN_SHIFT = 6,
		MV_SHIFT = 7,
		MU_SHIFT = 8,
		MI_SHIFT = 9,
		AF_SHIFT = 10,
		SV_SHIFT = 11,
		SZ_SHIFT = 12,
		SS_SHIFT = 13,
		BTF_SHIFT = 18,
		FLG0_SHIFT = 19,
		FLG1_SHIFT = 20,
		FLG2_SHIFT = 21,
		FLG3_SHIFT = 22
	};

	enum MODE1_DELAY_MODE
	{
		MODE1_WRITE_IMM,
		MODE1_WRITE_REG,
		MODE1_SET,
		MODE1_CLEAR,
		MODE1_TOGGLE,
	};

	enum
	{
		EXCEPTION_INTERRUPT = 0,
		EXCEPTION_COUNT
	};

private:
	// ASTAT flags
	static constexpr uint32_t AZ =              0x0000'0001;    // ALU result zero
	static constexpr uint32_t AV =              0x0000'0002;    // ALU overflow
	static constexpr uint32_t AN =              0x0000'0004;    // ALU result negative
	static constexpr uint32_t AC =              0x0000'0008;    // ALU fixed-point carry
	static constexpr uint32_t AS =              0x0000'0010;    // ALU X input sign
	static constexpr uint32_t AI =              0x0000'0020;    // ALU floating-point invalid operation
	static constexpr uint32_t MN =              0x0000'0040;    // Multiplier result negative
	static constexpr uint32_t MV =              0x0000'0080;    // Multiplier overflow
	static constexpr uint32_t MU =              0x0000'0100;    // Multiplier underflow
	static constexpr uint32_t MI =              0x0000'0200;    // Multiplier floating-point invalid operation
	static constexpr uint32_t AF =              0x0000'0400;    // Last ALU operation was a floating-point operation
	static constexpr uint32_t SV =              0x0000'0800;    // Shifter overflow
	static constexpr uint32_t SZ =              0x0000'1000;    // Shifter result zero
	static constexpr uint32_t SS =              0x0000'2000;    // Shifter input sign
	static constexpr uint32_t BTF =             0x0004'0000;    // Bit Test Flag
	static constexpr uint32_t FLG0 =            0x0008'0000;    // FLAG0
	static constexpr uint32_t FLG1 =            0x0010'0000;    // FLAG1
	static constexpr uint32_t FLG2 =            0x0020'0000;    // FLAG2
	static constexpr uint32_t FLG3 =            0x0040'0000;    // FLAG3

	// STKY flags
	static constexpr uint32_t AUS =             0x0000'0001;    // ALU floating-point underflow
	static constexpr uint32_t AVS =             0x0000'0002;    // ALU floating-point overflow
	static constexpr uint32_t AOS =             0x0000'0004;    // ALU fixed-point overflow
	static constexpr uint32_t AIS =             0x0000'0020;    // ALU floating-point invalid operation
	static constexpr uint32_t MOS =             0x0000'0040;    // Multiplier fixed-point overflow
	static constexpr uint32_t MVS =             0x0000'0080;    // Multiplier floating-point overflow
	static constexpr uint32_t MUS =             0x0000'0100;    // Multiplier underflow
	static constexpr uint32_t MIS =             0x0000'0200;    // Multiplier floating-point invalid operation
	static constexpr uint32_t PCFL =            0x0020'0000;    // PC stack full
	static constexpr uint32_t PCEM =            0x0040'0000;    // PC stack empty
	static constexpr uint32_t SSOV =            0x0080'0000;    // Status stack overflow
	static constexpr uint32_t SSEM =            0x0100'0000;    // Status stack empty
	static constexpr uint32_t LSOV =            0x0200'0000;    // Loop stacks overflow
	static constexpr uint32_t LSEM =            0x0400'0000;    // Loop stacks empty

	// MODE1 flags
	static constexpr uint32_t MODE1_BR8 =       0x0000'0001;    // Bit-reverse for I8
	static constexpr uint32_t MODE1_BR0 =       0x0000'0002;    // Bit-reverse for I0
	static constexpr uint32_t MODE1_SRCU =      0x0000'0004;    // Alternate register select for computational units
	static constexpr uint32_t MODE1_SRD1H =     0x0000'0008;    // DAG alternate register select (7-4)
	static constexpr uint32_t MODE1_SRD1L =     0x0000'0010;    // DAG alternate register select (3-0)
	static constexpr uint32_t MODE1_SRD2H =     0x0000'0020;    // DAG alternate register select (15-12)
	static constexpr uint32_t MODE1_SRD2L =     0x0000'0040;    // DAG alternate register select (11-8)
	static constexpr uint32_t MODE1_SRRFH =     0x0000'0080;    // Register file alternate select for R(15-8)
	static constexpr uint32_t MODE1_SRRFL =     0x0000'0400;    // Register file alternate select for R(7-0)
	static constexpr uint32_t MODE1_NESTM =     0x0000'0800;    // Interrupt nesting enable
	static constexpr uint32_t MODE1_IRPTEN =    0x0000'1000;    // Global interrupt enable
	static constexpr uint32_t MODE1_ALUSAT =    0x0000'2000;    // Enable ALU fixed-point saturation
	static constexpr uint32_t MODE1_SSE =       0x0000'4000;    // Enable short word sign extension
	static constexpr uint32_t MODE1_TRUNCATE =  0x0000'8000;    // (1) Floating-point truncation / (0) round to nearest
	static constexpr uint32_t MODE1_RND32 =     0x0001'0000;    // (1) 32-bit floating-point rounding / (0) 40-bit rounding
	static constexpr uint32_t MODE1_CSEL =      0x0006'0000;    // CSelect

	// MODE2 flags
	static constexpr uint32_t MODE2_IRQ0E =     0x0000'0001;    // IRQ0 (1) Edge sens. / (0) Level sens.
	static constexpr uint32_t MODE2_IRQ1E =     0x0000'0002;    // IRQ1 (1) Edge sens. / (0) Level sens.
	static constexpr uint32_t MODE2_IRQ2E =     0x0000'0004;    // IRQ2 (1) Edge sens. / (0) Level sens.
	static constexpr uint32_t MODE2_CADIS =     0x0000'0010;    // Cache disable
	static constexpr uint32_t MODE2_TIMEN =     0x0000'0020;    // Timer enable
	static constexpr uint32_t MODE2_BUSLK =     0x0000'0040;    // External bus lock
	static constexpr uint32_t MODE2_FLG0O =     0x0000'8000;    // FLAG0 (1) Output / (0) Input
	static constexpr uint32_t MODE2_FLG1O =     0x0001'0000;    // FLAG1 (1) Output / (0) Input
	static constexpr uint32_t MODE2_FLG2O =     0x0002'0000;    // FLAG2 (1) Output / (0) Input
	static constexpr uint32_t MODE2_FLG3O =     0x0004'0000;    // FLAG3 (1) Output / (0) Input
	static constexpr uint32_t MODE2_CAFRZ =     0x0008'0000;    // Cache freeze

	using opcode_func = void (adsp21062_device::*)();
	struct SHARC_OP
	{
		uint32_t op_mask;
		uint32_t op_bits;
		opcode_func handler;
	};


	union SHARC_REG
	{
		int32_t r;
		float f;
	};

	struct SHARC_DMA_REGS
	{
		uint32_t control;
		uint32_t int_index;
		uint32_t int_modifier;
		uint32_t int_count;
		uint32_t chain_ptr;
		uint32_t gen_purpose;
		uint32_t ext_index;
		uint32_t ext_modifier;
		uint32_t ext_count;
	};

	struct SHARC_DMA_OP
	{
		uint32_t src;
		uint32_t dst;
		uint32_t chain_ptr;
		int32_t src_modifier;
		int32_t dst_modifier;
		int32_t src_count;
		int32_t dst_count;
		int32_t pmode;
		int32_t chained_direction;
		emu_timer *timer;
		bool active;
		bool chained;
	};


    struct alignas(16) sharc_internal_state;

	static const SHARC_OP s_sharc_opcode_table[];
	static const size_t s_num_ops;

	static const uint32_t recips_mantissa_lookup[128];
	static const uint32_t rsqrts_mantissa_lookup[128];

    // The program and data spaces (MAME pgm_2m, data_2m, then the board's map).
    struct program_space {
        adsp21062_device &d;
        uint64_t read_qword(uint32_t address);
        void write_qword(uint32_t address, uint64_t data, uint64_t mem_mask = ~uint64_t(0));
    };
    struct data_space {
        adsp21062_device &d;
        uint32_t read_dword(uint32_t address);
        void write_dword(uint32_t address, uint32_t data);
    };
    // The data space's sign-extending short-word view (MODE1 SSE).
    struct short_view {
        bool *on;
        void select(int) { *on = true; }
        void disable() { *on = false; }
    };
    struct flag_out {
        void operator()(int) const {}
    };

    Bus &m_bus;
    sharc_internal_state *m_core;
    std::unique_ptr<sharc_internal_state> m_heap_core;
    sharc_boot_mode m_boot_mode = BOOT_MODE_HOST;
    std::unique_ptr<uint32_t[]> m_block_storage;
    uint32_t *m_blocks[2];
    bool m_short_se = false;
    short_view m_dm_short_view{&m_short_se};
    program_space m_program{*this};
    data_space m_data{*this};
    flag_out m_flag_out_cb[4];
    opcode_func m_sharc_op[512];
    static constexpr bool m_enable_drc = false;
    bool m_halted = true;
    void (*m_trace)(void *, const adsp21062_device &) = nullptr;
    void *m_trace_ctx = nullptr;
    int m_iop_delay = 0;
    uint64_t m_instructions = 0;
    bool m_gen_checked = false; // run_generated compared program memory with its program
    uint32_t m_gen_end = 0;     // the recompiled program's end (0x20000 up): writes there are fatal
    uint32_t *m_profile = nullptr;

    // The recompiled program's handlers (m2sharcrecomp writes their definitions:
    // upstream's sharcop_* and COMPUTE, the opcode a template parameter).
    template <uint32_t OPCODE> void COMPUTE_t();
    // Inline copies of GET_UREG, SET_UREG and SHIFT_OPERATION_IMM for the
    // recompiled handlers (m2sharcrecomp: their register numbers are constants).
    inline uint32_t GET_UREG_i(int ureg);
    inline void SET_UREG_i(int ureg, uint32_t data);
    inline void SHIFT_OPERATION_IMM_i(int shiftop, int data, int rn, int rx);
    inline uint32_t dm_read32_i(uint32_t address);
    inline void dm_write32_i(uint32_t address, uint32_t data);
    inline uint32_t *pm_data32(uint32_t address);
    inline uint32_t pm_read32_i(uint32_t address);
    inline void pm_write32_i(uint32_t address, uint32_t data);
#ifdef M2_FAST_SHARC
    DirectBoard m_direct;
    inline uint32_t board_read(uint32_t address);
    inline void board_write(uint32_t address, uint32_t data);
#endif
    template <int CHUNK> int run_chunk(uint64_t &done);
    uint64_t m_budget = 0; // run_generated's, read by the chunks (end_run lowers it)
    template <uint64_t OPCODE> void sharcop_compute_dreg_dm_dreg_pm_t();
    template <uint64_t OPCODE> void sharcop_compute_t();
    template <uint64_t OPCODE> void sharcop_compute_ureg_dmpm_premod_t();
    template <uint64_t OPCODE> void sharcop_compute_ureg_dmpm_postmod_t();
    template <uint64_t OPCODE> void sharcop_compute_dm_to_dreg_immmod_t();
    template <uint64_t OPCODE> void sharcop_compute_dreg_to_dm_immmod_t();
    template <uint64_t OPCODE> void sharcop_compute_pm_to_dreg_immmod_t();
    template <uint64_t OPCODE> void sharcop_compute_dreg_to_pm_immmod_t();
    template <uint64_t OPCODE> void sharcop_compute_ureg_to_ureg_t();
    template <uint64_t OPCODE> void sharcop_imm_shift_dreg_dmpm_t();
    template <uint64_t OPCODE> void sharcop_imm_shift_t();
    template <uint64_t OPCODE> void sharcop_compute_modify_t();
    template <uint64_t OPCODE> void sharcop_direct_call_t();
    template <uint64_t OPCODE> void sharcop_direct_jump_t();
    template <uint64_t OPCODE> void sharcop_relative_call_t();
    template <uint64_t OPCODE> void sharcop_relative_jump_t();
    template <uint64_t OPCODE> void sharcop_indirect_jump_t();
    template <uint64_t OPCODE> void sharcop_indirect_call_t();
    template <uint64_t OPCODE> void sharcop_relative_jump_compute_t();
    template <uint64_t OPCODE> void sharcop_relative_call_compute_t();
    template <uint64_t OPCODE> void sharcop_indirect_jump_compute_dreg_dm_t();
    template <uint64_t OPCODE> void sharcop_relative_jump_compute_dreg_dm_t();
    template <uint64_t OPCODE> void sharcop_rts_t();
    template <uint64_t OPCODE> void sharcop_rti_t();
    template <uint64_t OPCODE> void sharcop_do_until_counter_imm_t();
    template <uint64_t OPCODE> void sharcop_do_until_counter_ureg_t();
    template <uint64_t OPCODE> void sharcop_do_until_t();
    template <uint64_t OPCODE> void sharcop_dm_to_ureg_direct_t();
    template <uint64_t OPCODE> void sharcop_ureg_to_dm_direct_t();
    template <uint64_t OPCODE> void sharcop_pm_to_ureg_direct_t();
    template <uint64_t OPCODE> void sharcop_ureg_to_pm_direct_t();
    template <uint64_t OPCODE> void sharcop_dm_to_ureg_indirect_t();
    template <uint64_t OPCODE> void sharcop_ureg_to_dm_indirect_t();
    template <uint64_t OPCODE> void sharcop_pm_to_ureg_indirect_t();
    template <uint64_t OPCODE> void sharcop_ureg_to_pm_indirect_t();
    template <uint64_t OPCODE> void sharcop_imm_to_dmpm_t();
    template <uint64_t OPCODE> void sharcop_imm_to_ureg_t();
    template <uint64_t OPCODE> void sharcop_sysreg_bitop_t();
    template <uint64_t OPCODE> void sharcop_modify_t();
    template <uint64_t OPCODE> void sharcop_bit_reverse_t();
    template <uint64_t OPCODE> void sharcop_push_pop_stacks_t();
    template <uint64_t OPCODE> void sharcop_nop_t();
    template <uint64_t OPCODE> void sharcop_idle_t();
    template <uint64_t OPCODE> [[noreturn]] void sharcop_unimplemented_t();

    inline bool step_pre();
    inline void step_fetch(uint64_t opcode);
    inline void step_fetch_gen(uint64_t opcode);
    inline void step_loop_end();
    inline void step_body();
    inline bool plain_ready() const;
    inline void step_after(); // instructions until the delayed IOP write (MAME: a timer)

    const char *tag() const { return ":copro_adsp"; }
    // For sharcop_unimplemented's message (no disassembler here: the opcode in hex).
    static std::string disassemble_one(uint32_t pc, uint64_t opcode);
    void build_opcode_table();
    void sharc_iop_delayed_write_callback();
    void sharc_dma_callback(int param); // MAME's DMA timer (DRC only; unused here)

	inline void CHANGE_PC(uint32_t newpc);
	inline void CHANGE_PC_DELAYED(uint32_t newpc);
	void sharc_iop_delayed_w(uint32_t reg, uint32_t data, int cycles);
	uint32_t pm_read32(uint32_t address);
	void pm_write32(uint32_t address, uint32_t data);
	uint64_t pm_read48(uint32_t address);
	void pm_write48(uint32_t address, uint64_t data);
	uint32_t dm_read32(uint32_t address);
	void dm_write32(uint32_t address, uint32_t data);
	void schedule_chained_dma_op(int channel, uint32_t dma_chain_ptr, int chained_direction);
	void schedule_dma_op(int channel, uint32_t src, uint32_t dst, int src_modifier, int dst_modifier, int src_count, int dst_count, int pmode);
	void dma_op(int channel);
	void sharc_dma_exec(int channel);
	void dma_run_cycle(int channel);
	void add_systemreg_write_latency_effect(int sysreg, uint32_t data, uint32_t prev_data);
	void systemreg_write_latency_effect();
	uint32_t GET_UREG(int ureg);
	void SET_UREG(int ureg, uint32_t data);
	void SHIFT_OPERATION_IMM(int shiftop, int data, int rn, int rx);
	void COMPUTE(uint32_t opcode);
	void check_interrupts();
	void PUSH_PC();
	uint32_t POP_PC();
	uint32_t TOP_PC();
	void PUSH_LOOP();
	void POP_LOOP();
	void PUSH_STATUS_STACK();
	void POP_STATUS_STACK();
	int IF_CONDITION_CODE(int cond);
	int DO_CONDITION_CODE(int cond);
	void sharcop_compute_dreg_dm_dreg_pm();
	void sharcop_compute();
	void sharcop_compute_ureg_dmpm_premod();
	void sharcop_compute_ureg_dmpm_postmod();
	void sharcop_compute_dm_to_dreg_immmod();
	void sharcop_compute_dreg_to_dm_immmod();
	void sharcop_compute_pm_to_dreg_immmod();
	void sharcop_compute_dreg_to_pm_immmod();
	void sharcop_compute_ureg_to_ureg();
	void sharcop_imm_shift_dreg_dmpm();
	void sharcop_imm_shift();
	void sharcop_compute_modify();
	void sharcop_direct_call();
	void sharcop_direct_jump();
	void sharcop_relative_call();
	void sharcop_relative_jump();
	void sharcop_indirect_jump();
	void sharcop_indirect_call();
	void sharcop_relative_jump_compute();
	void sharcop_relative_call_compute();
	void sharcop_indirect_jump_compute_dreg_dm();
	void sharcop_relative_jump_compute_dreg_dm();
	void sharcop_rts();
	void sharcop_rti();
	void sharcop_do_until_counter_imm();
	void sharcop_do_until_counter_ureg();
	void sharcop_do_until();
	void sharcop_dm_to_ureg_direct();
	void sharcop_ureg_to_dm_direct();
	void sharcop_pm_to_ureg_direct();
	void sharcop_ureg_to_pm_direct();
	void sharcop_dm_to_ureg_indirect();
	void sharcop_ureg_to_dm_indirect();
	void sharcop_pm_to_ureg_indirect();
	void sharcop_ureg_to_pm_indirect();
	void sharcop_imm_to_dmpm();
	void sharcop_imm_to_ureg();
	void sharcop_sysreg_bitop();
	void sharcop_modify();
	void sharcop_bit_reverse();
	void sharcop_push_pop_stacks();
	void sharcop_nop();
	void sharcop_idle();
	[[noreturn]] void sharcop_unimplemented() ATTR_COLD;

	// ALU helpers
	inline SHARC_REG FADD(int fx, int fy);
	inline SHARC_REG FSUB(int fx, int fy);
	inline SHARC_REG FAVG(int fx, int fy);
	inline SHARC_REG FABS(int fx);
	inline SHARC_REG FMIN(int fx, int fy);
	inline SHARC_REG FMAX(int fx, int fy);
	inline std::pair<SHARC_REG, SHARC_REG> FADD_FSUB(int fx, int fy);
	inline uint32_t SCALB(SHARC_REG fx, int ry);

	// Multiplier helpers
	inline SHARC_REG FMUL(int fx, int fy);

	// ALU fixed-point
	inline void compute_add(int rn, int rx, int ry);
	inline void compute_sub(int rn, int rx, int ry);
	inline void compute_add_ci(int rn, int rx, int ry);
	inline void compute_sub_ci(int rn, int rx, int ry);
	inline void compute_comp(int rx, int ry);
	inline void compute_add_ci(int rn, int rx);
	inline void compute_sub_ci(int rn, int rx);
	inline void compute_inc(int rn, int rx);
	inline void compute_dec(int rn, int rx);
	inline void compute_neg(int rn, int rx);
	inline void compute_abs(int rn, int rx);
	inline void compute_pass(int rn, int rx);
	inline void compute_and(int rn, int rx, int ry);
	inline void compute_or(int rn, int rx, int ry);
	inline void compute_xor(int rn, int rx, int ry);
	inline void compute_not(int rn, int rx);
	inline void compute_min(int rn, int rx, int ry);
	inline void compute_max(int rn, int rx, int ry);
	inline void compute_clip(int rn, int rx, int ry);

	// ALU floating-point
	inline void compute_fadd(int fn, int fx, int fy);
	inline void compute_fsub(int fn, int fx, int fy);
	inline void compute_fadd_abs(int fn, int fx, int fy);
	inline void compute_fsub_abs(int fn, int fx, int fy);
	inline void compute_favg(int fn, int fx, int fy);
	inline void compute_fcomp(int fx, int fy);
	inline void compute_fneg(int fn, int fx);
	inline void compute_fabs(int fn, int fx);
	inline void compute_fpass(int fn, int fx);
	inline void compute_scalb(int fn, int fx, int ry);
	inline void compute_logb(int rn, int fx);
	inline void compute_fix_scaled(int rn, int fx, int ry);
	inline void compute_fix(int rn, int fx);
	inline void compute_float_scaled(int fn, int rx, int ry);
	inline void compute_float(int fn, int rx);
	inline void compute_recips(int fn, int fx);
	inline void compute_rsqrts(int fn, int fx);
	inline void compute_fcopysign(int fn, int fx, int fy);
	inline void compute_fmin(int fn, int fx, int fy);
	inline void compute_fmax(int fn, int fx, int fy);
	inline void compute_fclip(int fn, int fx, int fy);

	// Multiplier
	inline void compute_mul_uuin(int rn, int rx, int ry);
	inline void compute_mul_ssin(int rn, int rx, int ry);
	inline uint32_t compute_mrf_plus_mul_ssin(int rx, int ry);
	inline uint32_t compute_mrb_plus_mul_ssin(int rx, int ry);
	inline void compute_fmul(int fn, int fx, int fy);

	// Dual add/subtract
	inline void compute_dual_add_sub(int ra, int rs, int rx, int ry);
	inline void compute_dual_fadd_fsub(int fa, int fs, int fx, int fy);

	// Fixed-Point multiply/accumulate and add, subtract or average
	inline void compute_mul_ssfr_add(int rm, int rxm, int rym, int ra, int rxa, int rya);
	inline void compute_mul_ssfr_sub(int rm, int rxm, int rym, int ra, int rxa, int rya);

	// Floating-Point multiplication and ALU operation
	inline void compute_fmul_fadd(int fm, int fxm, int fym, int fa, int fxa, int fya);
	inline void compute_fmul_fsub(int fm, int fxm, int fym, int fa, int fxa, int fya);
	inline void compute_fmul_float_scaled(int fm, int fxm, int fym, int fa, int rxa, int rya);
	inline void compute_fmul_fix_scaled(int fm, int fxm, int fym, int ra, int fxa, int rya);
	inline void compute_fmul_favg(int fm, int fxm, int fym, int fa, int fxa, int fya);
	inline void compute_fmul_fabs(int fm, int fxm, int fym, int fa, int fxa);
	inline void compute_fmul_fmax(int fm, int fxm, int fym, int fa, int fxa, int fya);
	inline void compute_fmul_fmin(int fm, int fxm, int fym, int fa, int fxa, int fya);

	// Multiplication and dual add/subtract
	inline void compute_fmul_dual_fadd_fsub(int fm, int fxm, int fym, int fa, int fs, int fxa, int fya);

	inline void compute_multi_mr_to_reg(int ai, int rk);
	inline void compute_multi_reg_to_mr(int ai, int rk);
};

} // namespace rt::sharc
