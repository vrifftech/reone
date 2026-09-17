/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effectfeedback.h"

#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"

#include <cassert>

namespace reone::game {
static constexpr float kCombatFeedbackRange2 = 900.0f;
static constexpr int kStrRefSavingThrow = 1406;
static constexpr int kStrRefSavingThrowFortitude = 1375;
static constexpr int kStrRefSavingThrowReflex = 1374;
static constexpr int kStrRefSavingThrowWill = 1376;
static constexpr int kStrRefSavingThrowSuccess = 1392;
static constexpr int kStrRefSavingThrowFailure = 1393;
static constexpr int kStrRefSavingThrowBaseComponent = 42387;
static constexpr int kStrRefSavingThrowEffectComponent = 42388;
static constexpr int kStrRefDamageImmunity = 1458;
static constexpr int kStrRefPoisonDamage = 41902;
static constexpr int kStrRefPoisoned = 47873;

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

void addHitPointHealingFeedback(Game &game, ServicesView &services,
                                const std::shared_ptr<Object> &creator, const Creature &target, int amount) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x97: target first, then a different creature creator.
    if (leader.get() != &target && leader.get() != dyn_cast<Creature>(creator.get())) return;
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal,
        getFeedbackString(game, services, 1495, {{0, target.name()}, {1, std::to_string(amount)}}));
}

void addBlindnessImmunityFeedback(Game &game, ServicesView &services,
                                 const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x8b uses GUI string 0 rather than a Blindness-specific message.
    // Preserve creator-first/target-second requests, including self-targeting.
    const auto text = getFeedbackString(game, services, 0, {{0, target.name()}});
    if (leader.get() == dyn_cast<Creature>(creator.get()))
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
    if (leader.get() == &target)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
}

void addForceHealingFeedback(Game &game, ServicesView &services,
                             const Creature &target, int amount) {
    const auto leader = game.party().getLeader();
    if (!leader || !canReceiveCombatFeedback(*leader, target)) return;
    const auto text = getFeedbackString(game, services, 1031,
        {{0, target.name()}, {1, std::to_string(amount)}});
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
}

static int getSavingThrowStrRef(SavingThrow savingThrow) {
    switch (savingThrow) {
    case SavingThrow::Fortitude:
        return kStrRefSavingThrowFortitude;
    case SavingThrow::Reflex:
        return kStrRefSavingThrowReflex;
    case SavingThrow::Will:
        return kStrRefSavingThrowWill;
    case SavingThrow::None:
        break;
    }
    assert(false && "unsupported saving-throw feedback type");
    return 0;
}

void addSavingThrowFeedback(
    Game &game,
    ServicesView &services,
    const Creature &target,
    SavingThrow savingThrow,
    const SavingThrowBreakdown &breakdown,
    int roll,
    int difficultyClass) {

    auto leader = game.party().getLeader();
    if (!leader || leader.get() != &target) {
        return;
    }

    std::string components;
    if (breakdown.base != 0) {
        components += getFeedbackString(
            game,
            services,
            kStrRefSavingThrowBaseComponent,
            {{0, std::to_string(breakdown.base)}});
    }
    if (breakdown.modifier != 0) {
        components += getFeedbackString(
            game,
            services,
            kStrRefSavingThrowEffectComponent,
            {{0, std::to_string(breakdown.modifier)}});
    }

    int total = roll + breakdown.total();
    std::string text = getFeedbackString(
        game,
        services,
        kStrRefSavingThrow,
        {
            {0, target.name()},
            {1, services.resource.strings.getText(
                    getSavingThrowStrRef(savingThrow))},
            {2, services.resource.strings.getText(
                    total >= difficultyClass
                        ? kStrRefSavingThrowSuccess
                        : kStrRefSavingThrowFailure)},
            {3, std::to_string(total)},
            {4, std::to_string(roll)},
            {5, components},
            {6, std::to_string(difficultyClass)},
        });
    game.messageLog().add(
        MessageLog::kFeedbackMessageType,
        MessageLog::Style::Normal,
        std::move(text));
}

void addEntangleImmunityFeedback(
    Game &game, ServicesView &services,
    const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x90 maps to GUI string 1490 and is requested for
    // creator first, then target, without self-target deduplication.
    const auto text = getFeedbackString(game, services, 1490, {{0, target.name()}});
    if (leader.get() == dyn_cast<Creature>(creator.get()))
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
    if (leader.get() == &target)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
}

void addSlowImmunityFeedback(
    Game &game, ServicesView &services,
    const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x92 sets token 0 to the target name and uses GUI 1492.
    // Requests go to the creator first, then the target, without deduplication.
    const bool creatorHasRecipient = dyn_cast<Creature>(creator.get()) == leader.get();
    const bool targetHasRecipient = &target == leader.get();
    if (!creatorHasRecipient && !targetHasRecipient) return;
    const auto text = getFeedbackString(game, services, 1492, {{0, target.name()}});
    if (creatorHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
    if (targetHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text);
}

void addPoisonImmunityFeedback(
    Game &game,
    ServicesView &services,
    const std::shared_ptr<Object> &creator,
    const Creature &target) {

    auto leader = game.party().getLeader();
    if (!leader) {
        return;
    }

    bool targetHasRecipient = &target == leader.get();
    bool creatorHasRecipient = creator.get() == leader.get();
    if (!targetHasRecipient && !creatorHasRecipient) {
        return;
    }

    std::string text = getFeedbackString(
        game,
        services,
        kStrRefDamageImmunity,
        {
            {0, target.name()},
            {1, services.resource.strings.getText(
                    kStrRefPoisonDamage)},
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

void addPoisonedFeedback(
    Game &game,
    ServicesView &services,
    Creature &target,
    int poisonNameStrRef) {

    auto leader = game.party().getLeader();
    if (!leader) {
        return;
    }

    if (canReceiveCombatFeedback(*leader, target)) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            getFeedbackString(
                game,
                services,
                kStrRefPoisoned,
                {
                    {0, target.name()},
                    {1, services.resource.strings.getText(
                            poisonNameStrRef)},
                }));
    }
    if (leader->getSquareDistanceTo(target) <= kCombatFeedbackRange2) {
        target.playSound(resource::SoundSetEntry::Poisoned);
    }
}

} // namespace reone::game
