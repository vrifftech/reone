/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effect/poison.h"
#include "reone/game/poisonstate.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/damage.h"
#include "reone/game/effect/damageforcepoints.h"
#include "reone/game/effect/forceresisted.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/resource/provider/2das.h"
#include "reone/system/randomutil.h"

namespace reone::game {
EffectApplicationResult PoisonEffect::queueRemoval(Object &object, EffectId id) {
    object.game().queueEffectRemoval(object, id);
    return EffectApplicationResult::Retained;
}
EffectApplicationResult PoisonEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    if (object.plotFlag() || creature->isDead() || creature->isTemporarilyDead() ||
        creature->activePoisonEffectId() != kUnassignedEffectId) return queueRemoval(object, instance.id);
    auto &game = object.game();
    auto &services = object.services();
    if (!instance.restoring) {
        auto source = instance.boundCreator();
        if (creature->hasEffectImmunity(ImmunityType::Poison, dyn_cast<Creature>(source.get()))) {
            addPoisonImmunityFeedback(game, services, source, *creature);
            return queueRemoval(object, instance.id);
        }
        // The first timestamp is written even when resource lookup or the save
        // subsequently fails and the retained root is queued for removal.
        const auto now = game.worldTimeMilliseconds();
        const auto day = static_cast<uint32_t>(game.millisecondsPerWorldDay());
        instance.setIntegerParameter(1, static_cast<int32_t>(now / day));
        instance.setIntegerParameter(2, static_cast<int32_t>(now % day));
        auto table = services.resource.twoDas.get("poison");
        if (!table) return queueRemoval(object, instance.id);
        const auto data = readPoisonData(*table, instance.integerParameter(0));
        auto breakdown = creature->getSavingThrowBreakdown(SavingThrow::Fortitude,
            SavingThrowType::Poison, dyn_cast<Creature>(source.get()));
        const int roll = randomInt(1, 20);
        const auto dc = static_cast<uint16_t>(data.difficultyClass);
        addSavingThrowFeedback(game, services, *creature, SavingThrow::Fortitude, breakdown, roll, dc);
        if (creature->getSavingThrowResult(roll + breakdown.total(), dc,
                SavingThrowType::Poison, source.get()) != SavingThrowResult::Failed) {
            if (game.isTSL() && (instance.spellId == 7 || instance.spellId == 38)) {
                auto resisted = std::make_shared<ForceResistedEffect>(source);
                auto marker = resisted->saveFacingInstance();
                marker.effect = resisted;
                marker.subType = 0;
                marker.objectParameters[0] = instance.creatorId;
                marker.objectParameterObjects[0] = instance.creator;
                object.applyEffect(std::move(marker));
            }
            return queueRemoval(object, instance.id);
        }
        addPoisonedFeedback(game, services, *creature, data.nameStrRef);
        // Voice-chat 28 selects the zero-based Poisoned SSF entry.
        creature->playSound(resource::SoundSetEntry::Poisoned);
        // Initial damage sees the original payload except the start time and
        // periodic factor; the remaining timer fields are committed afterward.
        instance.floatParameters[0] = 2.0f;
        applyDamage(*creature, instance, 1.0f);
        PoisonState state;
        state.start(now, data.duration, data.period);
        storePoisonState(instance, state, day);
        instance.setIntegerParameter(7, 0);
        // Generic duration and poison's millisecond budget are distinct fields.
        instance.setDuration(DurationType::Temporary, static_cast<float>(data.duration) * 1000.0f);
    }
    auto visual = std::make_shared<VisualEffectMarkerEffect>(1003);
    auto child = visual->saveFacingInstance();
    child.effect = visual;
    child.objectParameters[0] = instance.creatorId;
    child.objectParameterObjects[0] = instance.creator;
    child.setDuration(DurationType::Instant, 0.0f);
    child.subType = 0; // A fresh visual, not a supernatural linked child.
    child.restoring = false;
    object.applyEffect(std::move(child));
    creature->setActivePoisonEffectId(instance.id);
    return EffectApplicationResult::Retained;
}
void PoisonEffect::onUpdate(Object &object, const EffectInstance &instance, float) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || creature->activePoisonEffectId() != instance.id) return;
    auto &game = object.game();
    const auto day = static_cast<uint32_t>(game.millisecondsPerWorldDay());
    auto state = readPoisonState(instance, day);
    switch (state.step(game.worldTimeMilliseconds(), day, game.isConversationActive())) {
    case PoisonState::Step::Expired:
        queueRemoval(object, instance.id);
        return;
    case PoisonState::Step::Damage:
        applyDamage(*creature, instance, state.factor);
        state.completeTick(game.worldTimeMilliseconds());
        break;
    case PoisonState::Step::Conversation: break;
    case PoisonState::Step::None: return;
    }
    if (auto *record = object.findEffectApplication(instance.applicationOrder))
        storePoisonState(*record, state, day);
}
void PoisonEffect::applyDamage(Creature &creature, const EffectInstance &source, float factor) {
    auto table = creature.services().resource.twoDas.get("poison");
    if (!table) return;
    const auto data = readPoisonData(*table, source.integerParameter(0));
    const auto child = [&](const std::shared_ptr<Effect> &effect, DurationType type, float seconds) {
        auto result = source.linkedChild(effect);
        result.setDuration(type, seconds);
        result.restoring = false;
        result.exposed = 0; // Poison descendants are hidden from script enumeration.
        return result;
    };
    if (data.hitPointDamage > 0) {
        creature.game().queueEffectApplication(creature,
            child(std::make_shared<DamageEffect>(data.hitPointDamage,
                static_cast<DamageType>(0x2000), DamagePower::Normal, true), DurationType::Instant, 0.0f));
    }
    if (data.forcePointDamage > 0) {
        creature.applyEffect(child(std::make_shared<DamageForcePointsEffect>(data.forcePointDamage),
                                   DurationType::Instant, 0.0f));
    }
    const int remaining = poisonAbilityDuration(data.duration, data.period, factor);
    for (size_t ability = 0; ability < data.abilityDamage.size(); ++ability) {
        if (data.abilityDamage[ability] <= 0 || remaining <= 0) continue;
        // Indexed removal advances after erasure; do not restart and
        // do not remove every modifier owned by the creator or the root package.
        for (size_t index = 0; index < creature.effects().size(); ++index) {
            const auto record = creature.effects()[index];
            if (record.type() == EffectType::AbilityDecrease && record.spellId == source.spellId &&
                record.integerParameter(0) == static_cast<int>(ability))
                creature.removeEffectApplication(record.applicationOrder);
        }
        creature.applyEffect(child(std::make_shared<AbilityDecreaseEffect>(static_cast<Ability>(ability),
            poisonAbilityAmount(data.abilityDamage[ability], factor)), DurationType::Temporary,
            static_cast<float>(remaining)));
    }
}
EffectRemovalResult PoisonEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->clearActivePoisonEffectId(instance.id);
    return EffectRemovalResult::Removed;
}
} // namespace reone::game
