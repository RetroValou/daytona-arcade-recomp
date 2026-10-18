#include "frontend/session.h"

#include "frontend/storage.h"
#include "runtime/board/game.h"
#include "runtime/romset/rom_import.h"
#include "system/core_policy.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace vita {

Session::Session(Audio &audio, NativeOut &native, GpuGlRenderer &gpu, Diagnostics &diag, DiagnosticLog &log,
                 AsyncLog &perf_log)
    : audio_(audio), native_(native), gpu_(gpu), diag_(diag), log_(log), perf_log_(perf_log) {
    // Reference sound board on its own core (0: unpinned, the fourth-core policy).
    worker_.set_cpu_mask(core_pin_mask(kSoundCore));
    worker_.open();
    log_.log("GPU25 sound worker: threaded=%d affinity_result=%d affinity_mask=0x%x\n",
             int(worker_.threaded()), worker_.affinity_result(), worker_.affinity_mask());
}

void Session::fail(const char *what, const char *message) {
    error_ = message;
    perf_log_.sync([&] { log_.fault("GPU25 %s: %s\n", what, message); });
}

// Join only after the following frame's board work, or before an operation
// that mutates audio/lifetime state. The worker never touches GameLoop's
// profile or pending bytes while the next board frame runs.
bool Session::finish_sound() {
    if (!sound_in_flight_) return true;
    sound_in_flight_ = false;
    const uint64_t begin = diagnostic_ticks_us();
    try { worker_.finish(); }
    catch (const std::exception &e) {
        diag_.sound_failed(begin);
        fail("sound runtime", e.what());
        audio_.pause(); native_.pause();
        active_sound_ = {};
        game_.reset(); // finish has joined, including its error path
        return false;
    }
    diag_.sound_joined(begin, worker_);
    return true;
}

bool Session::save_nv(const VitaSettings &settings) {
    if (!game_) return true;
    const bool a = vita::save_nv("ioboard_eeprom.bin", game_->board().io().eeprom);
    const bool b = !settings.save_backup || vita::save_nv("backup_ram.bin", game_->board().backup_ram());
    if (!a || !b) perf_log_.sync([&] { log_.fault("GPU25: save failed\n"); });
    return a && b;
}

void Session::stop() {
    active_sound_ = {};
    native_.close();
    audio_.close();
    game_.reset();
#if DAYTONA_VITA_LINK
    link_.reset();
#endif
    gpu_.reset_materials();
}

void Session::close_worker() { worker_.close(); }

void Session::release_all() {
    worker_.close();
    game_.reset();
#if DAYTONA_VITA_LINK
    link_.reset();
#endif
}

// Geometrizer on its own core, pipelined (the only Vita GPU mode). The
// geometry thread belongs to the board's geometrizer and ends with it.
void Session::configure_geometry() {
    rt::Geo *geo = geometry_of(game_.get());
    if (!geo) return;
    const bool ok = geo->configure(rt::GeoMode::Pipelined, kGeoCore, kProfile ? ticks_us : nullptr);
    diag_.geometry_changed();
    const rt::Geo::Worker &w = geo->worker();
    char line[256];
    const int n = std::snprintf(line, sizeof line,
        "geometry: mode=%s thread=%d core=%d result=%d mask=0x%x priority=%d fpscr main=0x%08x worker=0x%08x\n",
        rt::geo_mode_name(geo->mode()), int(w.threaded), w.core, w.create_result,
        unsigned(w.affinity_mask), w.priority, unsigned(w.fpscr_main), unsigned(w.fpscr_worker));
    diag_.perf_note(line, std::min(n, int(sizeof line) - 1));
    perf_log_.sync([&] {
        if (ok) log_.log("GPU25 %s", line);
        else log_.fault("GPU25 %s", line); // thread unavailable: geometry stays on the main core
    });
}

bool Session::start(const VitaSettings &settings, rt::TestHold &test_hold) {
    try {
        if (DAYTONA_VITA_LINK && settings.link_enabled && !rt::game().link)
            throw std::runtime_error(std::string("LINK PLAY: ") + rt::game().set +
                                     " HAS NO COMMUNICATION BOARD. SELECT ANOTHER ROM OR TURN LINK OFF.");
        auto images = rt::import_rom_set(kRom);
        native_audio_ = settings.native_audio && rt::game().native_sound; // the game's driver tables known
        native_effect_warned_ = false;
        if (native_audio_) {
            worker_.close();
            auto engine = std::make_unique<snd::NativeSoundEngine>(
                std::move(images.sound_program), std::move(images.pcm1), std::move(images.pcm2), *rt::game().native_sound,
                rt::game().music_channels);
            if (!native_.open(std::move(engine), kDiagnostics ? ticks_us : nullptr))
                throw std::runtime_error("Native audio output unavailable; select Reference audio and reset");
        } else {
            if (!worker_.threaded()) worker_.open();
            if (!audio_.open()) perf_log_.sync([&] { log_.fault("GPU25 reference audio unavailable: %s\n", SDL_GetError()); });
        }
        game_ = std::make_unique<rt::GameLoop>(std::move(images), !native_audio_);
        if (test_hold.armed()) test_hold.arm();
        perf_log_.sync([&] {
            log_.log("GPU25 audio engine: backend=%s clock=%s reference_sound_board=%d\n",
                     native_audio_ ? "NATIVE_TEST" : "REFERENCE",
                     native_audio_ ? "AUDIO_DEVICE_48000" : "GAME_FRAME", int(game_->sound() != nullptr));
        });
        game_->set_profile_clock(kProfile ? ticks_us : nullptr);
        diag_.game_started(*game_);
        load_nv("ioboard_eeprom.bin", game_->board().io().eeprom);
        if (settings.save_backup) load_nv("backup_ram.bin", game_->board().backup_ram());
#if DAYTONA_VITA_LINK
        if (settings.link_enabled) {
            link_ = std::make_unique<TcpLink>(settings.link_port, settings.next_address(), settings.next_port);
            if (!link_->error().empty()) throw std::runtime_error("LINK: " + link_->error());
            game_->board().set_link(link_.get(), settings.link_sync);
        }
#endif
        game_->board().video().set_profile_clock(kProfile ? ticks_us : nullptr);
        // The GPU draws the 3D and the System 24 layers (the only Vita mode: the
        // pipelined geometrizer leaves no exact CPU picture to switch to).
        game_->board().video().set_external_3d(true);
        gpu_.reset_materials();
        configure_geometry();
        return true;
    } catch (const std::exception &e) {
        native_.close(); audio_.pause(); game_.reset();
#if DAYTONA_VITA_LINK
        link_.reset();
#endif
        fail("start", e.what());
        return false;
    }
}

bool Session::run_frame(const rt::Inputs &frame_inputs, rt::TestHold &test_hold, const VitaSettings &settings) {
    const Diagnostics::FrameStart frame_start = diag_.frame_start(*game_);
    try {
        rt::Inputs inputs = frame_inputs;
        test_hold.apply(game_->frames(), inputs.in0);
        auto next_sound = game_->run_frame_sound_packet(inputs);
        diag_.frame_done(*game_, frame_start, diagnostic_ticks_us());
        const uint64_t sound_begin = diagnostic_ticks_us();
        if (native_audio_) {
            // Only commands cross into audio. The callback sequences
            // and mixes continuously, not once per graphics frame.
            const auto bytes = game_->board().take_sound_bytes();
            const auto health = native_.stats();
            if (health.unsupported && !native_effect_warned_) {
                native_effect_warned_ = true;
                log_.log("Native audio: unsupported effect omitted; playback continues\n");
            }
            if (health.failed || health.invalid) {
                char error[192];
                std::snprintf(error, sizeof error,
                    "Native audio fault: callback=%u invalid=%u unsupported=%u. Reset with Reference audio.",
                    health.failed, health.invalid, health.unsupported);
                throw std::runtime_error(error);
            }
            if (!native_.send(bytes.data(), bytes.size()))
                throw std::runtime_error("Native audio queue overflow; use Reference audio and reset");
        } else {
            // Reference backend retains the bounded sound pipeline.
            if (!finish_sound()) return false;
            // The sound worker is idle until the dispatch below: the
            // MultiPCMs' music/effects balance changes between its frames.
            if (auto *sound = game_->sound())
                sound->set_volumes(float(settings.music_volume) / 100.0f, float(settings.effects_volume) / 100.0f);
            active_sound_ = std::move(next_sound);
            worker_.dispatch_packet(active_sound_, audio_, kProfile ? ticks_us : nullptr);
            sound_in_flight_ = true;
        }
        diag_.sound_sync(sound_begin);
        return true;
    } catch (const std::exception &e) {
        // A board/dispatch failure must not destroy a sound board
        // still used by the previous job.
        finish_sound();
        fail("runtime", e.what());
        audio_.pause(); native_.close(); active_sound_ = {}; game_.reset();
        return false;
    }
}

void Session::poll_link() {
#if DAYTONA_VITA_LINK
    if (link_) link_->poll();
#endif
}

std::string Session::link_status() const {
    std::string shown;
#if DAYTONA_VITA_LINK
    if (link_ && game_) {
        const auto *comm = game_->board().comm_board();
        shown += "\nIP " + link_->local_ip() + " RX " + (link_->rx_open() ? "YES" : "WAIT") +
            " TX " + (link_->tx_open() ? "YES" : "WAIT");
        if (comm) shown += " CABINET " + std::to_string(comm->id()) + "/" +
            std::to_string(comm->count()) + (comm->link() == rt::CommBoard::Link::Lost ? " LOST" : "");
    }
#endif
    return shown;
}

} // namespace vita
