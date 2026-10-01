/*
 * Copyright (c) 2026 The reone project contributors
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

#include "../action.h"
#include "../object/door.h"

namespace reone {

namespace game {

/**
 * Burning through a locked door with a lightsaber (TSL). The creature goes
 * to within a metre of the door, faces it, works at it, then opens it.
 */
class DoorSaberAction : public Action {
public:
    DoorSaberAction(Game &game, ServicesView &services, std::shared_ptr<Door> door) :
        Action(game, services, ActionType::DoorSaber),
        _door(std::move(door)) {
        requireRuntimeObject(_door);
    }

    static bool classof(Action *from) {
        return from->type() == ActionType::DoorSaber;
    }

    uint32_t serializedActionId() const override { return 67; }
    void execute(std::shared_ptr<Action> self, Object &actor, float dt) override;
    bool cancel(std::shared_ptr<Action> self, Object &actor) override;
    std::optional<SavedActionRecord> saveFacingState() const override;

private:
    std::shared_ptr<Door> _door;
    bool _approached {false}; // in range and turned to the door
    bool _working {false};    // the work at the door has begun
};

} // namespace game

} // namespace reone
