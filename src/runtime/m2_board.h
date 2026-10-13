// The Model 2 board as the recompiled game runs on it outside lockstep: the
// whole i960 memory map with every device answered natively (no trace, no
// MAME, no emulated CPUs). Frame pacing is the only clock: the game's native
// code runs until it waits for vblank (its idle loop), then the board starts
// vblank: the geometrizer parses the display list, the vblank interrupt is
// raised, and at vblank end the screen is composed.
//
// Devices, each native: RAM/ROM and texture RAM (as the replay bus), the TGP
// and geometry ports (TgpBoard, recompiled TGP), the geometrizer (Geo), the
// screen (Video: tilemaps, palette, 3D layer), the interrupt controller and
// timers, video/render mode registers, the I/O board (IoBoard: its dual-port
// RAM mailbox protocol, inputs, settings EEPROM), the sound UART (bytes to
// the sound runtime), the comm board's shared RAM and backup RAM.
#pragma once

#include "runtime/cpu.h"
#include "runtime/geo.h"
#include "runtime/lockstep.h"
#include "runtime/game.h"
#include "runtime/m2_tgp_board.h"
#include "runtime/video.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "runtime/comm_board.h"

namespace rt {

// The game's inputs, settings EEPROM and drive board commands, on either
// board. Model 2 (original): the Model 1 I/O board's side of the MB8421
// dual-port RAM (2 KB, read/write): the game writes a command to byte 0x20
// and the board answers. 1: latch the inputs into bytes 0-10. 3: copy the
// 128-byte settings EEPROM to bytes 0x100-0x17f. 2: store bytes 0x100-0x17f
// to the EEPROM. The byte returns to 0 when done. Byte 0x11 is the force
// feedback drive board's command (rt::DriveBoard): each byte written is
// queued for the host (found by logging the game's writes: it carries the
// drive board's command set).
//
// Model 2A-CRX: the 315-5649 I/O chip on the bus (crx_read/crx_write; MAME
// sega_315_5649_device and model2a_state's port callbacks): ports A-G,
// direction register, analog mux. Port A drives the 93C46 settings EEPROM
// (64 x 16 bits, the same 128 bytes as `eeprom`, word n in bytes 2n-2n+1
// little-endian) and the input mode; port B reads IN0 (or, in that mode,
// the EEPROM's data out), C IN1, D IN2, G the DIP switches; port E is the
// drive board's command (queued as above), F the lamps; analog channels 0-2
// are the wheel, accelerator and brake.
//
// Which of the game's controls each port and analog channel carries is the
// game's (Game::io, src/games/).
class IoBoard {
public:
    uint8_t read(uint32_t index) const { return ram_[index & 0x7ff]; }
    void write(uint32_t index, uint8_t v);
    // 315-5649 registers 0x00-0x0f. `now`: i960 instructions so far (the
    // EEPROM's write cycle time, which the game polls by reading).
    uint8_t crx_read(uint32_t offset, uint64_t now);
    void crx_write(uint32_t offset, uint8_t v, uint64_t now);
    Inputs inputs;
    CrxInputs wiring{}; // Model 2A-CRX: the game's ports and analog channels (Game::io)
    std::array<uint8_t, 128> eeprom;
    bool eeprom_dirty = false;
    std::vector<uint8_t> drive_commands; // written to byte 0x11 (2A: port E) since last taken (at most kMaxDrive kept)
    static constexpr size_t kMaxDrive = 256;
    IoBoard() { ram_.fill(0); eeprom.fill(0xff); }

private:
    std::array<uint8_t, 0x800> ram_;

    // 315-5649
    uint8_t port_[7] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t port_config_ = 0xff, mode_ = 0, analog_channel_ = 0;
    bool ctrl_mode_ = false; // port A bit 0: port B reads the EEPROM
    uint8_t port_in(uint32_t offset, uint64_t now);
    void eeprom_pins(uint8_t port_a, uint64_t now);

    // 93C46 (MAME eeprom_serial_93cxx_device, 16-bit, 6 address bits)
    enum class EeState : uint8_t { Reset, WaitStart, Command, Reading, WaitData, Completion };
    EeState ee_state_ = EeState::Reset;
    bool ee_cs_ = false, ee_clk_ = false, ee_di_ = false, ee_locked_ = true;
    uint32_t ee_bits_ = 0, ee_accum_ = 0, ee_shift_ = 0;
    uint8_t ee_command_ = 0, ee_address_ = 0;
    uint64_t ee_done_at_ = 0, ee_now_ = 0; // ee_now_: the time of the pin write being handled
    bool ee_ready(uint64_t now) const { return now >= ee_done_at_; }
    bool ee_do(uint64_t now) const;
    uint16_t ee_word(uint32_t a) const { return uint16_t(eeprom[a * 2] | eeprom[a * 2 + 1] << 8); }
    void ee_set_word(uint32_t a, uint16_t v);
    void ee_clock();
};

class M2Board : public Bus {
public:
    struct Images {
        std::vector<uint8_t> program, main_data, copro_tables, copro_data, polygons, textures;
        std::vector<uint8_t> sound_program, pcm1, pcm2; // sound board (68000 program, MultiPCM samples)
        const Game *game = &rt::game();                   // the game these images are for (and its board)
#ifdef M2_DC_MEMORY
        // The Dreamcast: program, main_data, polygons, textures and copro_data
        // are read through rom (those images stay empty); texture RAM (tex0
        // then tex1, 2 MB each; the game's writes only reach the first 1 MB
        // of each) and frame buffer RAM (A then B, 512 KB each; optional, the
        // game does not write it) are the frontend's, in video RAM.
        RomSource *rom = nullptr;
        uint8_t *texture_ram = nullptr, *frame_buffer_ram = nullptr;
#endif
    };
    explicit M2Board(Images images);

    // The CPU and scheduler (free-run Lockstep) the board raises interrupts on.
    void attach(Cpu &cpu, Lockstep &ls);

    // Bus
    uint32_t fetch(uint32_t addr) override;
    uint8_t read_byte(uint32_t addr) override;
    uint16_t read_word(uint32_t addr) override;
    uint32_t read_dword(uint32_t addr) override;
    void write_byte(uint32_t addr, uint8_t data) override;
    void write_word(uint32_t addr, uint16_t data) override;
    void write_dword(uint32_t addr, uint32_t data) override;
    uint16_t flags(uint32_t addr) override { return page(addr).burst ? Cpu::BURST : 0; }

    // Frame control, called by the runner.
    void vblank_start();       // MAME screen_vblank: geometrizer, vblank IRQ
    void vblank_end();         // screen_update
    bool in_idle_loop() const; // the game is waiting for vblank
    uint64_t frame() const { return frame_; }

    Video &video() { return *video_; }
    // Widescreen (enhancement): pixels added to each side; 0 = the original screen.
    void set_wide_margin(int pixels);
    // Draw mode (enhancement): draw the screen every (1 + skip)th frame; the
    // frames between keep the last picture. 0 = every frame, as the game does
    // (double buffered, a new 3D picture each frame); 1 = every 2nd; 2 = every
    // 3rd. The game logic and the geometrizer still run every frame.
#ifdef M2_DC_SPEED
    // The Dreamcast draws every 4th frame: 3 is allowed too.
    void set_frame_skip(int skip) { frame_skip_ = skip < 0 ? 0 : skip > 3 ? 3 : skip; }
#else
    void set_frame_skip(int skip) { frame_skip_ = skip < 0 ? 0 : skip > 2 ? 2 : skip; }
#endif
    IoBoard &io() { return io_; }
    TgpBoard &tgp() { return tgp_; }
    // Bytes sent to the sound board since the last take.
    std::vector<uint8_t> take_sound_bytes() { sound_total_ += uart_out_.size(); return std::exchange(uart_out_, {}); }
    uint64_t sound_bytes_total() const { return sound_total_ + uart_out_.size(); }
    std::vector<uint8_t> &backup_ram() { return backup_; }
    // Link play: the communication board on a host transport (CommBoard;
    // nullptr: no link, the registers stay plain). Set before the game runs.
    void set_link(LinkTransport *transport, bool framesync = false);
    const CommBoard *comm_board() const { return comm_board_.get(); }
#ifdef M2_DC_SPIN_SKIP
    uint64_t spin_skipped() const { return spin_skipped_; } // instructions skipped in the frame wait (read_byte)
#endif

private:
    enum Kind : uint8_t { Unmapped, Rom, Ram, Tex, Dev };
    struct Page {
        Kind kind = Unmapped;
        bool burst = false;
        uint8_t *base = nullptr;
#ifdef M2_DC_MEMORY
        RomRegion region = RomRegion::Program; // Rom pages: read through img_.rom, base is null
        uint32_t rom_offset = 0;
#endif
    };
    static constexpr unsigned kPageBits = 12;
#ifdef M2_DC_MEMORY
    // Two levels: 1 MB chunks of 256 pages, allocated when mapped (one table
    // entry per 4 KB of the 4 GB space is 12 MB here).
    Page &page(uint32_t addr) {
        auto &chunk = chunks_[addr >> 20];
        return chunk ? chunk[(addr >> kPageBits) & 255] : unmapped_;
    }
    Page &map_page(uint32_t addr);
    void map_rom(uint32_t start, uint32_t end, RomRegion region, uint32_t offset);
    const uint8_t *rom_page(const Page &p) { return img_.rom->page_fast(p.region, p.rom_offset >> kPageBits); }
#else
    Page &page(uint32_t addr) { return pages_[addr >> kPageBits]; }
#endif
    void map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror = 0, bool burst = true);

    // Device dword access: data in its byte lanes, mask = lanes accessed.
    uint32_t dev_read(uint32_t addr, uint32_t mask);
    void dev_write(uint32_t addr, uint32_t data, uint32_t mask);
    void ram_written(uint32_t addr, uint32_t data, uint32_t mask); // video registers kept in RAM pages
    void tex_write(const Page &p, uint32_t addr, uint32_t lane_data);

    bool crx() const { return img_.game->board == BoardType::Model2a; }
    uint64_t io_now() const { // i960 instructions so far, for the 315-5649's EEPROM
#ifdef M2_FAST_GEN
        return ls_->now();
#else
        return ls_->count;
#endif
    }
    void irq_update();
    void uart_write_data(uint8_t v);
    void uart_txrdy(bool state);

    Images img_;
    std::vector<uint8_t> ram_, work_, cpuctl_, backup_, tile_, chr_, palette_, xlat_, tex0_, tex1_, luma_, fb_a_, fb_b_,
        comm_;
#ifdef M2_DC_MEMORY
    std::vector<std::unique_ptr<Page[]>> chunks_;
    static Page unmapped_; // what page() gives outside the map; never written
#else
    std::vector<Page> pages_;
#endif

    TgpBoard tgp_;
    std::unique_ptr<Geo> geo_;
    std::unique_ptr<Video> video_;
    IoBoard io_;
    std::unique_ptr<CommBoard> comm_board_; // link play only
    Cpu *cpu_ = nullptr;
    int frame_skip_ = 0;
    uint64_t tex_generation_ = 0; // texture RAM writes so far
    Lockstep *ls_ = nullptr;

    uint32_t intreq_ = 0, intena_ = 0;
    int8_t lines_[4] = {-1, -1, -1, -1};
    uint32_t timervals_[4] = {0, 0, 0, 0};
    uint32_t videocontrol_ = 0, zclip_ = 0;
    bool render_mode_ = false, render_test_ = false, render_unk_ = false;
    uint8_t comm_cn_ = 0, comm_fg_ = 0;
    uint64_t frame_ = 0;
#ifdef M2_DC_SPIN_SKIP
    uint64_t spin_skipped_ = 0;
#endif
#ifdef M2_FAST_BUS
    // The last page read from plain memory (RAM, texture RAM) and the last
    // plain RAM page written (no video register behind it: ram_written does
    // nothing there), straight through their base. The map never changes
    // after the constructor, so neither goes stale.
    uint32_t fast_read_page_ = ~0u, fast_write_page_ = ~0u;
    uint8_t *fast_read_base_ = nullptr, *fast_write_base_ = nullptr;
    static bool plain_ram(uint32_t addr) {
        return !((addr >= 0x01000000 && addr <= 0x011fffff) || (addr >= 0x01800000 && addr <= 0x0181bfff));
    }
    void fast_read(uint32_t addr, const Page &p) { fast_read_page_ = addr >> kPageBits, fast_read_base_ = p.base; }
    void fast_write(uint32_t addr, const Page &p) {
        if (plain_ram(addr)) fast_write_page_ = addr >> kPageBits, fast_write_base_ = p.base;
    }
#endif

    // i8251 UART to the sound board (transmit side)
    bool uart_txrdy_ = true, uart_shift_busy_ = false, uart_have_hold_ = false;
    uint8_t uart_hold_ = 0;
    std::vector<uint8_t> uart_out_;
    uint64_t sound_total_ = 0;
    void uart_shift_done();
};

} // namespace rt
