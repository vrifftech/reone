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
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/castspellatlocation.h"
#include "reone/game/d20/spell.h"
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
    case ActionType::MoveAwayFromObject: return 3;
    case ActionType::JumpToLocation: return 5;
    case ActionType::SpeakString: return 14;
    case ActionType::ResumeConversation: return 32;
    case ActionType::SpeakStringByStrRef: return 33;
    case ActionType::GiveItem: return 34;
    case ActionType::TakeItem: return 35;
    case ActionType::MoveAwayFromLocation: return 44;
    case ActionType::JumpToObject: return 48;
    case ActionType::Appear: return 52;
    case ActionType::BarkString: return 62;
    case ActionType::SurrenderToEnemies: return 65;
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
    case ActionType::PickUpItem: return 7;
    case ActionType::PutDownItem: return 9;
    case ActionType::OpenDoor: return 20;
    case ActionType::CloseDoor: return 21;
    case ActionType::StartConversation: return 24;
    case ActionType::Wait: return 30;
    case ActionType::DoCommand: return 37;
    case ActionType::RandomWalk: return 45;
    case ActionType::OpenLock: return 38;
    case ActionType::Lock: return 39;
    case ActionType::OpenContainer: return 40;
    case ActionType::Follow:
    case ActionType::ForceFollowObject: return 55;
    case ActionType::FollowLeader: return 61;
    case ActionType::FollowOwner: return 70;
    default: return 0xffff; // An action type with no saved ID has none.
    }
}

std::optional<int> scriptActionNumber(uint32_t actionId, bool tsl) {
    switch (actionId) {
    case 1: return 0;
    case 7: return 1;
    case 9: return 2;
    case 12: return 3;
    case 15: return 4;
    case 20: return 5;
    case 21: return 6;
    case 24: return 7;
    case 25: return 8;
    case 26: return 9;
    case 27: return 10;
    case 28: return 11;
    case 29: return 12;
    case 30: return 36;
    case 38: return 13;
    case 39: return 14;
    case 40: return 15;
    case 41: return 16;
    case 42: return 17;
    case 43: return 18;
    case 46: return 19;
    case 50: return 31;
    case 54: return 34;
    case 55: return 35;
    case 56: return 33;
    case 61: return 38;
    case 63: return 39;
    default: break;
    }
    if (!tsl) return std::nullopt;
    switch (actionId) {
    case 67: return 40;
    case 68: return 41;
    case 69: return 42;
    case 70: return 43;
    case 71: return 44;
    default: return std::nullopt;
    }
}

bool isHostileAction(Action &action) {
    switch (action.type()) {
    case ActionType::AttackObject:
        return true;
    case ActionType::UseFeat:
        return isPhysicalAttackFeat(static_cast<UseFeatAction &>(action).feat());
    case ActionType::CastSpellAtObject:
        return static_cast<CastSpellAtObjectAction &>(action).spell()->hostile;
    case ActionType::CastSpellAtLocation:
        return static_cast<CastSpellAtLocationAction &>(action).spell()->hostile;
    default:
        break;
    }
    return false;
}

} // namespace game

} // namespace reone
