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

#include "reone/game/effect/abilityincrease.h"
#include "reone/game/object/creature.h"
#include "reone/game/effect/rules.h"

namespace reone {

namespace game {

EffectApplicationResult AbilityIncreaseEffect::onApply(Object &object, EffectInstance &instance) {
    // Load mode bypasses only the dead/dying gate. Target, amount,
    // immunity and plot checks still apply; equipped duration is not a bypass.
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Rejected;

    return (admitsAbilityEffect(!instance.restoring && creature->isDead(),
                                !instance.restoring && creature->isTemporarilyDead(),
                                instance.integerParameter(1), false, creature->plotFlag()))
        ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}

} // namespace game

} // namespace reone
