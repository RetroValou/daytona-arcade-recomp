// license:BSD-3-Clause
// copyright-holders:Ville Linde
// The SHARC's pipeline step, shared by the interpreter (sharc.cpp step()) and
// the recompiled program's chunks (m2sharcrecomp): upstream's CHANGE_PC and
// CHANGE_PC_DELAYED (sharc.cpp) and the pieces of its interpreter loop
// (execute_run), as inline functions so every translation unit has them.
// Included inside namespace rt::sharc, after sharcinternal.ipp.

inline void adsp21062_device::CHANGE_PC(uint32_t newpc)
{
	m_core->pc = newpc;
	m_core->daddr = newpc;
	m_core->faddr = newpc+1;
	m_core->nfaddr = newpc+2;
}

inline void adsp21062_device::CHANGE_PC_DELAYED(uint32_t newpc)
{
	m_core->nfaddr = newpc;

	m_core->delay_slot1 = m_core->pc;
	m_core->delay_slot2 = m_core->daddr;
}


// Per-instruction execution counts for profiling builds (M2_SHARC_PROFILE).
#ifdef M2_SHARC_PROFILE
#define M2SHARC_COUNT(k) do { if (m_profile) ++m_profile[k]; } while (0)
#else
#define M2SHARC_COUNT(k) do {} while (0)
#endif

#if defined(__GNUC__)
#define SHARC_INLINE inline __attribute__((always_inline))
#else
#define SHARC_INLINE __forceinline
#endif

// False when the SHARC idles (no instruction this step).
SHARC_INLINE bool adsp21062_device::step_pre()
{
	if (m_iop_delay > 0 && --m_iop_delay == 0) sharc_iop_delayed_write_callback();
	if (m_core->idle && m_core->irq_pending == 0)
	{
		if (!m_core->write_stalled)
		{
			dma_run_cycle(6);
			dma_run_cycle(7);
		}
		return false;
	}
	if (m_core->irq_pending != 0)
	{
		check_interrupts();
		m_core->idle = 0;
	}
	return !m_core->idle && !m_core->write_stalled;
}

SHARC_INLINE void adsp21062_device::step_fetch(uint64_t opcode)
{
	m_core->pc = m_core->daddr;
	m_core->daddr = m_core->faddr;
	m_core->faddr = m_core->nfaddr;
	m_core->nfaddr++;

	m_core->astat_old_old_old = m_core->astat_old_old;
	m_core->astat_old_old = m_core->astat_old;
	m_core->astat_old = m_core->astat;

	m_core->opcode = opcode;
	if (m_trace) m_trace(m_trace_ctx, *this); // where MAME's M2TRACE_SHARCPC logs (patch 0005)
}

// step_fetch for the recompiled program's full steps: its handlers take the
// opcode as a template parameter, so it is stored only for the trace.
SHARC_INLINE void adsp21062_device::step_fetch_gen(uint64_t opcode)
{
	m_core->pc = m_core->daddr;
	m_core->daddr = m_core->faddr;
	m_core->faddr = m_core->nfaddr;
	m_core->nfaddr++;

	m_core->astat_old_old_old = m_core->astat_old_old;
	m_core->astat_old_old = m_core->astat_old;
	m_core->astat_old = m_core->astat;

	if (m_trace) { m_core->opcode = opcode; m_trace(m_trace_ctx, *this); }
}

inline void adsp21062_device::step_loop_end()
{
	// handle looping
	if (!(m_core->stky & LSEM) && (m_core->pc == m_core->laddr.addr))
	{
		switch (m_core->laddr.loop_type)
		{
		case 0:     // arithmetic condition-based
		{
			if ((m_core->pc - TOP_PC()) > 2)
			{
				m_core->astat = m_core->astat_old_old_old;
			}

			if (DO_CONDITION_CODE(m_core->laddr.code))
			{
				POP_LOOP();
				POP_PC();
			}
			else
			{
				CHANGE_PC(TOP_PC());
			}

			m_core->astat = m_core->astat_old;
			break;
		}
		case 1:     // counter-based, length 1
		{
			//throw emu_fatalerror("SHARC: counter-based loop, length 1 at %08X\n", m_pc);
			//break;
		}
		case 2:     // counter-based, length 2
		{
			//throw emu_fatalerror("SHARC: counter-based loop, length 2 at %08X\n", m_pc);
			//break;
		}
		case 3:     // counter-based, length >2
		{
			--m_core->curlcntr;
			if (m_core->curlcntr == 0)
			{
				POP_LOOP();
				POP_PC();
			}
			else
			{
				CHANGE_PC(TOP_PC());
			}
		}
		}
	}
}

// The recompiled program's plain instructions skip the pipeline and the
// latency count: they run only where the pipeline is linear (what a delayed
// branch breaks) and no MODE1 write waits for its latency.
SHARC_INLINE bool adsp21062_device::plain_ready() const
{
	return m_core->systemreg_latency_cycles <= 0 && m_core->faddr == m_core->daddr + 1 && m_core->nfaddr == m_core->daddr + 2;
}

SHARC_INLINE void adsp21062_device::step_after()
{
	// System register latency effect
	if (m_core->systemreg_latency_cycles > 0)
	{
		--m_core->systemreg_latency_cycles;
		if (m_core->systemreg_latency_cycles <= 0)
		{
			systemreg_write_latency_effect();
		}
	}

	// Ours: the calls only while a DMA runs (dma_run_cycle tests the same bits).
	if (!m_core->write_stalled && (m_core->dma_status & ((1 << 6) | (1 << 7))))
	{
		dma_run_cycle(6);
		dma_run_cycle(7);
	}
	++m_instructions;
}

#ifdef M2_FAST_SHARC
// The board (M2_FAST_SHARC): TgpBoard::SharcCopro's read and write, inline.
SHARC_INLINE uint32_t adsp21062_device::board_read(uint32_t address)
{
	M2SHARC_EV(ext_r);
	if (address >= 0x400000 && address < 0xc00000) {
		WordFifo &in = *m_direct.in;
		if (in.empty()) return M2SHARC_EV(fifo_in_empty), 0;
		M2SHARC_EV(fifo_in);
		const uint32_t v = in.front();
		in.pop_front();
		if (in.empty()) set_flag_input(0, 1);
		return v;
	}
	if (address >= 0x1400000 && address < 0x1c00000) return M2SHARC_EV(buffer_r), m_direct.buffer[address & 0x7fff];
	if (address >= 0x1c00000 && address < 0x1e00000)
		return M2SHARC_EV(rom_r), m_direct.rom[(0x800000 | (address - 0x1c00000)) & m_direct.rom_mask];
	return M2SHARC_EV(other), 0;
}

SHARC_INLINE void adsp21062_device::board_write(uint32_t address, uint32_t data)
{
	M2SHARC_EV(ext_w);
	if (address >= 0xc00000 && address < 0x1400000) {
		M2SHARC_EV(fifo_out);
		m_direct.out->push_back(data);
		if (*m_direct.stop_on_output) end_run(); // need_output: the word the i960 waits for
	} else if (address >= 0x1400000 && address < 0x1c00000) {
		M2SHARC_EV(buffer_w);
		m_direct.buffer[address & 0x7fff] = data;
	} else {
		M2SHARC_EV(other);
	}
}
#endif

// The recompiled handlers' data memory accesses: internal memory's 32-bit
// words inline (data_space::read_dword / write_dword's second case, the same
// result), the board directly with M2_FAST_SHARC, the rest through the data
// space.
SHARC_INLINE uint32_t adsp21062_device::dm_read32_i(uint32_t address)
{
	if (address - 0x20000 < 0x20000) {
		M2SHARC_EV(dm_r);
		return m_blocks[(address & 0x18000) ? 1 : 0][address & 0x7fff];
	}
#ifdef M2_FAST_SHARC
	if (address >= 0x400000 && m_direct.in) return board_read(address);
#endif
	return m_data.read_dword(address);
}

SHARC_INLINE void adsp21062_device::dm_write32_i(uint32_t address, uint32_t data)
{
	if (address - 0x20000 < 0x20000) {
		M2SHARC_EV(dm_w);
		m_blocks[(address & 0x18000) ? 1 : 0][address & 0x7fff] = data;
		return;
	}
#ifdef M2_FAST_SHARC
	if (address >= 0x400000 && m_direct.in) return board_write(address, data);
#endif
	m_data.write_dword(address, data);
}

// The recompiled handlers' 32-bit program memory data (pm_read32 / pm_write32:
// program_space::read_qword / write_qword and pm_r / pm_w with the 32-bit mask,
// worked out): the internal word that holds it, or null where read_qword reads
// 0 and write_qword writes nothing.
SHARC_INLINE uint32_t *adsp21062_device::pm_data32(uint32_t address)
{
	address &= 0xffffff;
	if (address < 0x20000 || address >= 0x40000 || (address & 0x7fff) >= 0x5000) return nullptr;
	const uint32_t offset = address & 0x7fff, slot = offset >> 12;
	return &m_blocks[(address & 0x18000) ? 1 : 0][(offset & 0xfff) + (slot >> 1) * 0x3000 + ((slot & 1) ? 0x2000 : 0)];
}

SHARC_INLINE uint32_t adsp21062_device::pm_read32_i(uint32_t address)
{
	M2SHARC_EV(pm_r);
	const uint32_t *w = pm_data32(address);
	return w ? *w : 0;
}

SHARC_INLINE void adsp21062_device::pm_write32_i(uint32_t address, uint32_t data)
{
	if (m_gen_end && (address & 0xffffff) < m_gen_end) return pm_write32(address, data); // the rewrite check
	M2SHARC_EV(pm_w);
	if (uint32_t *w = pm_data32(address)) *w = data;
}

// One instruction after step_pre: the interpreter's (step) and the recompiled
// program's where it cannot run plain (run_generated).
inline void adsp21062_device::step_body()
{
	step_fetch(m_program.read_qword(m_core->daddr));
	step_loop_end();
	(this->*m_sharc_op[(m_core->opcode >> 39) & 0x1ff])();
	step_after();
}
