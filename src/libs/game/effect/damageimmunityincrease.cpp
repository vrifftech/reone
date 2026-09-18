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

#include "reone/game/effect/damageimmunityincrease.h"

#include "reone/game/effect/damageimmunitydecrease.h"
#include "reone/game/object.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult DamageImmunityIncreaseEffect::onApply(
    Object &, EffectInstance &) {
    return (_percentImmunity >= 0) ? EffectApplicationResult::Retained
        : EffectApplicationResult::Rejected;
}

EffectApplicationResult DamageImmunityDecreaseEffect::onApply(
    Object &object, EffectInstance &instance) {
    if (_percentImmunity < 0 || object.plotFlag()) {
        return EffectApplicationResult::Rejected;
    }
    auto *target = dyn_cast<Creature>(&object);
    if (!target) {
        return EffectApplicationResult::Retained;
    }
    auto creator = instance.boundCreator();
    return (!target->hasEffectImmunity(
        ImmunityType::DamageImmunityDecrease,
        creator ? dyn_cast<Creature>(creator.get()) : nullptr)) ? EffectApplicationResult::Retained
        : EffectApplicationResult::Rejected;
}

} // namespace game

} // namespace reone
