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

#include "reone/game/camerastyles.h"

#include "reone/resource/2da.h"
#include "reone/resource/provider/2das.h"

using namespace reone::resource;

namespace reone {

namespace game {

void CameraStyles::init() {
    std::shared_ptr<TwoDA> twoDa(_twoDas.get("camerastyle"));
    if (!twoDa) {
        return;
    }
    for (int row = 0; row < twoDa->getRowCount(); ++row) {
        auto style = std::make_shared<CameraStyle>();
        style->name = twoDa->getString(row, "name");
        style->distance = twoDa->getFloat(row, "distance");
        style->pitch = twoDa->getFloat(row, "pitch");
        style->viewAngle = twoDa->getFloat(row, "viewangle");
        style->height = twoDa->getFloat(row, "height");
        // TSL's table names the free-look speeds for the player character; KotOR's does not.
        style->freeLookTiltSpeed = twoDa->getFloat(row, "fl_pc_tiltspeed", twoDa->getFloat(row, "fl_tiltspeed", 60.0f));
        style->freeLookRotateSpeed = twoDa->getFloat(row, "fl_pc_rotatespeed", twoDa->getFloat(row, "fl_rotatespeed", 60.0f));
        style->freeLookUp = twoDa->getFloat(row, "fl_lookup");
        style->freeLookDown = twoDa->getFloat(row, "fl_lookdown");
        _styles.push_back(std::move(style));
    }
}

std::shared_ptr<CameraStyle> CameraStyles::get(int index) const {
    // A row outside the table has no style.
    return index >= 0 && index < static_cast<int>(_styles.size()) ? _styles[index] : nullptr;
}

std::shared_ptr<CameraStyle> CameraStyles::get(const std::string &name) const {
    for (auto &style : _styles) {
        if (style->name == name) {
            return style;
        }
    }
    return nullptr;
}

} // namespace game

} // namespace reone
