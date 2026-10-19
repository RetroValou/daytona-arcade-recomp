#include "runtime/board/game_loop.h"

#include "runtime/cpu/i960_profile.h"
#include "runtime/sound/scsp_board.h"
#include "runtime/sound/model1_board.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace rt {

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path + " (run the setup / importer first)");
    return {std::istreambuf_iterator<char>(f), {}};
}

constexpr uint64_t kProbe = 1024;       // instructions between wait-loop checks
constexpr uint64_t kFrameCap = 110000;  // a CPU-bound frame: 25 MHz / 57.52 Hz of i960 work, as MAME measures
constexpr uint64_t kVblankCap = 40000;  // a vblank handler that never returns to the wait loop
constexpr uint64_t kMinFrame = kProbe * 2;
} // namespace

GameLoop::GameLoop(const std::string &dir, bool sound_enabled) : GameLoop([&] {
    M2Board::Images img;
    img.program = load(dir + "/program.bin");
    img.main_data = load(dir + "/main_data.bin");
    img.copro_tables = load(dir + "/copro_tables.bin");
    img.copro_data = load(dir + "/copro_data.bin");
    img.polygons = load(dir + "/polygons.bin");
    img.textures = load(dir + "/textures.bin");
    img.sound_program = load(dir + "/sound_program.bin");
    img.pcm1 = load(dir + "/pcm1.bin");
    img.pcm2 = load(dir + "/pcm2.bin");
    if (game().sound == SoundBoardType::Scsp) img.samples = load(dir + "/samples.bin");
    if (std::filesystem::exists(dir + "/eeprom.bin")) img.eeprom = load(dir + "/eeprom.bin"); // the set's default settings
    return img;
}(), sound_enabled) {}

GameLoop::GameLoop(M2Board::Images img, bool sound_enabled) {
#ifdef M2_INLINE_BUS
    // Code generated with m2recomp --board inlines that board's bus (gen::BoardMem):
    // it must run on that board, with the whole program ROM and data ROM in
    // memory (as the TGP program's CRC is checked before it runs).
    if (gen::inline_board >= 0 &&
        (gen::inline_board != int(img.game->board) || img.program.size() != 0x200000 || img.main_data.size() < 0x2000000))
        throw Fatal("i960 code generated for another board (m2recomp --board) or ROM images of the wrong size");
#endif
    if (sound_enabled && !img.sound_program.empty()) {
        if (img.game->sound == SoundBoardType::Scsp)
            sound_ = std::make_unique<snd::ScspBoard>(img.sound_program, img.samples, img.game->scsp_voices, img.game->music_channels);
        else
            sound_ = std::make_unique<snd::SoundBoard>(img.sound_program, img.pcm1, img.pcm2, img.game->model1_voices,
                                                       img.game->music_channels);
    }
    board_ = std::make_unique<M2Board>(std::move(img));
    cpu_ = std::make_unique<Cpu>(board_.get());
    ls_ = std::make_unique<Lockstep>(*cpu_);
    board_->attach(*cpu_, *ls_);
    cpu_->reset();
    env_ = std::make_unique<gen::Env>(gen::Env{*cpu_, *ls_});
    ls_->add_callback(kProbe, [this] { probe(); });
}

void GameLoop::probe() {
    const bool idle = board_->in_idle_loop();
    if (in_vblank_) {
        if ((idle && ls_->count - vblank_start_ >= kProbe * 2) || ls_->count - vblank_start_ >= kVblankCap) {
            {
                auto sample = profiler_.measure(profiler_.frame.video);
                board_->vblank_end();
            }
            in_vblank_ = false;
            frame_start_ = ls_->count;
            ++frames_;
            frame_done_ = true;
#ifdef M2_FAST_GEN
            ls_->set_end(ls_->count); // gen::run returns to the caller
#else
            ls_->end_count = ls_->count; // gen::run returns to the caller
#endif
        }
    } else {
        const uint64_t since = ls_->count - frame_start_;
        if (idle && since >= kMinFrame && since < kFrameCap && board_->idle_until_vblank()) {
            // a timer interrupt the game enabled was due before vblank: it
            // runs first (board time skipped the wait up to it)
        } else if ((idle && since >= kMinFrame) || since >= kFrameCap) {
            board_->io().inputs = inputs_;
            {
                auto sample = profiler_.measure(profiler_.frame.geometry);
                board_->vblank_start();
            }
            in_vblank_ = true;
            vblank_start_ = ls_->count;
        }
    }
    ls_->add_callback(ls_->count + kProbe, [this] { probe(); });
}

void GameLoop::run_frame(const Inputs &inputs) {
    run_frame_deferred_sound(inputs);
    complete_deferred_sound(execute_deferred_sound());
}

GameLoop::SoundPacket GameLoop::run_frame_sound_packet(const Inputs &inputs) {
    run_frame_deferred_sound(inputs);
    sound_frame_pending_ = false;
    return SoundPacket(sound_.get(), replies_.get(), std::move(pending_sound_bytes_), sound_profile_clock_);
}

uint64_t GameLoop::SoundPacket::execute() {
    if (!sound_) return 0;
    if (!pending_) throw Fatal("detached sound frame has already executed");
    // A partial failure must not permit replaying UART bytes or board time.
    pending_ = false;
    const uint64_t begin = clock_ ? clock_() : 0;
    sound_->send(bytes_.data(), bytes_.size());
    sound_->advance(1.0 / kFrameHz);
    replies_->put(sound_->take_replies());
    const uint64_t end = clock_ ? clock_() : 0;
    return end >= begin ? end - begin : 0;
}

void GameLoop::run_generated() {
#ifdef M2_I960_PROFILE
    I960Profile &prof = i960_profile;
    const uint64_t start = prof.clock ? prof.clock() : 0;
    gen::run(*env_);
    if (prof.clock) prof.ticks += prof.clock() - start;
    ++prof.runs;
#else
    gen::run(*env_);
#endif
}

void GameLoop::run_frame_deferred_sound(const Inputs &inputs) {
    if (sound_frame_pending_) throw Fatal("previous sound frame must complete before advancing the board");
    profiler_.reset();
    auto frame_sample = profiler_.measure(profiler_.frame.total);
    inputs_ = inputs;
    frame_done_ = false;
    if (sound_) { // what the sound board sent back during the frames it has run
        const std::vector<uint8_t> replies = replies_->take();
        board_->receive_sound_bytes(replies.data(), replies.size());
    }
#ifdef M2_FAST_GEN
    ls_->set_end(UINT64_MAX);
#else
    ls_->end_count = UINT64_MAX;
#endif
    while (!frame_done_) {
#ifdef M2_FAST_GEN
        // has_code is a binary search over every address: asked only after a
        // run that did nothing (where the IP has no code, run returns at once).
        const uint64_t count = ls_->count;
        const uint32_t ip = cpu_->m_IP;
        run_generated();
        if (ls_->count == count && cpu_->m_IP == ip && !frame_done_ && !gen::has_code(ip)) {
#else
        if (!gen::has_code(cpu_->m_IP)) {
#endif
            char b[128];
            std::snprintf(b, sizeof b, "no recompiled code at %08x: add it to the seeds", cpu_->m_IP);
            throw Fatal(b);
        }
#ifndef M2_FAST_GEN
        run_generated();
#endif
    }
    // Transfer the UART bytes on the owning thread. The sound worker never
    // touches M2Board, video, i960/TGP state, or the frame profiler.
    if (sound_) {
        pending_sound_bytes_ = board_->take_sound_bytes();
        sound_frame_pending_ = true;
    }
}

uint64_t GameLoop::execute_deferred_sound() {
    if (!sound_frame_pending_) return 0;
    const uint64_t begin = sound_profile_clock_ ? sound_profile_clock_() : 0;
    sound_->send(pending_sound_bytes_.data(), pending_sound_bytes_.size());
    sound_->advance(1.0 / kFrameHz);
    replies_->put(sound_->take_replies());
    const uint64_t end = sound_profile_clock_ ? sound_profile_clock_() : 0;
    return end >= begin ? end - begin : 0;
}

void GameLoop::complete_deferred_sound(uint64_t ticks) {
    if (!sound_frame_pending_) return;
    pending_sound_bytes_.clear();
    sound_frame_pending_ = false;
    profiler_.frame.sound += ticks;
    // Total represents cumulative work, not elapsed time when overlapped.
    // Adding the same duration to total and sound keeps core() unchanged.
    profiler_.frame.total += ticks;
}

} // namespace rt
