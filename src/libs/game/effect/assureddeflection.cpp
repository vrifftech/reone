/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effect/assureddeflection.h"
#include "reone/game/object/creature.h"

namespace reone::game {

EffectApplicationResult AssuredDeflectionEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    return creature && creature->applyAssuredDeflection(instance.integerParameter(0))
        ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}

EffectRemovalResult AssuredDeflectionEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->removeAssuredDeflection();
    return EffectRemovalResult::Removed;
}

} // namespace reone::game
