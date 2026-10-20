// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// The Model 2 board as native runtime (see m2_board.h). Register semantics
// follow MAME's src/mame/sega/model2.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above kept):
// irq_request_r/irq_ack_w/irq_enable_w/irq_update, timers_w, videoctl_r/w,
// render_mode_r/w, tgpid_r, the model2o memory map. The I/O board's mailbox
// protocol was read from the game's own traffic (see IoBoard). See
// THIRD_PARTY.md.

#include "runtime/board/m2_board.h"
#include "runtime/cpu/i960_profile.h"

#include <algorithm>
#include <iterator>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

// ---------------------------------------------------------------------------
// I/O board

void IoBoard::write(uint32_t index, uint8_t v) {
    index &= 0x7ff;
    ram_[index] = v;
    if (index == 0x11 && drive_commands.size() < kMaxDrive) drive_commands.push_back(v); // to the drive board
    if (index != 0x20) return;
    switch (v) {
    case 1: // latch inputs
        if (gun.present()) { // the light gun board (Virtua Cop's 837-12079), as MAME's dump of it
            for (int i = 0; i < 8; i++) ram_[size_t(i)] = 0xff; // no ADC
            ram_[8] = inputs.in0;
            ram_[9] = uint8_t(inputs.in1 | 0x70); // the driving layout's gear bits: unused here (1)
            ram_[10] = inputs.in2;
            for (int i = 0x0b; i < 0x0e; i++) ram_[size_t(i)] = 0xff;
            ram_[0x0e] = 0x0f; // the board's DIP switches (MAME's default: Normal reloading, Normal enemies)
            latch_gun();
        } else {
            ram_[0] = inputs.steer;
            ram_[1] = inputs.accel;
            ram_[2] = inputs.brake;
            for (int i = 3; i < 8; i++) ram_[size_t(i)] = 0xff; // unused ADC channels
            ram_[8] = inputs.in0;
            ram_[9] = inputs.in1;
            ram_[10] = inputs.in2;
        }
        ram_[0x21] = 0x40; // board status the game checks at boot
        ram_[0x20] = 0;
        break;
    case 2: // store settings
        std::memcpy(eeprom.data(), &ram_[0x100], eeprom.size());
        eeprom_dirty = true;
        ram_[0x20] = 0;
        break;
    case 3: // load settings
        std::memcpy(&ram_[0x100], eeprom.data(), eeprom.size());
        ram_[0x20] = 0;
        break;
    default: break;
    }
}

// The light gun's report, as MAME's lightgun ports give it (model1io2 for the
// Model 1 I/O 2 board, model2 lightgun_data_r / lightgun_offscreen_r for the
// 2A-CRX): bytes 0-7 each gun's Y then X (16 bits, little-endian; player 1
// then 2), byte 8 the off-screen bits (1: player 1, 2: player 2), set within
// 5 % of the range from an edge, as MAME's. Player 2 stays at MAME's defaults.
uint8_t IoBoard::gun_byte(unsigned i) const {
    if (i < 8) {
        const uint16_t v[4] = {inputs.gun_y, inputs.gun_x, gun.p2_y, gun.p2_x};
        return uint8_t(v[i >> 1] >> (8 * (i & 1)));
    }
    const int bx = int(float(gun.x_max - gun.x_min) * 0.05f), by = int(float(gun.y_max - gun.y_min) * 0.05f);
    const bool off = inputs.gun_x <= gun.x_min + bx || inputs.gun_x >= gun.x_max - bx ||
                     inputs.gun_y <= gun.y_min + by || inputs.gun_y >= gun.y_max - by;
    return off ? 1 : 0;
}

// The Model 1 I/O 2 board (Virtua Cop): the report at 0x80-0x88 of the DPRAM.
void IoBoard::latch_gun() {
    for (unsigned i = 0; i < 9; ++i) ram_[0x80 + i] = gun_byte(i);
}

// --- 315-5649 (Model 2A-CRX) ---------------------------------------------------
// MAME sega_315_5649_device::read/write and model2a_state's port callbacks
// (eeprom_w, in0_r); the game's wiring of its controls: Game::io.

uint8_t IoBoard::port_in(uint32_t offset, uint64_t now) {
    switch (offset) {
    case 1: { // in0_r
        const uint8_t in0 = wiring.port ? wiring.port(1, inputs) : 0xff;
        return ctrl_mode_ ? uint8_t(0xc0 | (ee_do(now) ? 0x20 : 0) | 0x10 | (in0 & 0x0f)) : in0;
    }
    case 2: case 3: return wiring.port ? wiring.port(offset, inputs) : 0xff;
    case 6: return 0xff; // SW3 DIP switches, all off
    default: return 0xff;
    }
}

uint8_t IoBoard::crx_read(uint32_t offset, uint64_t now) {
    switch (offset) {
    case 0x06:
        if (mode_ & 0x80) { // port G counter mode: no counters connected
            port_[6] = uint8_t((port_[6] & 0xf8) | ((port_[6] + 1) & 7));
            return 0;
        }
        [[fallthrough]];
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        return (port_config_ >> offset) & 1 ? port_in(offset, now) : port_[offset];
    case 0x0c: // serial channel 2: the 2A-CRX light gun (MAME lightgun_mux_r: past byte 7 the off-screen bits, 0xfc set)
        if (gun.present()) return gun_mux_ < 8 ? gun_byte(gun_mux_) : uint8_t(0xfc | gun_byte(8));
        return 0;
    case 0x0b: return 0; // RS-422 receive: nothing connected
    case 0x0d: return 0x0c;         // RS-422 status (MAME's: receive buffers full, transmit empty)
    case 0x0f: {                    // analog input, auto-increments
        const uint8_t ch = analog_channel_;
        analog_channel_ = (analog_channel_ + 1) & 7;
        return wiring.analog ? wiring.analog(ch, inputs) : 0xff;
    }
    default: return 0xff;
    }
}

void IoBoard::crx_write(uint32_t offset, uint8_t v, uint64_t now) {
    switch (offset) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        port_[offset] = v;
        if (offset == 0) eeprom_pins(v, now);
        else if (offset == 4 && drive_commands.size() < kMaxDrive) drive_commands.push_back(v);
        break;
    case 0x08: port_config_ = v; break;
    case 0x0e: mode_ = v; break;
    case 0x0f: analog_channel_ = v & 7; break;
    case 0x0a: gun_mux_ = v; break; // serial channel 2: the light gun byte to read (MAME lightgun_mux_w)
    default: break; // RS-422 transmit: nothing connected
    }
}

// --- 93C46 (MAME eeprom_serial_93cxx_device: 64 x 16 bits) -------------------
// Write/erase cycles keep DO low (busy) for MAME's times: 1.75 ms for a word,
// 1 ms for an erase, 8 ms for all words, at 25 MHz i960 instructions.

namespace {
constexpr uint64_t kEeWrite = 43750, kEeErase = 25000, kEeAll = 200000;
constexpr uint32_t kEeAddrBits = 6, kEeDataBits = 16;
} // namespace

void IoBoard::ee_set_word(uint32_t a, uint16_t v) {
    eeprom[a * 2] = uint8_t(v);
    eeprom[a * 2 + 1] = uint8_t(v >> 8);
    eeprom_dirty = true;
}

bool IoBoard::ee_do(uint64_t now) const {
    if (ee_state_ == EeState::WaitStart) return ee_ready(now);
    if (ee_state_ == EeState::Reading) return (ee_shift_ & 0x80000000u) != 0;
    return true; // tristated, pulled up
}

void IoBoard::eeprom_pins(uint8_t a, uint64_t now) {
    ee_now_ = now;
    ctrl_mode_ = a & 1;
    ee_di_ = (a >> 5) & 1;
    const bool clk = (a >> 7) & 1, cs = (a >> 6) & 1;
    if (clk != ee_clk_) {
        ee_clk_ = clk;
        if (clk) ee_clock();
    }
    if (cs != ee_cs_) {
        ee_cs_ = cs;
        ee_state_ = cs ? (ee_state_ == EeState::Reset ? EeState::WaitStart : ee_state_) : EeState::Reset;
    }
}

void IoBoard::ee_clock() { // CLK rising edge
    switch (ee_state_) {
    case EeState::WaitStart:
        if (ee_di_ && ee_ready(ee_now_)) {
            ee_accum_ = ee_bits_ = 0;
            ee_state_ = EeState::Command;
        }
        break;
    case EeState::Command: {
        ee_accum_ = (ee_accum_ << 1) | uint32_t(ee_di_);
        if (++ee_bits_ < 2 + kEeAddrBits) break;
        ee_bits_ = 0;
        const uint32_t op = ee_accum_ >> kEeAddrBits, addr = ee_accum_ & ((1u << kEeAddrBits) - 1);
        ee_address_ = uint8_t(addr);
        if (op == 2) { // READ: a dummy 0 bit, then the word
            ee_shift_ = 0;
            ee_state_ = EeState::Reading;
        } else if (op == 1) { // WRITE
            ee_command_ = 1;
            ee_shift_ = 0;
            ee_state_ = EeState::WaitData;
        } else if (op == 3) { // ERASE
            if (ee_locked_) { ee_state_ = EeState::Reset; break; }
            ee_set_word(addr, 0xffff);
            ee_done_at_ = ee_now_ + kEeErase;
            ee_state_ = EeState::Completion;
        } else switch (addr >> (kEeAddrBits - 2)) {
            case 0: ee_locked_ = true; ee_state_ = EeState::Reset; break;   // WRDS
            case 1: ee_command_ = 0; ee_shift_ = 0; ee_state_ = EeState::WaitData; break; // WRAL
            case 2:                                                          // ERAL
                if (ee_locked_) { ee_state_ = EeState::Reset; break; }
                for (uint32_t i = 0; i < 64; i++) ee_set_word(i, 0xffff);
                ee_done_at_ = ee_now_ + kEeAll;
                ee_state_ = EeState::Completion;
                break;
            default: ee_locked_ = false; ee_state_ = EeState::Reset; break; // EWEN
            }
        break;
    }
    case EeState::Reading: {
        const uint32_t bit = ee_bits_++;
        if (bit == 0) ee_shift_ = uint32_t(ee_word(ee_address_)) << (32 - kEeDataBits);
        else ee_shift_ = (ee_shift_ << 1) | 1;
        break;
    }
    case EeState::WaitData:
        ee_shift_ = (ee_shift_ << 1) | uint32_t(ee_di_);
        if (++ee_bits_ < kEeDataBits) break;
        if (ee_locked_) { ee_state_ = EeState::Reset; break; }
        if (ee_command_ == 1) {
            ee_set_word(ee_address_, uint16_t(ee_shift_));
            ee_done_at_ = ee_now_ + kEeWrite;
        } else { // WRAL: ANDed into the cells (an ERAL comes first)
            for (uint32_t i = 0; i < 64; i++) ee_set_word(i, uint16_t(ee_word(i) & ee_shift_));
            ee_done_at_ = ee_now_ + kEeAll;
        }
        ee_state_ = EeState::Completion;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Board

#ifdef M2_DC_MEMORY
// The Dreamcast: no texture RAM vectors (the frontend's, in video RAM), a
// two-level page table, ROM through img_.rom.
M2Board::M2Board(Images images)
    : img_(std::move(images)), ram_(0x20000), work_(0x100000), cpuctl_(0x1000), backup_(0x4000, 0xff), tile_(0x10000),
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), luma_(0x20000), comm_(0x4000),
      chunks_(size_t(1) << 12), tgp_(img_.copro_tables, *img_.rom) {
    if (!img_.rom || !img_.texture_ram) throw Fatal("M2_DC_MEMORY: no ROM source or texture RAM");
    img_.copro_tables = {}; // the TGP board keeps its own copy (as words)
    map_rom(0x00000000, 0x001fffff, RomRegion::Program, 0);
    map(0x00200000, 0x0021ffff, Ram, ram_.data());
    map_rom(0x00220000, 0x0023ffff, RomRegion::Program, 0x20000);
#else
M2Board::M2Board(Images images)
    : img_(std::move(images)), ram_(0x20000), work_(0x100000), cpuctl_(0x1000), backup_(0x4000, 0xff), tile_(0x10000),
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x200000), tex1_(0x200000), luma_(0x20000), fb_a_(0x80000),
      fb_b_(0x80000), comm_(0x4000), pages_(size_t(1) << (32 - kPageBits)), tgp_(img_.copro_tables, img_.copro_data) {
    // MAME model2_base_mem and model2_tgp_mem, then model2o_mem or model2a_crx_mem
    map(0x00000000, 0x001fffff, Rom, img_.program.data());
    if (crx()) {
        ram_.assign(0x40000, 0);
        map(0x00200000, 0x0023ffff, Ram, ram_.data());
    } else {
        map(0x00200000, 0x0021ffff, Ram, ram_.data());
        map(0x00220000, 0x0023ffff, Rom, img_.program.data() + 0x20000);
    }
#endif
    io_.wiring = img_.game->io;
    io_.gun = img_.game->gun;
    // The set's default settings (MAME loads its "eeprom" region the same way);
    // a frontend's saved EEPROM replaces them after.
    if (img_.eeprom.size() == io_.eeprom.size()) std::copy(img_.eeprom.begin(), img_.eeprom.end(), io_.eeprom.begin());
#ifdef M2_SPIN_SKIP
    std::copy(std::begin(img_.game->spin), std::end(img_.game->spin), std::begin(spin_));
    for (int i = 0; i < 2; i++)
        if (spin_[i].kind != Game::SpinWait::None) spin_addr_[i] = spin_[i].addr;
#endif
    map(0x00500000, 0x005fffff, Ram, work_.data());
    map(0x00800000, 0x00807fff, Dev, nullptr, 0, false);
    map(0x00880000, 0x00887fff, Dev, nullptr, 0, false);
    map(0x00900000, 0x0091ffff, Dev, nullptr, 0x60000, true); // buffer RAM (TgpBoard)
    map(0x00980000, 0x00980fff, Dev, nullptr, 0, false);
    map(0x00e00000, 0x00e00fff, Ram, cpuctl_.data(), 0, false);
    map(0x00e80000, 0x00e80fff, Dev, nullptr, 0, false);
    map(0x00f00000, 0x00f00fff, Dev, nullptr, 0, false);
    map(0x01000000, 0x0100ffff, Ram, tile_.data(), 0x110000);
    map(0x01040000, 0x01040fff, Dev, nullptr, 0x100000, false);
    map(0x01060000, 0x01060fff, Dev, nullptr, 0x100000, false);
    map(0x01080000, 0x010fffff, Ram, chr_.data(), 0x100000);
    map(0x01800000, 0x01803fff, Ram, palette_.data());
    map(0x01810000, 0x0181bfff, Ram, xlat_.data());
    map(0x0181c000, 0x0181cfff, Dev, nullptr, 0, false);
    map(0x01a00000, 0x01a03fff, Ram, comm_.data(), 0x10000);
    map(0x01a04000, 0x01a04fff, Dev, nullptr, 0x10000, false);
    map(0x01c00000, 0x01c00fff, Dev, nullptr, 0, false);
    map(0x01c80000, 0x01c80fff, Dev, nullptr, 0, false);
    map(0x01d00000, 0x01d03fff, Ram, backup_.data());
    if (img_.game->protection == Protection::Comp0229Doa) { // MAME model2_0229_mem: RAM with three registers in it
        prot_ram_.assign(0x8000, 0);
        map(0x01d80000, 0x01d87fff, Dev, nullptr, 0, false);
    }
#ifdef M2_DC_MEMORY
    map_rom(0x02000000, 0x03ffffff, RomRegion::MainData, 0);
    map_rom(0x06000000, 0x06ffffff, RomRegion::MainData, 0x1000000);
#else
    map(0x02000000, 0x03ffffff, Rom, img_.main_data.data());
    map(0x06000000, 0x06ffffff, Rom, img_.main_data.data() + 0x1000000);
#endif
    map(0x10000000, 0x105fffff, Dev, nullptr, 0, false);
#ifdef M2_DC_MEMORY
    // Frame buffer RAM is optional: Daytona never writes it (0 of 256 pages
    // in 9,000 attract frames and a whole race); without it the range is
    // unmapped, reading 0 as the zeroed RAM would.
    if (img_.frame_buffer_ram) {
        map(0x11600000, 0x1167ffff, Ram, img_.frame_buffer_ram);
        map(0x11680000, 0x116fffff, Ram, img_.frame_buffer_ram + 0x80000);
    }
#else
    map(0x11600000, 0x1167ffff, Ram, fb_a_.data());
    map(0x11680000, 0x116fffff, Ram, fb_b_.data());
#endif
#ifdef M2_DC_MEMORY
    map(0x12000000, 0x121fffff, Tex, img_.texture_ram, 0x200000);
    map(0x12400000, 0x125fffff, Tex, img_.texture_ram + 0x200000, 0x200000);
#else
    if (model2b()) {
        // MAME model2b_crx_mem: texture RAM as plain RAM, 1 MB a sheet mirrored
        // once, stored as 2A's tex0_w/tex1_w store it (ram_written tracks its
        // pages); luma RAM bytes (umask16 0x00ff), kept in 2A's layout (dev_r/w);
        // the SHARC's IOP port; the sound UART at 0x009c0000.
        map(0x11000000, 0x110fffff, Ram, tex0_.data(), 0x100000);
        map(0x11200000, 0x112fffff, Ram, tex1_.data(), 0x100000);
        map(0x11400000, 0x1140ffff, Dev, nullptr, 0, true);
        map(0x008c0000, 0x008c0fff, Dev, nullptr, 0, false);
        map(0x009c0000, 0x009c0fff, Dev, nullptr, 0, false);
    } else {
        map(0x12000000, 0x121fffff, Tex, tex0_.data(), 0x200000);
        map(0x12400000, 0x125fffff, Tex, tex1_.data(), 0x200000);
    }
#endif
    if (!model2b()) map(0x12800000, 0x1281ffff, Ram, luma_.data());

#ifdef M2_DC_MEMORY
    geo_ = std::make_unique<Geo>(*img_.rom, tgp_.buffer_data());
#else
    geo_ = std::make_unique<Geo>(img_.polygons, img_.textures, tgp_.buffer_data());
    // The geometrizer keeps its own copies: the images are not read again
    // (Dead or Alive's are 32 and 16 MB).
    img_.polygons = {};
    img_.textures = {};
#endif
    video_ = std::make_unique<Video>(tile_.data(), chr_.data());
    video_->enable_write_tracking();
}

#ifdef M2_DC_MEMORY
M2Board::Page M2Board::unmapped_;

M2Board::Page &M2Board::map_page(uint32_t addr) {
    auto &chunk = chunks_[addr >> 20];
    if (!chunk) chunk = std::make_unique<Page[]>(256);
    return chunk[(addr >> kPageBits) & 255];
}

void M2Board::map_rom(uint32_t start, uint32_t end, RomRegion region, uint32_t offset) {
    for (uint64_t a = start; a <= end; a += (1u << kPageBits)) {
        Page &p = map_page(uint32_t(a));
        p.kind = Rom;
        p.burst = true;
        p.base = nullptr;
        p.region = region;
        p.rom_offset = offset + uint32_t(a - start);
    }
}
#endif

void M2Board::map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror, bool burst) {
    for (uint32_t m = 0;; m = (m - mirror) & mirror) {
        for (uint64_t a = start; a <= end; a += (1u << kPageBits)) {
#ifdef M2_DC_MEMORY
            Page &p = map_page(uint32_t(a | m));
#else
            Page &p = pages_[uint32_t(a | m) >> kPageBits];
#endif
            p.kind = k;
            p.burst = burst;
            p.base = base ? base + (a - start) : nullptr;
        }
        if (((m - mirror) & mirror) == 0) break;
    }
}

void M2Board::attach(Cpu &cpu, Lockstep &ls) {
    cpu_ = &cpu;
    ls_ = &ls;
#ifdef M2_FAST_GEN
    cpu.work_ram = work_.data(); // plain RAM: nothing watches its writes (ram_written)
#endif
}

// --- interrupts (MAME irq_update) --------------------------------------------

void M2Board::irq_update() {
    const int8_t want[4] = {int8_t((intreq_ & 0x001) ? 1 : 0), int8_t((intreq_ & 0x002) ? 1 : 0),
                            int8_t((intreq_ & 0x3fc) ? 1 : 0), int8_t((intreq_ & 0xc00) ? 1 : 0)};
    for (int l = 0; l < 4; l++)
        if (want[l] != lines_[l]) {
            lines_[l] = want[l];
            cpu_->execute_set_input(l, want[l]);
            ls_->poke();
        }
}

void M2Board::vblank_start() {
    vblank_tick_ = ticks();
    // 60 Hz mode or an even frame: the geometrizer starts a new frame
    if ((videocontrol_ & 1) == 0 || (frame_ & 1) == 0) {
#ifdef M2_DC_SPEED
        // Draw mode: this frame's polygons are shown if it is drawn, or, in
        // 30 Hz mode (no parse next frame), if the next one is.
        if (frame_skip_) {
            const uint64_t n = uint64_t(frame_skip_ + 1);
            geo_->skip_objects = frame_ % n != 0 && ((videocontrol_ & 1) == 0 || (frame_ + 1) % n != 0);
        }
#endif
        geo_->zclip_w(zclip_);
        // The TGP runs alongside the i960 in MAME: by vblank it has written
        // to buffer RAM whatever its input asked for. Ours runs on demand, so
        // it catches up before the geometrizer reads the display list (Manx
        // TT has the TGP write display list words there, 19 Oct 2026).
        tgp_.sync();
        geo_->parse(tgp_.geo_read_start());
        video_->frame_start();
    }
    if (intena_ & 1) {
        intreq_ |= 1;
        irq_update();
    }
    if (comm_board_) comm_board_->vblank(); // MAME check_vint_irq
}

void M2Board::set_link(LinkTransport *transport, bool framesync) {
    if (!transport) {
        comm_board_.reset();
        return;
    }
    comm_board_ = std::make_unique<CommBoard>(comm_.data());
    comm_board_->set_transport(transport);
    comm_board_->set_framesync(framesync);
}

void M2Board::set_wide_margin(int pixels) {
    geo_->set_wide_margin(pixels);
    video_->set_wide_margin(pixels);
}

void M2Board::vblank_end() {
    if (frame_skip_ && frame_ % uint64_t(frame_skip_ + 1) != 0) { // draw mode: keep the last picture
        ++frame_;
        return;
    }
    VideoMem m;
    m.palram = palette_.data();
    m.colorxlat = xlat_.data();
    m.lumaram = luma_.data();
#ifdef M2_DC_MEMORY
    m.tex0 = reinterpret_cast<const uint32_t *>(img_.texture_ram);
    m.tex1 = reinterpret_cast<const uint32_t *>(img_.texture_ram + 0x200000);
#else
    m.tex0 = reinterpret_cast<const uint32_t *>(tex0_.data());
    m.tex1 = reinterpret_cast<const uint32_t *>(tex1_.data());
#endif
    m.tex_generation = tex_generation_;
    m.tex_page_generation = tex_page_generation_;
    video_->screen_update(geo_->polys, geo_->windows(), m);
    ++frame_;
}

bool M2Board::in_idle_loop() const {
    const uint32_t ip = cpu_->m_IP;
    for (const Game::Range &r : img_.game->idle)
        if (r.last && ip >= r.first && ip <= r.last) return true;
    return false;
}

// --- timers ------------------------------------------------------------------

void M2Board::timer_write(unsigned n, uint32_t data, uint32_t mask) {
    timervals_[n] = (timervals_[n] & ~mask) | (data & mask);
    Timer &t = timers_[n];
    t.orig = timervals_[n];
    t.start = ticks();
    t.run = true;
    timer_schedule(n);
}

// Expire when the instruction count reaches the timer's tick (the count
// that board time reaches it at, with no further idle skip). The previous
// expiry is cancelled, not left to lapse: it is rescheduled every frame (the
// idle skip moves board time) and on every reload, and most of a frame's
// board time is skipped, not run, so its count can be far ahead (10^9 with
// a long reload): stale ones piled up in the Lockstep log (30 k by frame
// 3,500 of srallyc, every insert shifting them). So the one scheduled is
// always current; [this, n] also fits std::function's inline storage (no
// allocation per reschedule on the Vita's 32-bit ARM).
void M2Board::timer_schedule(unsigned n) {
    Timer &t = timers_[n];
    if (t.expiry != kNoExpiry) ls_->cancel_callback(std::exchange(t.expiry, kNoExpiry));
    if (!t.run) return;
    const uint64_t due = t.start + t.orig, skip = std::min(due, tick_skip_);
    const uint64_t count = ((due - skip) * kInstrPerFrame + kTicksPerFrame - 1) / kTicksPerFrame;
    t.expiry = ls_->add_callback(std::max(count, io_now() + 1), [this, n] {
        timers_[n].expiry = kNoExpiry; // (its slot is free from here)
        timer_expire(n);
    });
}

void M2Board::timer_expire(unsigned n) { // model2_timer_cb
    Timer &t = timers_[n];
    if (!t.run) return;
    const uint32_t line = 1u << (n + 2);
    if (intena_ & line) {
        intreq_ |= line;
        irq_update();
    }
    timervals_[n] = 0xfffff;
    t.run = false;
}

bool M2Board::idle_until_vblank() {
    const uint64_t target = vblank_tick_ + kTicksPerFrame, now = ticks();
    int first = -1;
    uint64_t first_due = UINT64_MAX;
    for (unsigned n = 0; n < 4; n++) {
        const Timer &t = timers_[n];
        if (t.run && (intena_ & (1u << (n + 2))) && t.start + t.orig < first_due) {
            first = int(n);
            first_due = t.start + t.orig;
        }
    }
    const bool fire = first >= 0 && first_due <= target;
    const uint64_t to = fire ? first_due : target;
    if (to > now) tick_skip_ += to - now;
    if (fire) timer_expire(unsigned(first));
    for (unsigned n = 0; n < 4; n++) timer_schedule(n); // the skip moved their counts
    return fire;
}

// --- sound UART (i8251, transmit side) ------------------------------------------
// The game sends bytes from its IRQ3 handler whenever TxRDY is set. With no
// clock, a byte moves to the shifter and out at once; the sound runtime
// receives it immediately.

void M2Board::uart_txrdy(bool state) {
    if (state == uart_txrdy_) return;
    uart_txrdy_ = state;
    // MAME sound_ready_w: TxRDY (or RxRDY) sets request bit 10 if enabled
    if (state && (intena_ & (1u << 10))) {
        intreq_ |= 1u << 10;
        irq_update();
    }
}

void M2Board::uart_write_data(uint8_t v) {
    uart_hold_ = v;
    uart_have_hold_ = true;
    uart_txrdy(false);
    if (!uart_shift_busy_) {
        uart_shift_busy_ = true;
#ifdef M2_FAST_GEN
        ls_->add_callback(ls_->now() + 1, [this] { uart_shift_done(); }); // (now(): Lockstep::pending)
#else
        ls_->add_callback(ls_->count + 1, [this] { uart_shift_done(); });
#endif
    }
}

void M2Board::uart_shift_done() {
    uart_shift_busy_ = false;
    if (uart_have_hold_) {
        uart_out_.push_back(uart_hold_);
        uart_have_hold_ = false;
    }
    uart_txrdy(true);
}

void M2Board::receive_sound_bytes(const uint8_t *bytes, size_t n) {
    if (!n) return;
    uart_in_.insert(uart_in_.end(), bytes, bytes + n);
    // MAME sound_ready_w: RxRDY (or TxRDY) sets request bit 10 if enabled
    if (intena_ & (1u << 10)) {
        intreq_ |= 1u << 10;
        irq_update();
    }
}

// --- device registers --------------------------------------------------------

uint32_t M2Board::dev_read(uint32_t addr, uint32_t mask) {
    if (addr >= 0x00900000 && addr <= 0x0097ffff) {
        tgp_.sync_buffer_read(); // the TGP answers through its mailbox here (0x0091fff0-8)
        return tgp_.buffer_r(addr & 0x1ffff);
    }
    if (addr >= 0x00800000 && addr <= 0x00803fff) return tgp_.geo_r((addr - 0x00800000) >> 2);
    if (addr >= 0x00804000 && addr <= 0x00807fff) return 0xffffffffu; // geo_prg_r
    if (addr >= 0x00884000 && addr <= 0x00887fff) return tgp_.fifo_r();
    if (model2b()) {
        if (addr == 0x00980014) return tgp_.copro_status_r();
        if (addr >= 0x11400000 && addr <= 0x1140ffff) { // luma RAM: two entries a dword (bytes 0 and 2)
            const uint32_t k = (addr & 0xffff) >> 1;
            return uint32_t(luma_[k * 4]) | uint32_t(luma_[(k + 1) * 4]) << 16;
        }
        if (addr >= 0x009c0000 && addr <= 0x009c0007) // i8251, umask32 0x000000ff: data, then status
            return (mask & 0xff) ? (addr & 4 ? uart_status_r() : uart_data_r()) : 0;
    }
    switch (addr) {
    case 0x00980000: return tgp_.coproctl_r();
    case 0x00980004: {
        const uint32_t v = tgp_.fifo_out_empty() ? 1 : 0;
#ifndef M2_FAST_BUS // (a desktop debugging print: getenv on every status read)
        if (std::getenv("M2RUN_VERBOSE")) std::fprintf(stderr, "fifo status read -> %u (frame %llu)\n", v, (unsigned long long)frame_);
#endif
        return v;
    }
    case 0x0098000c: { // videoctl_r
        const uint32_t framenum = render_mode_ ? uint32_t((frame_ & 1) << 2) : uint32_t((frame_ & 2) << 1);
        return framenum | (videocontrol_ & 3);
    }
    case 0x00e80000: return intreq_;
    case 0x00e80004: return intena_;
    default: break;
    }
    if (addr >= 0x00980030 && addr <= 0x0098003f) { // tgpid_r, byte-wide
        static const uint8_t id[] = {0, 'T', 'A', 'H', 0, 'A', 'K', 'O', 0, 'Z', 'A', 'K', 0, 'M', 'T', 'K'};
        const uint32_t o = addr - 0x00980030;
        return uint32_t(id[o]) | uint32_t(id[o + 1]) << 8 | uint32_t(id[o + 2]) << 16 | uint32_t(id[o + 3]) << 24;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) { // timers_r: the current count while running
        const unsigned n = (addr >> 2) & 3;
        if (timers_[n].run) timervals_[n] = timers_[n].orig - uint32_t(ticks() - timers_[n].start);
        return timervals_[n];
    }
    if ((addr & ~0x10000u) == 0x01a04000) { // cn_r, fg_r
        if (comm_board_) // fg_r takes frames in: only when that lane is read
            return uint32_t(comm_board_->cn_r()) | ((mask & 0xff0000) ? uint32_t(comm_board_->fg_r()) << 16 : 0);
        return uint32_t(comm_cn_ | 0xfe) | uint32_t(comm_fg_) << 16;
    }
    if (addr >= 0x01c00000 && addr <= 0x01c00fff) { // MB8421 or 315-5649, through umask 0x00ff00ff
        const uint32_t i = ((addr & 0xfff) >> 2) * 2;
        if (crx()) {
            if (i >= 0x10) return 0;
            const uint64_t now = io_now();
            return ((mask & 0x000000ff) ? uint32_t(io_.crx_read(i, now)) : 0) |
                   ((mask & 0x00ff0000) ? uint32_t(io_.crx_read(i + 1, now)) << 16 : 0);
        }
        return uint32_t(io_.read(i)) | uint32_t(io_.read(i + 1)) << 16;
    }
    if (addr >= 0x01c80000 && addr <= 0x01c80003) { // i8251 (MAME read/status_r/data_r), umask16 0x00ff
        uint32_t v = 0;
        if (mask & 0x00ff0000) v |= uint32_t(uart_status_r()) << 16;
        if (mask & 0x000000ff) v |= uart_data_r();
        return v;
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) return uint32_t(render_unk_) << 14 | uint32_t(render_mode_) << 2 | uint32_t(render_test_);
    if (addr >= 0x10400000 && addr <= 0x105fffff) return uint32_t(geo_->polys.size()); // polygon_count_r
    if (addr >= 0x01d80000 && addr <= 0x01d87fff && !prot_ram_.empty()) return prot_read(addr, mask);
    return 0;
}

void M2Board::dev_write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (addr >= 0x00900000 && addr <= 0x0097ffff) { tgp_.buffer_w(addr & 0x1ffff, data, mask); return; }
    if (model2b()) {
        if (addr >= 0x008c0000 && addr <= 0x008c0fff) { tgp_.sharc_iop_w((addr & 0xfff) >> 2, data & mask); return; }
        if (addr >= 0x11400000 && addr <= 0x1140ffff) { // luma RAM, 2A's layout (one byte a dword)
            const uint32_t k = (addr & 0xffff) >> 1;
            if (mask & 0x000000ff) luma_[k * 4] = uint8_t(data);
            if (mask & 0x00ff0000) luma_[(k + 1) * 4] = uint8_t(data >> 16);
            video_->colour_tables_w();
            return;
        }
        if (addr >= 0x009c0000 && addr <= 0x009c0007) { // i8251: data at 0, control at 4 (lane 0)
            if ((mask & 0xff) && !(addr & 4)) uart_write_data(uint8_t(data));
            return;
        }
    }
    if (addr == 0x00980000) { tgp_.coproctl_w(data, mask); return; }
    const uint32_t d = data & mask; // handlers without a mask see unwritten lanes as 0
    if (addr >= 0x00884000 && addr <= 0x00887fff) { tgp_.fifo_w(d); return; }
    if (addr >= 0x00880000 && addr <= 0x00883fff) { tgp_.function_port_w((addr - 0x00880000) >> 2, d); return; }
    if (addr >= 0x00804000 && addr <= 0x00807fff) { tgp_.geo_prg_w(d); return; }
    if (addr >= 0x00800000 && addr <= 0x00803fff) { tgp_.geo_w((addr - 0x00800000) >> 2, d); return; }
    switch (addr) {
    case 0x00980008: tgp_.geoctl_w(d); return;
    case 0x0098000c: videocontrol_ = (videocontrol_ & ~mask) | (data & mask); return;
    case 0x00e80000: intreq_ &= data | ~mask; irq_update(); return; // irq_ack_w
    case 0x00e80004: {                                                // irq_enable_w (MAME delays 80 ns)
        intena_ = (intena_ & ~mask) | (data & mask);
        if ((uart_txrdy_ || !uart_in_.empty()) && (intena_ & (1u << 10))) intreq_ |= 1u << 10; // irq_mask_delayed_update
        irq_update();
        return;
    }
    case 0x0181c000: zclip_ = d; return;
    default: break;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) {
        // timers_w. Daytona reloads timer 0 each frame and never enables its
        // interrupt; Sega Rally runs its sound UART polling from timer 0 and 2.
        timer_write((addr >> 2) & 3, data, mask);
        return;
    }
    if ((addr & ~0x100000u) == 0x01040000) { if (mask & 0xffff) video_->xhout_w(uint16_t(data)); return; }
    if ((addr & ~0x100000u) == 0x01060000) { if (mask & 0xffff) video_->xvout_w(uint16_t(data)); return; }
    if ((addr & ~0x10000u) == 0x01a04000) {
        if (comm_board_) {
            if (mask & 0xff) comm_board_->cn_w(uint8_t(data));
            if (mask & 0xff0000) comm_board_->fg_w(uint8_t(data >> 16));
            return;
        }
        if (mask & 0xff) comm_cn_ = uint8_t(data & 1);
        if (mask & 0xff0000) comm_fg_ = uint8_t(data >> 16);
        return;
    }
    if (addr >= 0x01c00000 && addr <= 0x01c00fff) {
        const uint32_t i = ((addr & 0xfff) >> 2) * 2;
        if (crx()) { // 315-5649 at 0x01c00000-0x01c0001f; 0x01c00040 is written and ignored
            if (i >= 0x10) return;
            const uint64_t now = io_now();
            if (mask & 0x000000ff) io_.crx_write(i, uint8_t(data), now);
            if (mask & 0x00ff0000) io_.crx_write(i + 1, uint8_t(data >> 16), now);
            return;
        }
        if (mask & 0x000000ff) io_.write(i, uint8_t(data));
        if (mask & 0x00ff0000) io_.write(i + 1, uint8_t(data >> 16));
        return;
    }
    if (addr >= 0x01c80000 && addr <= 0x01c80003) { // i8251, umask16 0x00ff: lane 0 data, lane 2 control
        if (mask & 0x000000ff) uart_write_data(uint8_t(data));
        return; // mode/command bytes: transmitter enabled by the game at boot
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) { // render_mode_w
        render_test_ = data & 1;
        render_mode_ = (data >> 2) & 1;
        render_unk_ = (data >> 14) & 1;
        return;
    }
    if (addr >= 0x01d80000 && addr <= 0x01d87fff && !prot_ram_.empty()) { prot_write(addr, data, mask); return; }
}

// --- Dead or Alive's 317-0229 (MAME model2_0229_mem, sega_315_5838_comp_device
// in HACK_MODE_DOA) ------------------------------------------------------------
// The chip's cipher is not known: the game only checks one string it decodes
// at boot (MAME: PC 0x2c20 passes, 0x2c28 fails), so the chip answers that
// string from the source offset the game sets, as MAME does. The table uploads
// (0x01d87ff4) change nothing in that mode; the source data the game leaves in
// the RAM window is not read.

// MAME get_decompressed_byte, HACK_MODE_DOA.
uint8_t M2Board::prot_byte() {
    static const char kString[] = "  TECMO LTD.  DEAD OR ALIVE  1996.10.22  VER. 1.00"; // 50 bytes
    const uint8_t v = prot_src_ < 50 ? uint8_t(kString[prot_src_]) : 0;
    prot_src_ = (prot_src_ + 1) & 0x007fffff;
    return v;
}

uint32_t M2Board::prot_read(uint32_t addr, uint32_t mask) {
    if (addr == 0x01d87ff8) { // doa_prot_r: 16 bits a data_r, two bytes, high first; the upper lane first
        uint32_t v = 0;
        if (mask & 0xffff0000) { const uint32_t hi = prot_byte(); v |= (hi << 8 | prot_byte()) << 16; }
        if (mask & 0x0000ffff) { const uint32_t hi = prot_byte(); v |= hi << 8 | prot_byte(); }
        return v;
    }
    if (addr == 0x01d8400c) { // doa_unk_r: a busy flag that toggles at every read
        prot_busy_ = !prot_busy_;
        return prot_busy_ ? 0xffff : 0xfff0;
    }
    uint32_t v;
    std::memcpy(&v, &prot_ram_[addr & 0x7ffc], 4);
    return v;
}

void M2Board::prot_write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (addr == 0x01d87ff0) { // srcaddr_w (COMBINE_DATA)
        prot_src_ = ((prot_src_ & ~mask) | (data & mask)) & 0x007fffff;
        return;
    }
    if (addr == 0x01d87ff4) return; // data_w_doa: the decoder's tables, unused in this mode
    uint32_t v;
    std::memcpy(&v, &prot_ram_[addr & 0x7ffc], 4);
    v = (v & ~mask) | (data & mask);
    std::memcpy(&prot_ram_[addr & 0x7ffc], &v, 4);
}

// The i8251's status (DSR unconnected: 1, TxEMPTY, RxRDY, TxRDY: the data
// buffer empty) and data (reading takes the received byte; RxRDY clears when
// none is left), MAME's read/status_r/data_r.
uint8_t M2Board::uart_status_r() const {
    const bool tx_empty = !uart_have_hold_ && !uart_shift_busy_;
    return uint8_t(0x80 | (tx_empty ? 4 : 0) | (uart_in_.empty() ? 0 : 2) | (uart_have_hold_ ? 0 : 1));
}

uint8_t M2Board::uart_data_r() {
    if (uart_in_.empty()) return 0;
    const uint8_t v = uart_in_.front();
    uart_in_.pop_front();
    return v;
}

// Palette and colour translation live in RAM pages; the video output tracks
// what MAME's handlers do on each write.
void M2Board::ram_written(uint32_t addr, uint32_t data, uint32_t mask) {
    (void)data;
    if ((addr >= 0x01000000 && addr <= 0x0100ffff) ||
        (addr >= 0x01110000 && addr <= 0x0111ffff)) video_->tile_memory_w(addr & 0xffff);
    else if ((addr >= 0x01080000 && addr <= 0x010fffff) ||
             (addr >= 0x01180000 && addr <= 0x011fffff)) video_->character_memory_w(addr & 0x7ffff);
    if (addr >= 0x01800000 && addr <= 0x01803fff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->palette_w(((addr & 0x3fff) >> 1) + lane, palette_.data(), xlat_.data());
        if (addr >= 0x01802000 && addr <= 0x018027ff) video_->colour_tables_w(); // 3D colours
    } else if (addr >= 0x01810000 && addr <= 0x0181bfff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->colorxlat_w(((addr - 0x01810000) >> 1) + lane);
        video_->colour_tables_w();
    } else if (addr >= 0x12800000 && addr <= 0x1281ffff) {
        video_->colour_tables_w(); // luma RAM
    } else if (addr >= 0x11000000 && addr <= 0x113fffff) {
        // Model 2B texture RAM (plain RAM): its page's generation, as tex_write's.
        ++tex_generation_;
        const uint32_t sheet = (addr >> 21) & 1, stored = addr & 0xfffff;
        tex_page_generation_[sheet * VideoMem::kTexPages + (stored >> 12)] = tex_generation_;
    }
}

void M2Board::tex_write(const Page &p, uint32_t addr, uint32_t lane_data) {
    uint8_t *const base = p.base - ((addr & 0x1fffff) & ~((1u << kPageBits) - 1));
    const uint32_t o = (addr & 0x1fffff) >> 2;
    uint8_t *const w = base + (o >> 1) * 4 + (o & 1) * 2;
    w[0] = uint8_t(lane_data);
    w[1] = uint8_t(lane_data >> 8);
    ++tex_generation_; // the hardware renderer re-uploads texture RAM when this moves
#ifdef M2_DC_MEMORY
    const uint8_t *const sheet0 = img_.texture_ram;
#else
    const uint8_t *const sheet0 = tex0_.data();
#endif
    const uint32_t sheet = base == sheet0 ? 0 : 1, stored = (o >> 1) * 4 + (o & 1) * 2;
    tex_page_generation_[sheet * VideoMem::kTexPages + (stored >> 12)] = tex_generation_;
}

// --- bus ---------------------------------------------------------------------

uint32_t M2Board::fetch(uint32_t addr) {
    const Page &p = page(addr);
    if (p.kind != Rom && p.kind != Ram) throw Fatal("instruction fetch from a device");
    uint32_t v;
#ifdef M2_DC_MEMORY
    if (p.kind == Rom) {
        std::memcpy(&v, M2_AL(rom_page(p) + (addr & 0xffc), 4), 4);
        return v;
    }
#endif
    std::memcpy(&v, M2_AL(p.base + (addr & 0xffc), 4), 4);
    return v;
}

#ifdef M2_I960_PROFILE
void M2Board::prof_bus(uint32_t addr, bool write) {
    I960BusRegion r;
    if ((addr & 0xffff8000u) == 0x00880000u || (addr & ~3u) == 0x00980004u || addr - 0x00900000u < 0x80000u) r = BusTgp;
    else switch (page(addr).kind) {
        case Ram: r = addr - 0x00500000u < 0x100000u ? BusWorkRam : BusRam; break;
        case Rom: r = BusRom; break;
        case Tex: r = BusTex; break;
        case Dev: r = BusDev; break;
        default: r = BusUnmapped; break;
        }
    ++(write ? i960_profile.bus_w : i960_profile.bus_r)[r];
}
#endif

#ifdef M2_SPIN_SKIP
// A wait loop of the game's (Game::SpinWait), at its pass's first read: `value` is
// what it reads. Each pass reads the same RAM byte and, while it loops, leaves
// nothing behind its next pass does not overwrite (its registers and flags are
// written before they are read); nothing changes the byte until the next
// lockstep event (an interrupt, a callback: a timer's expiry among them). So
// when this pass will loop, the passes up to just before that event are
// skipped: the count moves on by whole passes, to the state those passes
// would have left, and this read stands for the read of the first pass not
// skipped. This read is under way (its boundary passed at `count`), so the
// last skipped pass ends at a count below the event's. With a timer, a pass
// also leaves when the timer's count read `timer_at` instructions into it
// allows: that count is the dev_read expression at that instruction count,
// falling while the timer runs (its expiry is an event), so the skip stops
// before the first pass that would leave.
void M2Board::spin_skip(const Game::SpinWait &w, uint32_t value) {
    switch (w.kind) {
    case Game::SpinWait::ByteIsReg: if (uint8_t(value) != uint8_t(cpu_->m_r[w.reg])) return; break;
    case Game::SpinWait::WordBitsCount: if ((value & w.mask) != cpu_->m_r[w.reg]) return; break;
    case Game::SpinWait::WordZeroCount: if (value != 0) return; break;
    default: if (uint8_t(value) != 0) return; break;
    }
    const uint64_t limit = std::min(ls_->next_count, ls_->end_count), count = io_now();
    if (limit <= count + w.pass) return;
    uint64_t passes = (limit - 1 - count) / w.pass;
    if (w.kind == Game::SpinWait::ByteZeroTimer) {
        const Timer &t = timers_[w.timer];
        const uint32_t threshold = cpu_->m_r[w.reg];
        auto read = [&](uint64_t pass) { // the timer as pass `pass` reads it (dev_read's timers_r)
            if (!t.run) return timervals_[w.timer];
            const uint64_t at = count + pass * w.pass + w.timer_at;
            return t.orig - uint32_t(at * kTicksPerFrame / kInstrPerFrame + tick_skip_ - t.start);
        };
        auto loops = [&](uint64_t pass) {
            const uint32_t m = read(pass) & 0xfffffu;
            return m != 0xfffffu && m > threshold;
        };
        // The 20 bits fall with the pass while the bits above them do not change.
        if ((read(0) ^ read(passes - 1)) >> 20) return;
        if (!loops(0)) return;
        if (!loops(passes - 1)) { // the first pass that leaves: passes before it are skipped
            uint64_t lo = 1, hi = passes - 1;
            while (lo < hi) {
                const uint64_t mid = lo + (hi - lo) / 2;
                if (loops(mid)) lo = mid + 1;
                else hi = mid;
            }
            passes = lo;
        }
    }
    const uint64_t skip = passes * w.pass;
    if (w.kind == Game::SpinWait::WordBitsCount || w.kind == Game::SpinWait::WordZeroCount)
        cpu_->m_r[w.counter] += uint32_t(passes); // what those passes counted
    ls_->count += skip;
    spin_skipped_ += skip;
#ifdef M2_FAST_GEN
    ++ls_->epoch; // the count jumped: the fast generated code checks in full next
#endif
}
#endif

uint8_t M2Board::read_byte(uint32_t addr) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, false);
#endif
#ifdef M2_FAST_BUS
#ifdef M2_SPIN_SKIP
    if ((addr >> kPageBits) == fast_read_page_ && !spin_addr(addr))
        return fast_read_base_[addr & 0xfff];
#else
    if ((addr >> kPageBits) == fast_read_page_) return fast_read_base_[addr & 0xfff];
#endif
#endif
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
#ifdef M2_SPIN_SKIP
    if (p.kind == Ram)
        for (const Game::SpinWait &w : spin_)
            if ((w.kind == Game::SpinWait::ByteIsReg || w.kind == Game::SpinWait::ByteZeroTimer) && addr == w.addr && cpu_->m_IP == w.ip) {
                const uint8_t v = p.base[addr & 0xfff];
                spin_skip(w, v);
                return v;
            }
#endif
#ifdef M2_DC_MEMORY
    if (p.kind == Rom) return rom_page(p)[addr & 0xfff];
#endif
    switch (p.kind) {
#ifdef M2_FAST_BUS
    case Ram: case Tex: fast_read(addr, p); return p.base[addr & 0xfff];
    case Rom: return p.base[addr & 0xfff];
#else
    case Rom: case Ram: case Tex: return p.base[addr & 0xfff];
#endif
    case Dev: return uint8_t(dev_read(addr & ~3u, 0xffu << sh) >> sh);
    default: return 0;
    }
}

uint16_t M2Board::read_word(uint32_t addr) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, false);
#endif
    addr &= ~1u;
#ifdef M2_FAST_BUS
    if ((addr >> kPageBits) == fast_read_page_) {
        uint16_t v;
        std::memcpy(&v, M2_AL(fast_read_base_ + (addr & 0xfff), 2), 2);
        return v;
    }
#endif
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
#ifdef M2_DC_MEMORY
    if (p.kind == Rom) {
        uint16_t v;
        std::memcpy(&v, M2_AL(rom_page(p) + (addr & 0xfff), 2), 2);
        return v;
    }
#endif
    switch (p.kind) {
#ifdef M2_FAST_BUS
    case Ram: case Tex: fast_read(addr, p); [[fallthrough]];
    case Rom: {
#else
    case Rom: case Ram: case Tex: {
#endif
        uint16_t v;
        std::memcpy(&v, M2_AL(p.base + (addr & 0xfff), 2), 2);
        return v;
    }
    case Dev: return uint16_t(dev_read(addr & ~3u, 0xffffu << sh) >> sh);
    default: return 0;
    }
}

uint32_t M2Board::read_dword(uint32_t addr) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, false);
#endif
    addr &= ~3u;
#ifdef M2_FAST_BUS
    // The TGP's FIFO, status and buffer RAM (dev_read's answers), without the
    // page table and the dispatch chain: tens of millions of reads a race.
    if ((addr & 0xffffc000u) == 0x00884000u) return tgp_.fifo_r();
    if (addr == 0x00980004u) return tgp_.fifo_out_empty() ? 1 : 0;
    if (addr - 0x00900000u < 0x80000u) {
        tgp_.sync_buffer_read();
        return tgp_.buffer_r(addr & 0x1ffff);
    }
#endif
#ifdef M2_FAST_BUS
#ifdef M2_SPIN_SKIP
    if ((addr >> kPageBits) == fast_read_page_ && !spin_addr(addr)) {
#else
    if ((addr >> kPageBits) == fast_read_page_) {
#endif
        uint32_t v;
        std::memcpy(&v, M2_AL(fast_read_base_ + (addr & 0xfff), 4), 4);
        return v;
    }
#endif
    const Page &p = page(addr);
#ifdef M2_DC_MEMORY
    if (p.kind == Rom) {
        uint32_t v;
        std::memcpy(&v, M2_AL(rom_page(p) + (addr & 0xfff), 4), 4);
        return v;
    }
#endif
#ifdef M2_SPIN_SKIP
    if (spin_addr(addr) && p.kind == Ram) // a work RAM word's wait loop (WordZeroCount)
        for (const Game::SpinWait &w : spin_)
            if (w.kind == Game::SpinWait::WordZeroCount && addr == w.addr && cpu_->m_IP == w.ip) {
                uint32_t v;
                std::memcpy(&v, M2_AL(p.base + (addr & 0xfff), 4), 4);
                spin_skip(w, v);
                return v;
            }
#endif
    switch (p.kind) {
#ifdef M2_FAST_BUS
    case Ram: case Tex: fast_read(addr, p); [[fallthrough]];
    case Rom: {
#else
    case Rom: case Ram: case Tex: {
#endif
        uint32_t v;
        std::memcpy(&v, M2_AL(p.base + (addr & 0xfff), 4), 4);
        return v;
    }
    case Dev: {
        const uint32_t v = dev_read(addr, 0xffffffffu);
#ifdef M2_SPIN_SKIP
        for (const Game::SpinWait &w : spin_) // a device register's wait loop (WordBitsCount)
            if (w.kind == Game::SpinWait::WordBitsCount && addr == w.addr && cpu_->m_IP == w.ip) spin_skip(w, v);
#endif
        return v;
    }
    default: return 0;
    }
}

void M2Board::write_byte(uint32_t addr, uint8_t data) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, true);
#endif
#ifdef M2_FAST_BUS
    if ((addr >> kPageBits) == fast_write_page_) {
        fast_write_base_[addr & 0xfff] = data;
        return;
    }
#endif
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Ram: {
#ifdef M2_FAST_BUS
        fast_write(addr, p);
#endif
        uint8_t &dst = p.base[addr & 0xfff];
        const bool changed = dst != data;
        dst = data;
        if (changed) ram_written(addr & ~3u, uint32_t(data) << sh, 0xffu << sh);
        return;
    }
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffu << sh); return;
    default: return;
    }
}

void M2Board::write_word(uint32_t addr, uint16_t data) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, true);
#endif
    addr &= ~1u;
#ifdef M2_FAST_BUS
    if ((addr >> kPageBits) == fast_write_page_) {
        std::memcpy(M2_AL(fast_write_base_ + (addr & 0xfff), 2), &data, 2);
        return;
    }
#endif
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Ram: {
#ifdef M2_FAST_BUS
        fast_write(addr, p);
#endif
        uint16_t old; std::memcpy(&old, M2_AL(p.base + (addr & 0xfff), 2), 2);
        std::memcpy(M2_AL(p.base + (addr & 0xfff), 2), &data, 2);
        if (old != data) ram_written(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh);
        return;
    }
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh); return;
    default: return;
    }
}

void M2Board::write_dword(uint32_t addr, uint32_t data) {
#ifdef M2_I960_PROFILE
    prof_bus(addr, true);
#endif
    addr &= ~3u;
#ifdef M2_FAST_BUS
    // The TGP's function port and FIFO (dev_write's, all lanes written),
    // without the page table and the dispatch chain.
    if ((addr & 0xffff8000u) == 0x00880000u) {
        if (addr & 0x4000) tgp_.fifo_w(data);
        else tgp_.function_port_w((addr - 0x00880000u) >> 2, data);
        return;
    }
#endif
#ifdef M2_FAST_BUS
    if ((addr >> kPageBits) == fast_write_page_) {
        std::memcpy(M2_AL(fast_write_base_ + (addr & 0xfff), 4), &data, 4);
        return;
    }
#endif
    const Page &p = page(addr);
    switch (p.kind) {
    case Ram: {
#ifdef M2_FAST_BUS
        fast_write(addr, p);
#endif
        uint32_t old; std::memcpy(&old, M2_AL(p.base + (addr & 0xfff), 4), 4);
        std::memcpy(M2_AL(p.base + (addr & 0xfff), 4), &data, 4);
        if (old != data) ram_written(addr, data, 0xffffffffu);
        return;
    }
    case Tex: tex_write(p, addr, data); return;
    case Dev: dev_write(addr, data, 0xffffffffu); return;
    default: return;
    }
}

} // namespace rt
