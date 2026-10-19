// license:BSD-3-Clause
// copyright-holders:Ville Linde
// The ADSP-21062 SHARC lifted from MAME (src/devices/cpu/sharc/sharc.cpp,
// commit dddd736806; THIRD_PARTY.md): the interpreter's parts, as upstream,
// with MAME's infrastructure replaced where marked (memory spaces, the delayed
// IOP write, flag inputs). sharcops.hxx, compute.hxx, sharcdma.hxx,
// sharcinternal.ipp and sharcops_table.inc are upstream's files unchanged
// (the last without its two #include lines).
#include "runtime/sharc/sharc.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>

#define LOG(...) do {} while (0)

namespace rt::sharc {

#include "runtime/sharc/sharcinternal.ipp"

// ---- upstream sharc.cpp ----

void adsp21062_device::CHANGE_PC(uint32_t newpc)
{
	m_core->pc = newpc;
	m_core->daddr = newpc;
	m_core->faddr = newpc+1;
	m_core->nfaddr = newpc+2;
}

void adsp21062_device::CHANGE_PC_DELAYED(uint32_t newpc)
{
	m_core->nfaddr = newpc;

	m_core->delay_slot1 = m_core->pc;
	m_core->delay_slot2 = m_core->daddr;
}

// MAME: a scheduler timer `cycles` SHARC cycles on; here counted in
// instructions by step() (the SHARC runs one instruction a cycle in MAME).
void adsp21062_device::sharc_iop_delayed_w(uint32_t reg, uint32_t data, int cycles)
{
	m_core->iop_delayed_reg = reg;
	m_core->iop_delayed_data = data;

	m_iop_delay = cycles;
}

void adsp21062_device::sharc_iop_delayed_write_callback()
{
	switch (m_core->iop_delayed_reg)
	{
		case 0x1c:
		{
			if (m_core->iop_delayed_data & 0x1)
			{
				sharc_dma_exec(6);
			}
			break;
		}

		case 0x1d:
		{
			if (m_core->iop_delayed_data & 0x1)
			{
				sharc_dma_exec(7);
			}
			break;
		}

		default:
			throw emu_fatalerror("SHARC: sharc_iop_delayed_write: unknown IOP register %02X\n", m_core->iop_delayed_reg);
	}
}

// 1 453 2h 2l 1l
// 2 678 3h 3l 4h
// 3 ab9 5h 5l 4l
// 4 cde 6h 6l 7h

template <unsigned N>
uint64_t adsp21062_device::pm_r(offs_t offset)
{
	offs_t slot = offset >> 12;
	offs_t base = (offset & 0xfff) + (slot >> 1) * (3<<12);
	if (slot & 1)
		return (uint64_t(m_blocks[N][base + 0x2000]) << 16) | (m_blocks[N][base + 0x1000] & 0xffff);
	else
		return (uint64_t(m_blocks[N][base         ]) << 16) | (m_blocks[N][base + 0x1000] >> 16);
}

template <unsigned N>
void adsp21062_device::pm_w(offs_t offset, uint64_t data, uint64_t mem_mask)
{
	offs_t slot = offset >> 12;
	offs_t base = (offset & 0xfff) + (slot >> 1) * (3<<12);
	if (slot & 1)
	{
		if (ACCESSING_BITS_0_15)
			m_blocks[N][base + 0x1000] = (m_blocks[N][base + 0x1000] & 0xffff0000) | (data & 0xffff);
		m_blocks[N][base + 0x2000] = (m_blocks[N][base + 0x2000] & ~(mem_mask >> 16)) | ((data & mem_mask) >> 16);
	}
	else
	{
		m_blocks[N][base + 0x0000] = (m_blocks[N][base + 0x0000] & ~(mem_mask >> 16)) | ((data & mem_mask) >> 16);
		if (ACCESSING_BITS_0_15)
			m_blocks[N][base + 0x1000] = (m_blocks[N][base + 0x1000] & 0xffff) | ((data & 0xffff) << 16);
	}
}

template <unsigned N>
uint32_t adsp21062_device::dm_short_r(offs_t offset)
{
	return util::little_endian_cast<uint16_t const>(&m_blocks[N][0])[offset];
}

template <unsigned N>
uint32_t adsp21062_device::dm_short_se_r(offs_t offset)
{
	return uint32_t(int32_t(util::little_endian_cast<int16_t const>(&m_blocks[N][0])[offset]));
}

template <unsigned N>
void adsp21062_device::dm_short_w(offs_t offset, uint32_t data)
{
	util::little_endian_cast<uint16_t>(&m_blocks[N][0])[offset] = uint16_t(data);
}


uint32_t adsp21062_device::iop_r(offs_t offset)
{
	switch (offset)
	{
		case 0x00: return 0;    // System configuration

		case 0x37:      // DMA status
		{
			return m_core->dma_status;
		}
		default:
			throw emu_fatalerror("sharc_iop_r: Unimplemented IOP reg %02X at %08X\n", offset, m_core->pc);

		return 0;
	}
}

void adsp21062_device::iop_w(offs_t offset, uint32_t data)
{
	switch (offset)
	{
		case 0x00: m_core->syscon = data; break;
		case 0x02: break;       // External Memory Wait State Configuration
		case 0x04: // External port DMA buffer 0
		{
			external_dma_write(m_core->extdma_shift, data);
			m_core->extdma_shift++;
			if (m_core->extdma_shift == 3)
				m_core->extdma_shift = 0;
			break;
		}

		case 0x08: break;       // Message Register 0
		case 0x09: break;       // Message Register 1
		case 0x0a: break;       // Message Register 2
		case 0x0b: break;       // Message Register 3
		case 0x0c: break;       // Message Register 4
		case 0x0d: break;       // Message Register 5
		case 0x0e: break;       // Message Register 6
		case 0x0f: break;       // Message Register 7

		case 0x14: // reserved??? written by Last Bronx
		case 0x17: break;

		// DMA 6
		case 0x1c:
		{
			m_core->dma[6].control = data;
			if (data & 0x1)
			{
				sharc_iop_delayed_w(0x1c, data, 1);
			}
			break;
		}

		case 0x20: break;

		case 0x40: m_core->dma[6].int_index = data; return;
		case 0x41: m_core->dma[6].int_modifier = data; return;
		case 0x42: m_core->dma[6].int_count = data; return;
		case 0x43: m_core->dma[6].chain_ptr = data; return;
		case 0x44: m_core->dma[6].gen_purpose = data; return;
		case 0x45: m_core->dma[6].ext_index = data; return;
		case 0x46: m_core->dma[6].ext_modifier = data; return;
		case 0x47: m_core->dma[6].ext_count = data; return;

		// DMA 7
		case 0x1d:
		{
			m_core->dma[7].control = data;
			if (data & 0x1)
			{
				sharc_iop_delayed_w(0x1d, data, 30);
			}
			break;
		}

		case 0x48: m_core->dma[7].int_index = data; return;
		case 0x49: m_core->dma[7].int_modifier = data; return;
		case 0x4a: m_core->dma[7].int_count = data; return;
		case 0x4b: m_core->dma[7].chain_ptr = data; return;
		case 0x4c: m_core->dma[7].gen_purpose = data; return;
		case 0x4d: m_core->dma[7].ext_index = data; return;
		case 0x4e: m_core->dma[7].ext_modifier = data; return;
		case 0x4f: m_core->dma[7].ext_count = data; return;

		default:
			throw emu_fatalerror("sharc_iop_w: Unimplemented IOP reg %02X, %08X at %08X\n", offset, data, m_core->pc);
	}
}


#include "runtime/sharc/sharcdma.hxx"
#include "runtime/sharc/sharcops.hxx"
#include "runtime/sharc/sharcops_table.inc"

void adsp21062_device::build_opcode_table()
{
	for (int i = 0; i < std::size(m_sharc_op); i++)
	{
		m_sharc_op[i] = &adsp21062_device::sharcop_unimplemented;

		const uint16_t op = i << 7;

		int j = 0;
		while (j < s_num_ops)
		{
			auto &opcode = s_sharc_opcode_table[j++];
			if ((opcode.op_mask & op) == opcode.op_bits)
			{
				m_sharc_op[i] = opcode.handler;
				break;
			}
		}
		while (j < s_num_ops)
		{
			auto &opcode = s_sharc_opcode_table[j++];
			if ((opcode.op_mask & op) == opcode.op_bits)
			{
				throw emu_fatalerror("build_opcode_table: table already filled! (i=%04X, j=%d)\n", i, j);
			}
		}
	}
}


void adsp21062_device::external_iop_write(offs_t address, uint32_t data)
{
	// host packing mode used to determine width of external host bus width (16/32)
	// if writing to external port DMA buffer, just write the data directly;
	// external_dma_write() handles packed data
	if (m_core->syscon & 0x10 && address != 0x04)
	{
		if ((m_core->iop_write_num++ & 1) == 0)
		{
			m_core->iop_data = data & 0xffff;
			return;
		}
		else
		{
			m_core->iop_data |= (data & 0xffff) << 16;
		}
	}
	else
	{
		m_core->iop_data = data;
	}

	if (address == 0x1c)
	{
		m_core->dma[6].control = m_core->iop_data;
	}
	else
	{
		LOG("SHARC IOP write %08X, %08X\n", address, m_core->iop_data);
		m_data.write_dword(address, m_core->iop_data);
	}
}

void adsp21062_device::external_dma_write(offs_t address, uint64_t data)
{
	/*
	All addresses in the 17-bit index registers are offset by 0x0002 0000, the
	first internal RAM location, before they are used by the DMA controller.
	*/

	offs_t const index = (m_core->dma[6].int_index & 0x1ffff) | 0x20000;
	unsigned const mswf = BIT(m_core->dma[6].control, 8);
	unsigned const pmode = BIT(m_core->dma[6].control, 6, 2);
	unsigned const dtype = BIT(m_core->dma[6].control, 5);
	switch (pmode)
	{
		case 0:         // no packing
		{
			if (dtype)
				pm_write32(index, data);
			else
				dm_write32(index, data);

			m_core->dma[6].int_index += m_core->dma[6].int_modifier;
			break;
		}
		case 2:         // 16/48 packing
		{
			// FIXME: honour DTYPE
			unsigned const word = address % 3;
			unsigned const shift = (mswf ? (2 - word) : word) * 16;

			uint64_t r = pm_read48(index);
			r &= ~(uint64_t(0xffff) << shift);
			r |= (data & 0xffff) << shift;

			pm_write48(index, r);

			if (word == 2)
			{
				m_core->dma[6].int_index += m_core->dma[6].int_modifier;
			}
			break;
		}
		default:
		{
			throw emu_fatalerror("sharc_external_dma_write: unimplemented packing mode %d\n", pmode);
		}
	}
}

// ---- MAME's address maps and device glue, ours ----

// pgm_2m: block 1 at 0x20000-0x24fff mirrored by 0x18000, block 0 over it at
// 0x20000-0x24fff; nothing else (unmapped reads 0).
uint64_t adsp21062_device::program_space::read_qword(uint32_t address)
{
	address &= 0xffffff;
	if (address < 0x20000 || address >= 0x40000 || (address & 0x7fff) >= 0x5000) return 0;
	return (address & 0x18000) ? d.pm_r<1>(address & 0x7fff) : d.pm_r<0>(address & 0x7fff);
}

void adsp21062_device::program_space::write_qword(uint32_t address, uint64_t data, uint64_t mem_mask)
{
	address &= 0xffffff;
	if (address < 0x20000 || address >= 0x40000 || (address & 0x7fff) >= 0x5000) return;
	const uint64_t before = d.m_gen_end && address < d.m_gen_end ? read_qword(address) : 0;
	if (address & 0x18000) d.pm_w<1>(address & 0x7fff, data, mem_mask);
	else d.pm_w<0>(address & 0x7fff, data, mem_mask);
	// Ours: the recompiled program cannot follow a rewrite of its own code.
	if (d.m_gen_end && address < d.m_gen_end && address >= 0x20000 && read_qword(address) != before)
		throw SharcFatal(emu_fatalerror::fmt("SHARC program memory rewritten at %05x (recompiled code)", address));
}

// data_2m: the IOP registers, the two blocks (32-bit words at 0x20000-0x27fff,
// block 1 mirrored by 0x18000), their 16-bit view at 0x40000-0x4ffff (block 1
// mirrored by 0x30000; sign-extended reads while MODE1 SSE is set); above
// 0x400000 the board (model2b copro_sharc_map).
uint32_t adsp21062_device::data_space::read_dword(uint32_t address)
{
	if (address < 0x100) return d.iop_r(address);
	if (address >= 0x20000 && address < 0x40000) return d.m_blocks[(address & 0x18000) ? 1 : 0][address & 0x7fff];
	if (address >= 0x40000 && address < 0x80000) {
		const unsigned n = (address & 0x30000) ? 1 : 0;
		const offs_t o = address & 0xffff;
		if (d.m_short_se) return n ? d.dm_short_se_r<1>(o) : d.dm_short_se_r<0>(o);
		return n ? d.dm_short_r<1>(o) : d.dm_short_r<0>(o);
	}
	if (address >= 0x400000) return d.m_bus.read(address);
	return 0;
}

void adsp21062_device::data_space::write_dword(uint32_t address, uint32_t data)
{
	if (address < 0x100) { d.iop_w(address, data); return; }
	if (address >= 0x20000 && address < 0x40000) { d.m_blocks[(address & 0x18000) ? 1 : 0][address & 0x7fff] = data; return; }
	if (address >= 0x40000 && address < 0x80000) {
		const offs_t o = address & 0xffff;
		if (address & 0x30000) d.dm_short_w<1>(o, data);
		else d.dm_short_w<0>(o, data);
		return;
	}
	if (address >= 0x400000) d.m_bus.write(address, data);
}

adsp21062_device::adsp21062_device(Bus &bus) : m_bus(bus)
{
	// MAME device_start (interpreter): the state, zeroed; the opcode table.
	m_heap_core = std::make_unique<sharc_internal_state>();
	m_core = m_heap_core.get();
	std::memset(static_cast<void *>(m_core), 0, sizeof(sharc_internal_state));
	m_block_storage = std::make_unique<uint32_t[]>(2 * 0x8000);
	m_blocks[0] = m_block_storage.get();
	m_blocks[1] = m_block_storage.get() + 0x8000;
	build_opcode_table();
	reset();
}

adsp21062_device::~adsp21062_device() = default;

// MAME device_reset, host boot (model2b: BOOT_MODE_HOST), and machine_reset's
// halt and FIFO flags (FLAG0, input FIFO empty: 1; FLAG1: 0).
void adsp21062_device::reset()
{
	std::fill(m_blocks[0], m_blocks[0] + 2 * 0x8000, 0);

	m_core->dma[6].int_index      = 0x20000;
	m_core->dma[6].int_modifier   = 1;
	m_core->dma[6].int_count      = 0x100;
	m_core->dma[6].ext_index      = 0x400000;
	m_core->dma[6].ext_modifier   = 1;
	m_core->dma[6].ext_count      = 0x600;
	m_core->dma[6].control        = 0xa1;

	m_core->pc = 0x20004;
	m_core->extdma_shift = 0;
	m_core->daddr = m_core->pc + 1;
	m_core->faddr = m_core->daddr + 1;
	m_core->nfaddr = m_core->faddr+1;

	m_core->idle = 0;
	m_core->mode1 = 0x00000000;
	m_core->mode2 &= 0xf0000000;
	m_core->astat &= FLG0 | FLG1 | FLG2 | FLG3;
	m_core->stky = PCEM | SSEM | LSEM;
	m_core->irptl = 0x0000;
	m_core->imask = 0x0003;
	m_core->ustat1 = 0x0000;
	m_core->ustat2 = 0x0000;

	m_core->pcstkp = 0;
	m_core->lstkp = 0;
	m_core->pcstk = 0x00ffffff;
	m_core->curlcntr = 0xffffffff;
	m_core->lcntr = m_core->lcstack[0];
	m_core->laddr.unpack(0xffffffff);
	m_core->status_stkp = 0;
	m_core->interrupt_active = 0;

	m_core->syscon = 0x00000010;
	m_core->sysstat &= 0x00000ff0;
	m_core->iop_write_num = 0;
	m_core->iop_data = 0;

	m_halted = true;
	m_iop_delay = 0;
	m_core->flag[0] = 1;
	m_core->flag[1] = 0;
}

void adsp21062_device::set_flag_input(int flag_num, int state)
{
	if (BIT(m_core->mode2, flag_num + 15))
		throw emu_fatalerror("sharc_set_flag_input: flag %d is set output!\n", flag_num);
	m_core->flag[flag_num] = state ? 1 : 0;
}

std::string adsp21062_device::disassemble_one(uint32_t pc, uint64_t opcode)
{
	char b[48];
	std::snprintf(b, sizeof b, "opcode %012llx", (unsigned long long)opcode);
	return b;
}

uint32_t adsp21062_device::pc() const { return m_core->pc; }
uint32_t adsp21062_device::daddr() const { return m_core->daddr; }
uint64_t adsp21062_device::opcode() const { return m_core->opcode; }
uint32_t adsp21062_device::astat() const { return m_core->astat; }
uint32_t adsp21062_device::mode1() const { return m_core->mode1; }
uint32_t adsp21062_device::r(int i) const { return uint32_t(m_core->r[i].r); }
bool adsp21062_device::idle() const { return m_core->idle != 0; }


void adsp21062_device::check_interrupts()
{
	if ((m_core->imask & m_core->irq_pending) && (m_core->mode1 & MODE1_IRPTEN) && !m_core->interrupt_active &&
		m_core->pc != m_core->delay_slot1 && m_core->pc != m_core->delay_slot2)
	{
		int which = 0;
		for (int i = 0; i < 32; i++)
		{
			if (BIT(m_core->irq_pending, i))
				break;
			which++;
		}

		PUSH_PC();
		if (m_core->idle)
			m_core->pcstk = m_core->pc + 1;
		else
			m_core->pcstk = m_core->daddr;

		m_core->irptl |= 1 << which;

		// TODO: timer and VIRPT interrupts also push the status stack
		if (which >= 6 && which <= 8)
			PUSH_STATUS_STACK();

		CHANGE_PC(0x20000 + (which * 0x4));

		/* TODO: alter IMASKP */

		m_core->active_irq_num = which;
		m_core->irq_pending &= ~(1 << which);

		m_core->interrupt_active = 1;
	}
}

// MAME execute_run (interpreter), one instruction, in the pieces the
// recompiled program (m2sharcrecomp) runs too: step_pre (the delayed IOP
// write, idling, interrupts), step_fetch (the pipeline and ASTAT history),
// step_loop_end (at a do-until's end address), the instruction, step_after
// (the system register latency and the DMA cycle). Ours: an idle SHARC with no
// interrupt pending runs its active DMAs one cycle (MAME runs them for the rest
// of its timeslice); the delayed IOP write (a timer in MAME) is due after its
// count of instructions; the interpreter loop's body is upstream's.

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

void adsp21062_device::step()
{
	if (m_halted || !step_pre()) return;
	step_fetch(m_program.read_qword(m_core->daddr));
	step_loop_end();
	(this->*m_sharc_op[(m_core->opcode >> 39) & 0x1ff])();
	step_after();
}

} // namespace rt::sharc
