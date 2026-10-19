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

