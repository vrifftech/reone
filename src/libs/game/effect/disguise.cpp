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

#include "reone/game/effect/disguise.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

EffectApplicationResult DisguiseEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || (!instance.restoring && creature->isDead()))
        return EffectApplicationResult::Rejected;
    for (const auto &record : object.effects()) {
        if (record.serializedType != 62 || record.id == instance.id) continue;
        const EffectId previousId = record.id;
        if (auto *previous = object.findEffectApplication(record.applicationOrder))
            previous->setIntegerParameter(1, 1);
        object.removeEffectsById(previousId);
        break;
    }
    creature->applyDisguiseAppearance(instance.integerParameter(0));
    return EffectApplicationResult::Retained;
}

EffectRemovalResult DisguiseEffect::onRemove(Object &object, const EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || (creature->isDead() && !creature->isPC() && object.isDestroyable()))
        return EffectRemovalResult::Removed;
    if (instance.integerParameter(1) == 0) {
        if (auto *record = object.findEffectApplication(instance.applicationOrder)) {
            record->setIntegerParameter(1, 1);
            record->subType = (record->subType & ~uint16_t(7)) |
                static_cast<uint16_t>(DurationType::Temporary);
            record->duration = 0.0f;
        }
    }
    creature->removeDisguiseAppearance();
    return EffectRemovalResult::Removed;
}

} // namespace game

} // namespace reone
