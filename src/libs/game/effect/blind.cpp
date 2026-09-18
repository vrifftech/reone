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

#include "reone/game/effect/blind.h"
#include "reone/game/effect/misschance.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/ultravision.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult BlindEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    const int mask = instance.integerParameter(0);
    if (mask != 8 && mask != 16) return EffectApplicationResult::Rejected;
    const auto creator = instance.boundCreator();
    if (mask == 16 && creature->hasEffectImmunity(ImmunityType::Blindness, dyn_cast<Creature>(creator.get()))) {
        addBlindnessImmunityFeedback(object.game(), object.services(), creator, *creature);
        return EffectApplicationResult::Rejected;
    }
    if (object.plotFlag()) return EffectApplicationResult::Rejected;
    if (mask == 8 && (creature->visibilityCounterBits() & 6) != 0)
        return EffectApplicationResult::Retained;
    auto miss = instance.linkedChild(std::make_shared<MissChanceEffect>(50));
    miss.setIntegerParameter(1, mask == 8 ? 1 : 0);
    object.applyEffect(std::move(miss));
    object.applyEffect(instance.linkedChild(std::make_shared<VisionEffect>(4)));
    object.applyEffect(instance.linkedChild(std::make_shared<VisualEffectMarkerEffect>(5002)));
    creature->setVisibilityCounter(static_cast<uint8_t>(mask));
    return EffectApplicationResult::Retained;
}

EffectRemovalResult BlindEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object))
        creature->restoreBlindnessCounter(instance.integerParameter(0), instance.applicationOrder);
    return EffectRemovalResult::Removed;
}

} // namespace game

} // namespace reone
