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

#pragma once

#include <optional>

#include "button.h"

namespace reone {

namespace gui {

class ToggleButton : public Button {
public:
    ToggleButton(
        IGUI &gui,
        scene::ISceneGraphs &sceneGraphs,
        graphics::GraphicsServices &graphicsSvc,
        resource::ResourceServices &resourceSvc) :
        Button(
            gui,
            ControlType::ToggleButton,
            sceneGraphs,
            graphicsSvc,
            resourceSvc) {
    }

    void load(const resource::generated::GUI_BASECONTROL &gui, bool protoItem = false) override;
    void render(const glm::ivec2 &screenSize, const glm::ivec2 &offset, scene::IRenderPass &pass) override;

    const glm::vec3 &getBorderColor() const override;

    void toggle();
    void setOn(bool on) { _on = on; }

    bool isOn() const { return _on; }

    void setOnColor(const glm::vec3 &color);

private:
    bool _on {false};
    std::optional<glm::vec3> _onColor;
    // Frames drawn while on, in place of the border and the hilight.
    std::shared_ptr<Border> _onBorder;
    std::shared_ptr<Border> _onHilight;
};

} // namespace gui

} // namespace reone
