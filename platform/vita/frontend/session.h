#pragma once

// One game being played: the board (rt::GameLoop), its link to the next cabinet,
// the reference sound board's worker and the sound packet it is running.
//
// The rules main() used to keep by comments and declaration order are this class's:
//   * one sound packet in flight at most; the worker is joined (finish_sound)
//     before anything touches the game or the audio again;
//   * the members are destroyed in reverse order: the worker first (it may still
//     run the packet), then the packet, the game, and the link last (the board
//     holds a pointer to it);
//   * a fault (board, sound worker, native audio) stops the game here and leaves
//     its message in error(); the caller shows the menu.

#include "audio/audio.h"
#include "audio/native_audio.h"
#include "audio/sound_worker.h"
#include "frontend/diagnostics.h"
#include "frontend/settings.h"
#include "render/renderer.h"
#include "runtime/board/game_loop.h"
#include "runtime/board/test_hold.h"
#include "runtime/sound/native/native_sound_engine.h"
#include "system/async_log.h"
#include "system/diagnostic_log.h"

#ifndef DAYTONA_VITA_LINK
#define DAYTONA_VITA_LINK 1
#endif
#if DAYTONA_VITA_LINK
#include "net/link.h"
#endif

#include <memory>
#include <string>

namespace vita {

class Session {
public:
    using NativeOut = NativeAudio<snd::NativeSoundEngine>;
    Session(Audio &audio, NativeOut &native, GpuGlRenderer &gpu, Diagnostics &diag, DiagnosticLog &log,
            AsyncLog &perf_log);
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    rt::GameLoop *game() { return game_.get(); }
    explicit operator bool() const { return bool(game_); }
    bool native_audio() const { return native_audio_; } // the game runs with the native engine
    const SoundWorker &worker() const { return worker_; }
    const std::string &error() const { return error_; }

    // Joins the sound worker. False: it failed, the game is stopped (error()).
    bool finish_sound();
    // The game's EEPROM and, with SAVE RECORDS, its backup RAM. Call after finish_sound.
    bool save_nv(const VitaSettings &settings);

    // A new game from the ROM set: the previous one must be stopped (stop()).
    // False: error(), nothing left running.
    bool start(const VitaSettings &settings, rt::TestHold &test_hold);
    void stop();          // the game, its link and its sound packet dropped (the caller saved first)
    void close_worker();  // at exit, or before running another executable
    void release_all();   // before running another executable: worker, game and link gone

    // One emulated frame and its sound. False: a fault, the game is stopped (error()).
    bool run_frame(const rt::Inputs &inputs, rt::TestHold &test_hold, const VitaSettings &settings);

    void poll_link();
    std::string link_status() const; // the menu's line about the link (empty without one)

private:
    void configure_geometry(); // the geometrizer on its own core, pipelined
    void fail(const char *what, const char *message); // logs it, keeps it in error()

    Audio &audio_;
    NativeOut &native_;
    GpuGlRenderer &gpu_;
    Diagnostics &diag_;
    DiagnosticLog &log_;
    AsyncLog &perf_log_;
    std::string error_;
    bool native_audio_ = false, native_effect_warned_ = false, sound_in_flight_ = false;
    // Destroyed bottom to top: see the top of this file.
#if DAYTONA_VITA_LINK
    std::unique_ptr<TcpLink> link_;
#endif
    std::unique_ptr<rt::GameLoop> game_;
    rt::GameLoop::SoundPacket active_sound_;
    SoundWorker worker_;
};

} // namespace vita
