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

#include "reone/game/object.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/effect/visual.h"
#include "reone/game/effect/forceresisted.h"

namespace reone {

namespace game {

EffectApplicationResult ForceResistedEffect::onApply(Object &object, EffectInstance &instance) {
    // These markers are fresh operations, not children of the incoming group.
    auto visual = std::make_shared<VisualEffect>(4037, false, object.services());
    auto child = visual->saveFacingInstance();
    child.effect = visual;
    child.subType = 0;
    child.creatorId = object.id();
    child.creator = object.game().getObjectById(object.id());
    child.setDuration(DurationType::Instant, 0.0f);
    child.restoring = instance.restoring;
    child.objectParameters[0] = instance.objectParameters[0];
    child.objectParameterObjects[0] = instance.objectParameterObjects[0];
    object.applyEffect(std::move(child));
    // Animation follows submission independently of VFX resources.
    if (auto *creature = dyn_cast<Creature>(&object)) creature->playForceResistedAnimation();
    return EffectApplicationResult::Applied;
}

} // namespace game

} // namespace reone
