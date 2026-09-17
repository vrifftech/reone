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

#include "reone/game/effect/lightsaberthrow.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/projectiles.h"
#include "reone/scene/node/model.h"

namespace reone::game {
EffectApplicationResult LightsaberThrowEffect::onApply(Object &object, EffectInstance &record) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    if (!object.game().isTSL() ||
        (!creature->attributes().hasSpell(static_cast<SpellType>(162)) &&
         !creature->attributes().hasSpell(static_cast<SpellType>(163)))) {
        creature->setThrowParryBlocked(true);
    }
    if (!record.restoring) {
        object.services().game.projectiles.launchLightsaberThrow(*creature, record, object.game(), object.services());
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult LightsaberThrowEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->setThrowParryBlocked(false);
    return EffectRemovalResult::Removed;
}
} // namespace reone::game
