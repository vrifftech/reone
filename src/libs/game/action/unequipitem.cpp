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

#include "reone/game/action/unequipitem.h"

#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/party.h"

namespace reone {

namespace game {

void UnequipItemAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto *creature = dyn_cast<Creature>(&actor);
    if (!creature || !_item || !_item->isEquipped() || _item->owner() != actor.id()) {
        complete();
        return;
    }
    // An unspecified receiver selects the actor/shared repository. An explicit
    // receiver must be an owned item container, not another creature or an
    // equipment slot.
    if (_container && (!_container->isRuntimeLive() ||
                       _container->owner() != actor.id() || _container == _item)) {
        complete();
        return;
    }
    std::shared_ptr<Object> receiver = _container;
    if (!receiver) receiver = _game.party().sharedInventoryReceiver(
        _game.getObjectById(actor.id()));
    if (receiver) creature->moveEquippedItemTo(_item, *receiver);
    complete();
}

std::optional<SavedActionRecord> UnequipItemAction::saveFacingState() const {
    if (!_item) return std::nullopt;
    // Ordinary equip and unequip actions use IDs 8 and 11.
    // Unequip parameter 1 is a container ObjectId, not an equipment slot.
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 11;
    result.declaredParameterCount = 3;
    result.parameters = {
        {3, SavedObjectReference::fromRuntimeId(_item->id())},
        {3, SavedObjectReference::fromRuntimeId(
                _container ? _container->id() : kSavedRuntimeInvalidObjectId)},
        {1, _instant},
    };
    return result;
}

} // namespace game

} // namespace reone
