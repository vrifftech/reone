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

#include "reone/game/action.h"

#include <algorithm>

#include "reone/game/action/usefeat.h"
#include "reone/game/action/movetoobject.h"
#include "reone/game/attack.h"
#include "reone/game/object.h"
#include "reone/system/logutil.h"

namespace reone {

namespace game {

void Action::requireRuntimeObject(const std::shared_ptr<Object> &object) {
    if (object) {
        _runtimeDependencies.emplace_back(object);
    }
}

bool Action::runtimeDependenciesLive() const {
    return std::all_of(
        _runtimeDependencies.begin(),
        _runtimeDependencies.end(),
        [](const auto &reference) { return reference.resolve() != nullptr; });
}

void Action::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    warn("Action execution not implemented: " + std::to_string(static_cast<int>(_type)));
    complete();
}

uint32_t Action::serializedActionId() const {
    if (_savedAction) return _savedAction->actionId;
    // IDs already used by the queue producers and existing codecs.
    switch (_type) {
    case ActionType::MoveToPoint:
    case ActionType::MoveToLocation: return 1;
    case ActionType::PlayAnimation: return 6;
    case ActionType::EquipItem: return 8;
    case ActionType::UnequipItem: return 11;
    case ActionType::AttackObject: return 12;
    case ActionType::UseFeat:
        return isPhysicalAttackFeat(static_cast<const UseFeatAction &>(*this).feat()) ? 12 : 0xffff;
    case ActionType::CastSpellAtObject:
    case ActionType::CastSpellAtLocation: return 15;
    case ActionType::MoveToObject: {
        const auto &move = static_cast<const MoveToObjectAction &>(*this);
        return move.usesPointPath() ? 1 : 17;
    }
    case ActionType::StartConversation: return 24;
    case ActionType::Wait: return 30;
    case ActionType::DoCommand: return 37;
    case ActionType::FollowLeader: return 61;
    default: return 0xffff; // Do not reinterpret an unrepresented engine enum.
    }
}

bool isHostileAction(Action &action) {
    switch (action.type()) {
    case ActionType::AttackObject:
        return true;
    case ActionType::UseFeat:
        return isPhysicalAttackFeat(static_cast<UseFeatAction &>(action).feat());
    default:
        break;
    }
    return false;
}

} // namespace game

} // namespace reone
