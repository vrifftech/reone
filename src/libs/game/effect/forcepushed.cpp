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

#include "reone/scene/collision.h"
#include "reone/game/effect/forcepushed.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/area.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/model.h"

namespace reone::game {
EffectInstance ForcePushStateEffect::saveFacingInstance() const {
    auto record = Effect::saveFacingInstance();
    record.serializedType = 8;
    return record;
}
EffectApplicationResult ForcePushStateEffect::onApply(Object &object, EffectInstance &record) {
    auto *creature = dyn_cast<Creature>(&object);
    return creature && (record.restoring || creature->isForcePushed())
        ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}
void ForcePushStateEffect::onUpdate(Object &object, const EffectInstance &record, float) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || !creature->isForcePushed()) object.removeEffectsById(record.id);
}
EffectRemovalResult ForcePushStateEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->endForcePush();
    return EffectRemovalResult::Removed;
}
bool applyForcePushMovement(Object &object, const glm::vec3 &centre,
                            bool ignoreDirectLine, EffectInstance &owner) {
    auto *target = dyn_cast<Creature>(&object);
    if (!target) return false;
    glm::vec3 delta = target->position() - centre;
    const float length = std::sqrt((delta.x * delta.x + delta.y * delta.y) + delta.z * delta.z);
    // Normalization supplies +X below the double-precision 1e-9 threshold.
    const glm::vec3 direction = static_cast<double>(length) < 1e-9
        ? glm::vec3(1.0f, 0.0f, 0.0f) : delta * (1.0f / length);
    const float distance = owner.floatParameters[3] == 0.0f ? 5.0f : owner.floatParameters[3];
    glm::vec3 destination = target->position() + direction * distance;
    const float facing = -glm::atan(-direction.x, -direction.y);
    target->setFacing(facing);
    auto module = object.game().module();
    auto area = module ? module->area() : nullptr;
    if (!area) return false;
    if (!ignoreDirectLine && target->sceneNode()) {
        scene::Collision collision;
        glm::vec3 origin = target->position() + glm::vec3(0.0f, 0.0f, 0.1f);
        if (target->sceneNode()->graph().testWalk(origin,
                destination + glm::vec3(0.0f, 0.0f, 0.1f), target, collision))
            destination = collision.intersection;
    }
    target->beginForcePush(destination, facing);
    auto state = owner.linkedChild(std::make_shared<ForcePushStateEffect>());
    state.setDuration(DurationType::Temporary, owner.duration);
    state.restoring = false;
    object.applyEffect(std::move(state));
    return true;
}
EffectApplicationResult ForcePushedEffect::onApply(Object &object, EffectInstance &record) {
    if (auto creator = record.boundCreator())
        applyForcePushMovement(object, creator->position(), false, record);
    // Retain even inert non-creature or missing-creator records.
    return EffectApplicationResult::Retained;
}
} // namespace reone::game
