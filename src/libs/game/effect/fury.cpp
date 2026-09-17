/*
 * Copyright (c) 2020-2023 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "reone/game/effect/fury.h"

#include <algorithm>
#include "reone/game/object/creature.h"

namespace reone::game {

EffectApplicationResult FuryEffect::onApply(Object &object, EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->applyFuryState(instance.spellId);
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult FuryEffect::onRemove(Object &object, const EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectRemovalResult::Removed;
    const bool survivor = std::any_of(
        object.effects().begin(), object.effects().end(),
        [&instance](const EffectInstance &record) {
            return record.applicationOrder != instance.applicationOrder &&
                   record.hasLiveRuntimeSource() &&
                   record.serializedType == 111;
        });
    if (!survivor) creature->clearFuryState();
    return EffectRemovalResult::Removed;
}

} // namespace reone::game
