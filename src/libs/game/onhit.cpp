/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/onhit.h"
#include "reone/game/effect/forcepushed.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/effect/poison.h"
#include "reone/game/game.h"
#include "reone/game/di/services.h"
#include "reone/game/object/creature.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/death.h"
#include "reone/system/arrayref.h"
#include "posthitfeedback.h"
#include <type_traits>

namespace reone::game {
static constexpr float kCombatFeedbackRange2 = 900.0f;
static constexpr int kStrRefFearImmunity = 1481;
static constexpr int kStrRefParalysisImmunity = 1483;
static constexpr int kStrRefSleepImmunity = 1488;
static constexpr int kStrRefConfusionImmunity = 1489;
static constexpr int kStrRefStunImmunity = 1490;
static constexpr int kStrRefAbilityDrain = 42408;
static constexpr int kStrRefStrength = 211;
static constexpr int kStrRefDexterity = 212;
static constexpr int kStrRefConstitution = 213;
static constexpr int kStrRefWisdom = 214;
static constexpr int kStrRefIntelligence = 215;
static constexpr int kStrRefCharisma = 216;

static bool canReceiveCombatFeedback(
    const Creature &player,
    const Creature &subject) {

    return player.faction() == subject.faction() &&
           player.getSquareDistanceTo(subject) <= kCombatFeedbackRange2;
}

static std::string getFeedbackString(
    Game &game,
    ServicesView &services,
    int strRef,
    std::initializer_list<std::pair<int, std::string>> tokens) {

    std::string text = services.resource.strings.getText(strRef);
    for (const auto &[token, value] : tokens) {
        text = game.substituteCustomToken(
            std::move(text),
            token,
            value);
    }
    return text;
}

static int getCombatFeedbackBroadcastCount(
    Game &game,
    const std::shared_ptr<Object> &attacker,
    const Object &target) {

    auto leader = game.party().getLeader();
    if (!leader) {
        return 0;
    }

    int broadcasts = 0;
    if (auto *attackerCreature = dyn_cast<Creature>(attacker.get())) {
        broadcasts += static_cast<int>(
            canReceiveCombatFeedback(*leader, *attackerCreature));
    }
    if (const auto *targetCreature = dyn_cast<const Creature>(&target)) {
        broadcasts += static_cast<int>(
            canReceiveCombatFeedback(*leader, *targetCreature));
    }
    return broadcasts;
}

static void addEffectOutcomeFeedback(
    Game &game,
    ServicesView &services,
    const std::string &targetName,
    const EffectOutcomeBreakdown &values,
    int broadcasts) {

    const auto message = getEffectOutcomeMessageStrRef(game.isTSL(), values.outcome);
    if (!values.present || !message || broadcasts <= 0) {
        return;
    }
    std::string suffix = services.resource.strings.getText(*message);
    suffix = game.substituteCustomToken(std::move(suffix), 0, targetName);
    if (const auto label = getEffectOutcomeLabelStrRef(values.effectType)) {
        suffix = game.substituteCustomToken(
            std::move(suffix), 1, services.resource.strings.getText(*label));
    }
    const std::string text = services.resource.strings.getText(kEffectOutcomePrefixStrRef) + suffix;
    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
    }
}

static std::optional<int> getStateImmunityStrRef(CreatureState state) {
    switch (state) {
    case CreatureState::Confusion:
        return kStrRefConfusionImmunity;
    case CreatureState::Fear:
        return kStrRefFearImmunity;
    case CreatureState::Stun:
        return kStrRefStunImmunity;
    case CreatureState::Paralysis:
        return kStrRefParalysisImmunity;
    case CreatureState::Sleep:
        return kStrRefSleepImmunity;
    case CreatureState::DroidStun:
    case CreatureState::Choke:
    case CreatureState::Horrified:
    case CreatureState::Whirlwind:
    case CreatureState::None:
        return std::nullopt;
    }
    return std::nullopt;
}

static void addStateImmunityFeedback(
    Game &game,
    ServicesView &services,
    const std::shared_ptr<Object> &creator,
    const Object &target,
    CreatureState state) {

    auto leader = game.party().getLeader();
    const auto *targetCreature = dyn_cast<Creature>(&target);
    if (!leader || !targetCreature) {
        return;
    }

    bool targetHasRecipient = targetCreature == leader.get();
    bool creatorHasRecipient = creator.get() == leader.get();
    if (!targetHasRecipient && !creatorHasRecipient) {
        return;
    }

    const auto strRef = getStateImmunityStrRef(state);
    if (!strRef) return;
    std::string text = getFeedbackString(
        game,
        services,
        *strRef,
        {{0, target.name()}});
    if (targetHasRecipient) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            text);
    }
    if (creatorHasRecipient) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            text);
    }
}

static int getAbilityStrRef(Ability ability) {
    switch (ability) {
    case Ability::Strength:
        return kStrRefStrength;
    case Ability::Dexterity:
        return kStrRefDexterity;
    case Ability::Constitution:
        return kStrRefConstitution;
    case Ability::Intelligence:
        return kStrRefIntelligence;
    case Ability::Wisdom:
        return kStrRefWisdom;
    case Ability::Charisma:
        return kStrRefCharisma;
    }
    assert(false && "unsupported ability-drain feedback ability");
    return 0;
}

static void addAbilityDrainFeedback(
    Game &game,
    ServicesView &services,
    const std::shared_ptr<Object> &creator,
    const Object &target,
    ArrayRef<AbilityDrainFeedback> feedbackRecords) {

    if (feedbackRecords.empty()) {
        return;
    }

    auto leader = game.party().getLeader();
    const auto *targetCreature = dyn_cast<Creature>(&target);
    if (!leader || !targetCreature) {
        return;
    }

    bool targetHasRecipient = targetCreature == leader.get();
    bool creatorHasRecipient = creator.get() == leader.get();
    if (!targetHasRecipient && !creatorHasRecipient) {
        return;
    }

    for (const AbilityDrainFeedback &feedback : feedbackRecords) {
        std::string text = getFeedbackString(
            game,
            services,
            kStrRefAbilityDrain,
            {
                {0, target.name()},
                {1, services.resource.strings.getText(
                        getAbilityStrRef(feedback.ability))},
                {2, std::to_string(feedback.amount)},
                {3, std::to_string(feedback.durationSeconds)},
            });
        if (targetHasRecipient) {
            game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                text);
        }
        if (creatorHasRecipient) {
            game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                text);
        }
    }
}


std::optional<int> getItemOnHitEffectOutcomeType(
    ItemOnHitSubtype subtype) {

    switch (subtype) {
    case ItemOnHitSubtype::Confusion:
        return 1;
    case ItemOnHitSubtype::Fear:
        return 2;
    case ItemOnHitSubtype::Stun:
        return 4;
    case ItemOnHitSubtype::Paralyze:
        return 5;
    case ItemOnHitSubtype::Sleep:
        return 6;
    case ItemOnHitSubtype::Slow:
    case ItemOnHitSubtype::AbilityDrain:
    case ItemOnHitSubtype::ItemPoison:
    case ItemOnHitSubtype::SlayRG:
    case ItemOnHitSubtype::SlayAG:
    case ItemOnHitSubtype::InstantDeath:
    case ItemOnHitSubtype::Knockdown:
        return std::nullopt;
    }
    assert(false && "unsupported item on-hit subtype");
    return std::nullopt;
}

static bool hasItemOnHitImmunity(const Creature &target, ItemOnHitSubtype subtype,
                                const std::shared_ptr<Object> &source) {
    const auto *creator = dyn_cast<Creature>(source.get());
    switch (subtype) {
    case ItemOnHitSubtype::Sleep: return hasStateImmunity(target, CreatureState::Sleep, creator);
    case ItemOnHitSubtype::Stun: return hasStateImmunity(target, CreatureState::Stun, creator);
    case ItemOnHitSubtype::Paralyze: return hasStateImmunity(target, CreatureState::Paralysis, creator);
    case ItemOnHitSubtype::Confusion: return hasStateImmunity(target, CreatureState::Confusion, creator);
    case ItemOnHitSubtype::Fear: return hasStateImmunity(target, CreatureState::Fear, creator);
    case ItemOnHitSubtype::Slow: return hasSlowImmunity(target, creator);
    case ItemOnHitSubtype::AbilityDrain: return target.hasEffectImmunity(ImmunityType::AbilityDecrease, creator);
    case ItemOnHitSubtype::ItemPoison: return target.hasEffectImmunity(ImmunityType::Poison, creator);
    case ItemOnHitSubtype::InstantDeath: return target.hasEffectImmunity(ImmunityType::Death, creator);
    case ItemOnHitSubtype::Knockdown:
    case ItemOnHitSubtype::SlayRG: case ItemOnHitSubtype::SlayAG: return false;
    }
    return false;
}

static void applyStandalone(Object &target, const std::shared_ptr<Effect> &effect,
                            const std::shared_ptr<Object> &creator,
                            DurationType type, float duration = 0.0f) {
    effect->setSaveFacingCreator(creator);
    effect->setSubType(0);
    target.applyEffect(effect, type, duration);
}

static EffectInstance prepareOnHitEffect(Object &target, const std::shared_ptr<Effect> &effect,
                                        const std::shared_ptr<Object> &creator, float duration,
                                        DurationType type = DurationType::Temporary,
                                        uint16_t category = 0x18) {
    effect->setSaveFacingCreator(creator);
    auto record = effect->saveFacingInstance();
    record.effect = effect;
    record.id = target.game().allocateEffectId();
    record.setDuration(type, duration);
    record.subType = static_cast<uint16_t>((record.subType & ~uint16_t(0x18)) | (category & 0x18));
    record.exposed = 1;
    return record;
}

static void applyItemOnHitApplication(const ItemOnHitApplication &application, Object &target,
                                     Game &game, ServicesView &services) {
    auto *creature = dyn_cast<Creature>(&target);
    if (!creature || creature->isDead()) return;
    const auto creator = application.creator.resolve();
    StateApplicationResult result = StateApplicationResult::Rejected;
    CreatureState state = CreatureState::None;
    switch (application.subtype) {
    case ItemOnHitSubtype::Sleep: state = CreatureState::Sleep; break;
    case ItemOnHitSubtype::Stun: state = CreatureState::Stun; break;
    case ItemOnHitSubtype::Paralyze: state = CreatureState::Paralysis; break;
    case ItemOnHitSubtype::Confusion: state = CreatureState::Confusion; break;
    case ItemOnHitSubtype::Fear: state = CreatureState::Fear; break;
    case ItemOnHitSubtype::Slow: applySlowPackage(target, application.duration, creator); break;
    case ItemOnHitSubtype::AbilityDrain: {
        if (application.parameter < 0 || application.parameter > 5 ||
            hasItemOnHitImmunity(*creature, application.subtype, creator)) return;
        applyStandalone(target, std::make_shared<VisualEffectMarkerEffect>(91), creator, DurationType::Temporary, 30.0f);
        auto record = prepareOnHitEffect(target, std::make_shared<AbilityDecreaseEffect>(
            static_cast<Ability>(application.parameter), 1), creator, 30.0f);
        target.applyEffect(record);
        target.applyEffect(record.linkedChild(std::make_shared<EffectIconMarkerEffect>(28)));
        break;
    }
    case ItemOnHitSubtype::ItemPoison: {
        auto record = prepareOnHitEffect(target,
            std::make_shared<PoisonEffect>(static_cast<Poison>(application.parameter)),
            creator, 0.0f, DurationType::Permanent, 0x08);
        // The linked icon is submitted even if poison admission queues removal.
        // Both leaves share the ID, so that removal also removes the icon.
        target.applyEffect(record);
        target.applyEffect(record.linkedChild(std::make_shared<EffectIconMarkerEffect>(23)));
        break;
    }
    case ItemOnHitSubtype::SlayRG:
        // The record captured the after-save race qualification already.
        applyStandalone(target, std::make_shared<VisualEffectMarkerEffect>(50), creator, DurationType::Instant);
        applyStandalone(target, std::make_shared<DeathEffect>(false, true, false), creator, DurationType::Instant);
        break;
    case ItemOnHitSubtype::Knockdown:
        if (game.isTSL())
            applyStandalone(target, std::make_shared<ForcePushedEffect>(), creator,
                            DurationType::Temporary, 0.1f);
        break;
    case ItemOnHitSubtype::SlayAG:
        // No additional payload; selection still consumes chance and save checks.
        break;
    case ItemOnHitSubtype::InstantDeath:
        if (!hasItemOnHitImmunity(*creature, application.subtype, creator))
            applyStandalone(target, std::make_shared<DeathEffect>(false, true, false), creator, DurationType::Instant);
        break;
    }
    if (state != CreatureState::None) {
        result = applyStatePackage(target, state, application.duration, creator);
        if (result == StateApplicationResult::Applied && application.emitEffectOutcome) {
            addEffectOutcomeFeedback(game, services, target.name(), application.effectOutcome,
                                     getCombatFeedbackBroadcastCount(game, creator, target));
        } else if (result == StateApplicationResult::Immune &&
                   hasStateSpecificImmunity(*creature, state, dyn_cast<Creature>(creator.get()))) {
            addStateImmunityFeedback(game, services, creator, target, state);
        }
    }
}

void applyItemOnHitApplications(std::vector<ItemOnHitApplication> applications, Object &target,
                               Game &game, ServicesView &services) {
    for (const auto &application : applications) applyItemOnHitApplication(application, target, game, services);
}
void addDeferredCombatFeedback(
    Game &game,
    ServicesView &services,
    const std::shared_ptr<Object> &attacker,
    const Object &target,
    const std::vector<DeferredCombatFeedback> &records) {

    for (const DeferredCombatFeedback &record : records) {
        std::visit(
            [&](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, AbilityDrainFeedback>) {
                    addAbilityDrainFeedback(
                        game,
                        services,
                        attacker,
                        target,
                        ArrayRef<AbilityDrainFeedback>(&value, 1));
                } else if constexpr (std::is_same_v<
                                         Value,
                                         SavingThrowFeedback>) {
                    if (const auto *creature = dyn_cast<Creature>(&target)) {
                        addSavingThrowFeedback(
                            game,
                            services,
                            *creature,
                            value.savingThrow,
                            SavingThrowBreakdown {
                                value.base,
                                value.modifier,
                            },
                            value.roll,
                            value.difficultyClass);
                    }

                }
            },
            record);
    }
}

} // namespace reone::game
