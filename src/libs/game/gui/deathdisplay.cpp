/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/gui/deathdisplay.h"
#include "reone/game/game.h"
#include "reone/resource/strings.h"

namespace reone::game {

// Buttons ignore presses for this long after the panel goes up.
static constexpr float kDeathDisplayReadiness = 0.5f;
// Unready buttons have a dull border and dark green text; ready ones a dark
// green border and bright green text.
static constexpr glm::vec3 kUnreadyBorderColor {0.4f, 0.4f, 0.3f};
static constexpr glm::vec3 kReadyBorderColor {0.05f, 0.35f, 0.27f};
static constexpr glm::vec3 kUnreadyTextColor {0.05f, 0.35f, 0.27f};
static constexpr glm::vec3 kReadyTextColor {0.1f, 0.7f, 0.55f};
// Shown when no save of the current character can be launched.
static constexpr int kStrRefLastSaveUnavailable = 42491;

void DeathDisplay::onGUILoaded() {
    _controls.BTN_LASTSAVE = findControl<gui::Button>("BTN_LASTSAVE");
    _controls.BTN_LOADGAME = findControl<gui::Button>("BTN_LOADGAME");
    _controls.BTN_QUIT = findControl<gui::Button>("BTN_QUIT");

    _controls.BTN_LASTSAVE->setOnClick([this]() {
        if (_readiness > 0.0f) return;
        _game.leaveGameOver(false);
        switch (_game.launchMostRecentSave()) {
        case Game::LastSaveLaunch::Launched:
            dismiss();
            break;
        case Game::LastSaveLaunch::Unavailable:
            _game.showMessagePopup(_services.resource.strings.getText(kStrRefLastSaveUnavailable));
            break;
        case Game::LastSaveLaunch::NoSaves:
            break;
        }
    });
    _controls.BTN_LOADGAME->setOnClick([this]() {
        if (_readiness > 0.0f) return;
        _game.leaveGameOver(false);
        if (_game.savedGames().empty()) return;
        dismiss();
        _game.openSaveLoad(SaveLoadMode::LoadAfterDeath);
    });
    _controls.BTN_QUIT->setOnClick([this]() {
        if (_readiness > 0.0f) return;
        _game.leaveGameOver(true);
        dismiss();
    });
}

void DeathDisplay::present() {
    if (_presented) return;
    _presented = true;
    _readiness = kDeathDisplayReadiness;
    showButtonsReady(false);
}

void DeathDisplay::update(float dt) {
    _readiness -= dt;
    if (_readiness < 0.0f) showButtonsReady(true);
    GameGUI::update(dt);
}

void DeathDisplay::showButtonsReady(bool ready) {
    for (const auto &button : {_controls.BTN_LASTSAVE, _controls.BTN_LOADGAME, _controls.BTN_QUIT}) {
        button->setBorderColor(ready ? kReadyBorderColor : kUnreadyBorderColor);
        button->setTextColor(ready ? kReadyTextColor : kUnreadyTextColor);
    }
}

} // namespace reone::game
