/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "../gui.h"

namespace reone::game {

/** The sequel's authored gameover panel. The original title instead uses the
 * existing confirmation message box; neither title borrows the other's layout.
 */
class DeathDisplay : public GameGUI {
public:
    DeathDisplay(Game &game, ServicesView &services) : GameGUI(game, services) {
        _resRef = guiResRef("gameover");
    }
private:
    void onGUILoaded() override;
};

} // namespace reone::game
