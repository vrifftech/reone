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

#include "reone/game/effect/resurrection.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult ResurrectionEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    auto *creature = dyn_cast<Creature>(&object);
    return creature && creature->applyResurrectionEffect(instance.integerParameter(0))
        ? EffectApplicationResult::Applied : EffectApplicationResult::Rejected;
}

} // namespace game

} // namespace reone
