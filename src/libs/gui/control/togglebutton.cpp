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

#include "reone/gui/control/togglebutton.h"

#include "reone/graphics/renderbuffer.h"

namespace reone {

namespace gui {

void ToggleButton::load(const resource::generated::GUI_BASECONTROL &gui, bool protoItem) {
    Button::load(gui, protoItem);
    if (protoItem) {
        // A list prototype keeps its on frames for rows that carry a state;
        // it starts off, since every row shares it.
        auto &proto = static_cast<const resource::generated::GUI_CONTROLS_PROTOITEM &>(gui);
        auto authored = [](const resource::generated::GUI_BORDER &border) {
            return !border.FILL.empty() || !border.EDGE.empty() || !border.CORNER.empty();
        };
        if (authored(proto.SELECTED)) _onBorder = createBorder(proto.SELECTED);
        if (authored(proto.HILIGHTSELECTED)) _onHilight = createBorder(proto.HILIGHTSELECTED);
        return;
    }
    auto &controlStruct = *static_cast<const resource::generated::GUI_CONTROLS *>(&gui);
    if (controlStruct.SELECTED) _onBorder = createBorder(*controlStruct.SELECTED);
    if (controlStruct.HILIGHTSELECTED) _onHilight = createBorder(*controlStruct.HILIGHTSELECTED);
    _on = controlStruct.ISSELECTED != 0;
}

void ToggleButton::render(const glm::ivec2 &screenSize, const glm::ivec2 &offset, scene::IRenderPass &pass) {
    if (!_on) {
        Button::render(screenSize, offset, pass);
        return;
    }
    // The on frames follow the presentation scale of the frames they replace.
    auto border = _border;
    auto hilight = _hilight;
    if (_onBorder) {
        if (border) _onBorder->dimension = border->dimension;
        _border = _onBorder;
    }
    if (_onHilight) {
        if (hilight) _onHilight->dimension = hilight->dimension;
        _hilight = _onHilight;
    }
    Button::render(screenSize, offset, pass);
    _border = std::move(border);
    _hilight = std::move(hilight);
}

const glm::vec3 &ToggleButton::getBorderColor() const {
    return _on && _onColor ? *_onColor : Button::getBorderColor();
}

void ToggleButton::toggle() {
    _on = !_on;
}

void ToggleButton::setOnColor(const glm::vec3 &color) {
    _onColor = color;
}

} // namespace gui

} // namespace reone
