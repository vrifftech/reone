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

#include "reone/game/effect/bonusfeat.h"
#include "reone/game/effect/skillincrease.h"
#include "reone/game/effect/skilldecrease.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult BonusFeatEffect::onApply(Object &object, EffectInstance &) {
    return dyn_cast<Creature>(&object)
        ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}

EffectApplicationResult SkillIncreaseEffect::onApply(Object &object, EffectInstance &instance) {
    return dyn_cast<Creature>(&object) && instance.integerParameter(1) >= 0
        ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}

EffectApplicationResult SkillDecreaseEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || instance.integerParameter(1) < 0 || object.plotFlag())
        return EffectApplicationResult::Rejected;
    const auto creator = instance.boundCreator();
    return creature->hasEffectImmunity(ImmunityType::SkillDecrease, dyn_cast<Creature>(creator.get()))
        ? EffectApplicationResult::Rejected : EffectApplicationResult::Retained;
}

} // namespace game

} // namespace reone
