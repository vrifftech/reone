/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/gui/deathdisplay.h"
#include "reone/game/game.h"
#include "reone/gui/control/button.h"

namespace reone::game {

void DeathDisplay::onGUILoaded() {
    const auto quit = findControl<gui::Button>("BTN_QUIT");
    const auto load = findControl<gui::Button>("BTN_LOADGAME");
    const auto last = findControl<gui::Button>("BTN_LASTSAVE");
    quit->setOnClick([this]() { _game.requestEndGame(); });
    load->setOnClick([this]() { _game.openSaveLoad(SaveLoadMode::LoadFromInGame); });
    // The save backend exists, but the last-save selector is not yet represented.
    last->setDisabled(true);
}

} // namespace reone::game
