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

#include "reone/game/object/creature.h"
#include "reone/game/effect/forceresistanceincrease.h"
#include "reone/game/effect/forceresistancedecrease.h"

namespace reone::game {
EffectApplicationResult ForceResistanceIncreaseEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    const int amount = instance.integerParameter(0);
    if (amount < 0) return EffectApplicationResult::Rejected;
    creature->forceResistance().applyIncrease(amount);
    return EffectApplicationResult::Retained;
}
EffectRemovalResult ForceResistanceIncreaseEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object))
        creature->forceResistance().removeIncrease(object.effects(), instance);
    return EffectRemovalResult::Removed;
}
EffectApplicationResult ForceResistanceDecreaseEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    auto creator = instance.boundCreator();
    if (creature->hasEffectImmunity(ImmunityType::ForceResistanceDecrease, dyn_cast<Creature>(creator.get())))
        return EffectApplicationResult::Rejected;
    const int amount = instance.integerParameter(0);
    if (amount < 0 || object.plotFlag()) return EffectApplicationResult::Rejected;
    creature->forceResistance().applyDecrease(amount);
    return EffectApplicationResult::Retained;
}
EffectRemovalResult ForceResistanceDecreaseEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object))
        creature->forceResistance().removeDecrease(object.effects(), instance);
    return EffectRemovalResult::Removed;
}
} // namespace reone::game
