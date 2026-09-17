/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effect/haste.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/acincrease.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/movementspeedincrease.h"
#include "reone/game/effect/movementspeeddecrease.h"
#include "reone/game/effect/savingthrowdecrease.h"
#include "reone/game/object/creature.h"

namespace reone::game {
namespace {
void changeHasteSlowInternal(Object &object, HasteSlowTransition transition,
                            const EffectInstance *applyingRoot) {
    if (transition.before == transition.after) return;
    if (transition.before != 0) {
        const uint16_t previousType = transition.before > 0 ? 41 : 42;
        for (const auto &record : object.effects()) {
            if (record.serializedType > previousType) break;
            if (record.serializedType == previousType) {
                const auto id = record.id;
                object.removeEffectsById(id);
                break;
            }
        }
    } else {
        object.applyEffect(makeHasteSlowInternal(transition.after, applyingRoot));
    }
}
} // namespace

EffectApplicationResult HasteSlowEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    const auto creator = instance.boundCreator();
    // Slow admission checks immunity even on load; Haste does not.
    if (instance.serializedType == 3 &&
        hasSlowImmunity(*creature, dyn_cast<Creature>(creator.get()))) {
        addSlowImmunityFeedback(object.game(), object.services(), creator, *creature);
        return EffectApplicationResult::Rejected;
    }
    changeHasteSlowInternal(object,
        getHasteSlowTransition(object.effects(), instance.serializedType, false), &instance);
    return EffectApplicationResult::Retained;
}

EffectRemovalResult HasteSlowEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        // These cached flags are cleared before the balance comparison,
        // including the branch that does not replace an existing internal.
        creature->setHasted(false);
        creature->setSlowed(false);
        changeHasteSlowInternal(object,
            getHasteSlowTransition(object.effects(), instance.serializedType, true), nullptr);
    }
    return EffectRemovalResult::Removed;
}

EffectInstance HasteSlowInternalEffect::saveFacingInstance() const {
    auto record = Effect::saveFacingInstance();
    record.serializedType = _haste ? 41 : 42;
    return record;
}

EffectApplicationResult HasteSlowInternalEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    std::vector<EffectInstance> members;
    if (_haste) {
        members.push_back(instance.linkedChild(std::make_shared<MovementSpeedIncreaseEffect>(150)));
        members.push_back(instance.linkedChild(std::make_shared<ACIncreaseEffect>(4, ACBonus::Dodge, 0x4007)));
    } else {
        members = {
            instance.linkedChild(std::make_shared<MovementSpeedDecreaseEffect>(50)),
            instance.linkedChild(std::make_shared<ACDecreaseEffect>(2, ACBonus::Dodge, 0x4007)),
            instance.linkedChild(std::make_shared<AttackDecreaseEffect>(2, AttackBonus::Misc)),
            instance.linkedChild(std::make_shared<SavingThrowDecreaseEffect>(
                static_cast<int>(SavingThrow::Reflex), 2, SavingThrowType::All)),
            instance.linkedChild(std::make_shared<LimitMovementSpeedEffect>())};
    }
    for (auto &member : members) member.markGeneratedForLoad();
    object.applyEffectPackage(members);
    creature->setHasted(_haste);
    creature->setSlowed(!_haste);
    return EffectApplicationResult::Retained;
}

EffectRemovalResult HasteSlowInternalEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        if (_haste) creature->setHasted(false);
        else creature->setSlowed(false);
    }
    return EffectRemovalResult::Removed;
}
} // namespace reone::game
