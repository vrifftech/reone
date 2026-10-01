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

#include "reone/game/effect/poison.h"
#include "reone/game/combatfeedback.h"
#include "reone/game/combattables.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/damage.h"
#include "reone/game/effect/damageforcepoints.h"
#include "reone/game/effect/forceresisted.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/system/randomutil.h"

#include <cstdint>

namespace reone::game {

namespace {

inline std::int32_t poisonAbilityAmount(int base, float factor) {
    return base * truncateToInteger32(factor);
}
inline std::int32_t poisonAbilityDuration(int duration, int period, float factor) {
    return duration - (truncateToInteger32(factor) - 1) * period;
}

// Working view of the canonical root payload; it is decoded and committed
// at each callback, never maintained as a second live timer.
struct PoisonState {
    enum class Step { None, Damage, Conversation, Expired };
    std::uint32_t startDay {0};
    std::uint32_t startTime {0};
    std::uint32_t tickDay {0};
    std::uint32_t tickTime {0};
    std::int32_t durationSeconds {0};
    std::int32_t periodSeconds {0};
    float factor {2.0f};

    void start(std::uint32_t day, std::uint32_t time, int duration, int period) {
        startDay = tickDay = day;
        startTime = tickTime = time;
        durationSeconds = duration;
        periodSeconds = period;
        factor = 2.0f;
    }
    /**
     * \p age is the time of day part of the world time since the start and
     * \p elapsed that since the last tick; whole days do not count.
     */
    Step step(std::uint32_t age, std::uint32_t elapsed, std::uint32_t day, std::uint32_t time, bool conversation) {
        if (age > std::uint32_t(durationSeconds) * 1000U) {
            return Step::Expired;
        }
        if (elapsed <= std::uint32_t(periodSeconds) * 1000U) return Step::None;
        if (conversation) {
            tickDay = day; // No external tick callback on this branch.
            tickTime = time;
            durationSeconds += periodSeconds;
            return Step::Conversation;
        }
        return Step::Damage;
    }
    void completeTick(std::uint32_t day, std::uint32_t time) {
        factor += 1.0f;
        tickDay = day; // Commit after damage, not before callbacks.
        tickTime = time;
    }
};

// integer slots: row, start day/time, tick day/time, duration, period,
// damage mode. Float slot zero is the retained periodic factor.
inline PoisonState readPoisonState(const EffectInstance &record) {
    PoisonState state;
    state.startDay = uint32_t(record.integerParameter(1));
    state.startTime = uint32_t(record.integerParameter(2));
    state.tickDay = uint32_t(record.integerParameter(3));
    state.tickTime = uint32_t(record.integerParameter(4));
    state.durationSeconds = record.integerParameter(5);
    state.periodSeconds = record.integerParameter(6);
    state.factor = record.floatParameters[0];
    return state;
}
inline void storePoisonState(EffectInstance &record, const PoisonState &state) {
    record.setIntegerParameter(1, static_cast<int32_t>(state.startDay));
    record.setIntegerParameter(2, static_cast<int32_t>(state.startTime));
    record.setIntegerParameter(3, static_cast<int32_t>(state.tickDay));
    record.setIntegerParameter(4, static_cast<int32_t>(state.tickTime));
    record.setIntegerParameter(5, state.durationSeconds);
    record.setIntegerParameter(6, state.periodSeconds);
    record.floatParameters[0] = state.factor;
}

} // namespace

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
        if (creature->hasEffectImmunity(ImmunityType::Poison, source ? dyn_cast<Creature>(source.get()) : nullptr)) {
            addPoisonImmunityFeedback(game, services, source, *creature);
            return queueRemoval(object, instance.id);
        }
        // The first timestamp is written even when resource lookup or the save
        // subsequently fails and the retained root is queued for removal.
        const auto day = game.worldTimeDay();
        const auto time = game.worldTimeOfDay();
        instance.setIntegerParameter(1, static_cast<int32_t>(day));
        instance.setIntegerParameter(2, static_cast<int32_t>(time));
        const auto poison = services.game.combatTables.poison(instance.integerParameter(0));
        if (!poison) return queueRemoval(object, instance.id);
        const auto &data = *poison;
        auto breakdown = creature->getSavingThrowBreakdown(SavingThrow::Fortitude,
            SavingThrowType::Poison, source ? dyn_cast<Creature>(source.get()) : nullptr);
        const int roll = randomInt(1, 20);
        const auto dc = static_cast<uint16_t>(data.difficultyClass);
        addSavingThrowFeedback(game, services, *creature, source.get(), SavingThrow::Fortitude, breakdown, roll, dc);
        if (creature->getSavingThrowResult(roll + breakdown.total(), dc,
                SavingThrowType::Poison, source.get()) != SavingThrowResult::Failed) {
            if (game.isTSL() && (instance.spellId == 7 || instance.spellId == 38)) {
                auto resisted = std::make_shared<ForceResistedEffect>(source);
                auto marker = resisted->saveFacingInstance();
                marker.effect = resisted;
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
        state.start(day, time, data.duration, data.period);
        storePoisonState(instance, state);
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
    child.restoring = false;
    object.applyEffect(std::move(child));
    creature->setActivePoisonEffectId(instance.id);
    return EffectApplicationResult::Retained;
}
void PoisonEffect::onUpdate(Object &object, const EffectInstance &instance, float) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || creature->activePoisonEffectId() != instance.id) return;
    auto &game = object.game();
    // The poison runs on the creature's own time, which a time stop may leave running.
    const auto day = game.activeTimeDay(object);
    const auto time = game.activeTimeOfDay(object);
    auto state = readPoisonState(instance);
    // Only the time of day part of each difference counts.
    uint32_t days = 0;
    uint32_t age = 0;
    uint32_t elapsed = 0;
    game.subtractWorldTimes(day, time, state.startDay, state.startTime, days, age);
    game.subtractWorldTimes(day, time, state.tickDay, state.tickTime, days, elapsed);
    switch (state.step(age, elapsed, day, time, game.isConversationActive())) {
    case PoisonState::Step::Expired:
        queueRemoval(object, instance.id);
        return;
    case PoisonState::Step::Damage:
        applyDamage(*creature, instance, state.factor);
        state.completeTick(day, time);
        break;
    case PoisonState::Step::Conversation: break;
    case PoisonState::Step::None: return;
    }
    if (auto *record = object.findEffectApplication(instance.applicationOrder))
        storePoisonState(*record, state);
}
void PoisonEffect::applyDamage(Creature &creature, const EffectInstance &source, float factor) {
    const auto poison = creature.services().game.combatTables.poison(source.integerParameter(0));
    if (!poison) return;
    const auto &data = *poison;
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
void PoisonEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->clearActivePoisonEffectId(instance.id);
}
} // namespace reone::game
