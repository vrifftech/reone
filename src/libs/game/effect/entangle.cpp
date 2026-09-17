/*
 * Copyright (c) 2020-2023 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "reone/game/effect/entangle.h"

#include "reone/game/combat.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"

namespace reone::game {

EffectApplicationResult EntangleEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;

    auto creator = instance.boundCreator();
    auto *creatorCreature = dyn_cast<Creature>(creator.get());
    if (creature->hasEffectImmunity(ImmunityType::Entangle, creatorCreature)) {
        addEntangleImmunityFeedback(object.game(), object.services(), creator, *creature);
        return EffectApplicationResult::Rejected;
    }
    if (object.plotFlag()) return EffectApplicationResult::Rejected;

    creature->clearAllActions(true);
    object.game().combat().cancelActions(*creature);

    std::vector<EffectInstance> children;
    auto attack = instance.linkedChild(
        std::make_shared<AttackDecreaseEffect>(2, AttackBonus::Misc));
    // Copy the rules-owned racial selector into the third slot.
    // The stock rules value is 28 for this package.
    attack.setIntegerParameter(2, 28);
    children.push_back(std::move(attack));
    children.push_back(instance.linkedChild(
        std::make_shared<AbilityDecreaseEffect>(Ability::Dexterity, 4)));
    children.push_back(instance.linkedChild(
        std::make_shared<CreatureAIStateEffect>(-3)));
    object.applyEffectPackage(children);
    return EffectApplicationResult::Retained;
}

EffectRemovalResult EntangleEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object); creature && !creature->isPC()) {
        creature->refreshVisibilityPerception();
    }
    return EffectRemovalResult::Removed;
}

} // namespace reone::game
