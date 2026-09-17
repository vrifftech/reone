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

#include "reone/game/effect/modifyattacks.h"
#include "reone/game/game.h"

#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult ModifyAttacksEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) {
        return EffectApplicationResult::Retained;
    }
    // K1 alone rejects an already over-limit owner before mutation.
    if (!creature->game().isTSL() && creature->modifiedAttacks() > 2) {
        return EffectApplicationResult::Rejected;
    }

    creature->adjustModifiedAttacks(instance.integerParameter(0));
    return EffectApplicationResult::Retained;
}

EffectRemovalResult ModifyAttacksEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->adjustModifiedAttacks(-instance.integerParameter(0));
    }

    return EffectRemovalResult::Removed;
}

} // namespace game

} // namespace reone
