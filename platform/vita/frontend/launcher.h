#pragma once

// The launcher: the menu's pages (game, options, controls), their state, what the
// buttons do there and how they are drawn (ImGui). It changes the settings and
// hands back what the game itself must do (MenuAction); frontend/main.cpp does it
// on the Session.

#include "frontend/settings.h"
#include "games/game.h"
#include "runtime/board/test_hold.h"

#include <cstdint>
#include <functional>
#include <string>

namespace vita {

// The game's companion set when the package can bundle it (Game::companion:
// Daytona's two sets), or nullptr.
const rt::Game *companion();

enum class MenuAction { None, Resume, Start, Reset, Quit };

class Launcher {
public:
    explicit Launcher(rt::TestHold &test_hold) : test_hold_(test_hold) {}

    void open() { options_ = binds_ = false; selection_ = 0; capture_ = -1; } // the menu chord
    bool on_options() const { return options_; }

    // One loop's pressed buttons (the touch screen's from draw() are added). Settings
    // changes are applied and saved through commit(clocks too); the status line is set.
    MenuAction handle(uint32_t pressed, bool have_game, VitaSettings &settings, std::string &status,
                      bool &wait_release, const std::function<void(bool)> &commit);
    void draw(bool have_game, const VitaSettings &settings, const std::string &status);

private:
    rt::TestHold &test_hold_;
    bool options_ = false, binds_ = false;
    int selection_ = 0, capture_ = -1;   // capture: action waiting for its new button (Controls page)
    uint32_t touch_buttons_ = 0;         // what draw() saw touched, for the next handle()
};

} // namespace vita
