/*
 * Copyright (c) 2020-2023 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "reone/game/gui/sounds.h"

#include "reone/resource/format/2dareader.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/provider/audioclips.h"

using namespace reone::audio;
using namespace reone::resource;

namespace reone {

namespace game {

void GUISounds::init() {
    std::shared_ptr<TwoDA> sounds(_twoDas.get("guisounds"));
    if (!sounds) {
        return;
    }
    loadSound(*sounds, "Clicked_Default", _onClick);
    loadSound(*sounds, "Entered_Default", _onEnter);
    loadSound(*sounds, "Level_Up_Notify", _onLevelUpNotify);
    // Rows 10 and 11: an item chosen for, or cleared from, an equipment slot.
    loadSound(*sounds, "Inventory_Select", _inventorySelect);
    loadSound(*sounds, "Inventory_Drop", _inventoryDrop);
    // TSL only: the light and dark side stings.
    loadSound(*sounds, "DarkSide", _darkSide);
    loadSound(*sounds, "LightSide", _lightSide);
    // Rows 2 and 6 are the refusal and acceptance sounds.
    if (sounds->getRowCount() > 2)
        _actionUnavailable = _audioClips.get(sounds->getString(2, "soundresref"));
    if (sounds->getRowCount() > 6)
        _actionAccepted = _audioClips.get(sounds->getString(6, "soundresref"));
    // Row 3 is the toggle-button click.
    if (sounds->getRowCount() > 3)
        _checkboxCheck = _audioClips.get(sounds->getString(3, "soundresref"));
}

void GUISounds::loadSound(const TwoDA &twoDa, const std::string &label, std::shared_ptr<AudioClip> &sound) {
    int row = twoDa.indexByCellValue("label", label);
    if (row != -1) {
        sound = _audioClips.get(twoDa.getString(row, "soundresref"));
    }
}

void GUISounds::deinit() {
    _onClick.reset();
    _onEnter.reset();
    _onLevelUpNotify.reset();
    _actionAccepted.reset();
    _actionUnavailable.reset();
    _checkboxCheck.reset();
    _inventorySelect.reset();
    _inventoryDrop.reset();
    _darkSide.reset();
    _lightSide.reset();
}

} // namespace game

} // namespace reone
