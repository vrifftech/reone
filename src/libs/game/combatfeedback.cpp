/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/combatfeedback.h"

#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/savedruntime.h"
#include "reone/system/arrayref.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

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
static constexpr int kStrRefDeathExperience = 1407;

static bool canReceiveCombatFeedback(
    const Creature &player,
    const Creature &subject) {

    return player.faction() == subject.faction() &&
           player.getSquareDistanceTo(subject) <= kCombatFeedbackRange2;
}

void addHitPointHealingFeedback(Game &game, ServicesView &services,
                                const std::shared_ptr<Object> &creator, const Creature &target, int amount) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x97: target first, then a different creature creator.
    if (leader.get() != &target && leader.get() != creator.get()) return;
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal,
        game.getFeedbackText(1495, {{0, target.name()}, {1, std::to_string(amount)}}));
}

void addBlindnessImmunityFeedback(Game &game, ServicesView &services,
                                 const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x8b uses GUI string 0 rather than a Blindness-specific message.
    // Preserve creator-first/target-second requests, including self-targeting.
    const auto text = game.getFeedbackText(0, {{0, target.name()}});
    if (leader.get() == creator.get())
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
    if (leader.get() == &target)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
}

void addForceHealingFeedback(Game &game, ServicesView &services,
                             const Creature &target, int amount) {
    const auto leader = game.party().getLeader();
    if (!leader || !canReceiveCombatFeedback(*leader, target)) return;
    const auto text = game.getFeedbackText(1031,
        {{0, target.name()}, {1, std::to_string(amount)}});
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
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
    const Object *source,
    SavingThrow savingThrow,
    const SavingThrowBreakdown &breakdown,
    int roll,
    int difficultyClass) {

    auto leader = game.party().getLeader();
    if (!leader) return;
    // The saver and a different creature it saved against are both sent the line.
    const bool saverHasRecipient = leader.get() == &target;
    const bool sourceHasRecipient = source && source != &target && dyn_cast<Creature>(source) &&
                                    leader.get() == source;
    if (!saverHasRecipient && !sourceHasRecipient) {
        return;
    }

    std::string components;
    if (breakdown.base != 0) {
        components += game.getFeedbackText(kStrRefSavingThrowBaseComponent,
            {{0, std::to_string(breakdown.base)}});
    }
    if (breakdown.modifier != 0) {
        components += game.getFeedbackText(kStrRefSavingThrowEffectComponent,
            {{0, std::to_string(breakdown.modifier)}});
    }

    int total = roll + breakdown.total();
    std::string text = game.getFeedbackText(kStrRefSavingThrow,
        {
            {0, target.name()},
            {1, game.getFeedbackText(
                    getSavingThrowStrRef(savingThrow))},
            {2, game.getFeedbackText(
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
        std::move(text),
        MessageLog::Buffer::Combat);
}

void addEntangleImmunityFeedback(
    Game &game, ServicesView &services,
    const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x90 maps to GUI string 1491 and is requested for
    // creator first, then target, without self-target deduplication.
    const auto text = game.getFeedbackText(1491, {{0, target.name()}});
    if (leader.get() == creator.get())
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
    if (leader.get() == &target)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
}

// The GUI string naming the target's immunity to a state. Droid confusion has
// none: its immunity is reported with no text.
static std::optional<int> getStateImmunityStrRef(CreatureState state) {
    switch (state) {
    case CreatureState::Fear: return 1481;
    case CreatureState::Knockdown: return 1482;
    case CreatureState::Paralysis: return 1483;
    case CreatureState::MindTrick: return 1485;
    case CreatureState::Sleep: return 1488;
    case CreatureState::Confusion: return 1489;
    case CreatureState::Stun: return 1490;
    case CreatureState::Horrified: return 42531;
    default: return std::nullopt;
    }
}

void addStateImmunityFeedback(
    Game &game, ServicesView &services,
    const std::shared_ptr<Object> &creator, const Creature &target, CreatureState state) {
    const auto leader = game.party().getLeader();
    const auto strRef = getStateImmunityStrRef(state);
    if (!leader || !strRef) return;
    // The line names the target; the creator is told first, then the target,
    // without deduplication.
    const auto text = game.getFeedbackText(*strRef, {{0, target.name()}});
    if (leader.get() == creator.get())
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
    if (leader.get() == &target)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
}

void addSlowImmunityFeedback(
    Game &game, ServicesView &services,
    const std::shared_ptr<Object> &creator, const Creature &target) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // Feedback 0x92 sets token 0 to the target name and uses GUI 1492.
    // Requests go to the creator first, then the target, without deduplication.
    const bool creatorHasRecipient = creator.get() == leader.get();
    const bool targetHasRecipient = &target == leader.get();
    if (!creatorHasRecipient && !targetHasRecipient) return;
    const auto text = game.getFeedbackText(1492, {{0, target.name()}});
    if (creatorHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
    if (targetHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
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

    std::string text = game.getFeedbackText(kStrRefDamageImmunity,
        {
            {0, target.name()},
            {1, game.getFeedbackText(
                    kStrRefPoisonDamage)},
        });
    if (targetHasRecipient) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            text,
            MessageLog::Buffer::Combat);
    }
    if (creatorHasRecipient) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            text,
            MessageLog::Buffer::Combat);
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
            game.getFeedbackText(kStrRefPoisoned,
                {
                    {0, target.name()},
                    {1, game.getFeedbackText(
                            poisonNameStrRef)},
                }),
            MessageLog::Buffer::Combat);
    }
    if (leader->getSquareDistanceTo(target) <= kCombatFeedbackRange2) {
        target.playSound(resource::SoundSetEntry::Poisoned);
    }
}

void addSpellImmunityFeedback(Game &game, ServicesView &services, const Creature &target, const Object &caster,
                              const std::string &spellName) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    const bool targetHasRecipient = leader.get() == &target;
    const bool casterHasRecipient = leader.get() == &caster;
    if (!targetHasRecipient && !casterHasRecipient) return;
    const auto text = game.getFeedbackText(kStrRefDamageImmunity,
                                        {{0, target.name()}, {1, spellName}});
    if (targetHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
    if (casterHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
}

void addDispelFeedback(Game &game, ServicesView &services, const Object &target, const Object *caster,
                       const std::vector<uint32_t> &spellIds) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    const bool targetHasRecipient = leader.get() == &target;
    const bool casterHasRecipient = caster && caster != &target && leader.get() == caster;
    if (!targetHasRecipient && !casterHasRecipient) return;
    std::string text = game.getFeedbackText(0) + " : " + target.name() + " : ";
    for (size_t i = 0; i < spellIds.size(); ++i) {
        if (i != 0) text += ", ";
        text += services.game.spells.get(static_cast<SpellType>(spellIds[i]))->name;
    }
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                          MessageLog::Buffer::Combat);
}

void addDeathExperienceFeedback(Game &game, ServicesView &services, const std::string &recipientName,
                                const std::string &victimName, int reportedExperience) {
    game.messageLog().add(
        MessageLog::kFeedbackMessageType,
        MessageLog::Style::Normal,
        game.getFeedbackText(kStrRefDeathExperience,
                          {{0, recipientName}, {1, victimName}, {2, std::to_string(reportedExperience)}}),
        MessageLog::Buffer::Combat);
}


namespace {

/**
 * Localized message template produced for one mitigation-feedback record.
 * Custom tokens are stored in token-number order, beginning at zero.
 */
struct MitigationFeedbackMessage {
    int strRef {-1};
    std::array<std::string, 3> customTokens {};
    std::size_t customTokenCount {0};
};

} // namespace

static constexpr int kStrRefDamageResistance = 1454;
static constexpr int kStrRefDamageReduction = 1455;
static constexpr int kStrRefFiniteDamageResistance = 1456;
static constexpr int kStrRefFiniteDamageReduction = 1457;
static constexpr int kStrRefObjectNameFallback = 649;
static constexpr int kStrRefUniversalDamage = 1422;
static constexpr int kStrRefPhysicalDamage = 1423;
static constexpr int kStrRefAcidDamage = 1440;
static constexpr int kStrRefColdDamage = 1441;
static constexpr int kStrRefLightSideDamage = 1442;
static constexpr int kStrRefElectricalDamage = 1443;
static constexpr int kStrRefFireDamage = 1444;
static constexpr int kStrRefDarkSideDamage = 1445;
static constexpr int kStrRefSonicDamage = 1446;
static constexpr int kStrRefIonDamage = 1447;
static constexpr int kStrRefEnergyDamage = 1448;

static std::string resolveMitigationTargetName(
    const Game &game,
    std::optional<std::string> targetName) {

    if (targetName) {
        return std::move(*targetName);
    }
    return game.getFeedbackText(kStrRefObjectNameFallback);
}

static std::string getMitigationDamageTypeName(
    Game &game,
    int damageFlags) {

    int strRef = -1;
    if ((damageFlags & static_cast<int>(DamageType::Physical)) != 0) {
        strRef = kStrRefPhysicalDamage;
    } else {
        static constexpr std::pair<int, int> kDamageTypes[] {
            {0x0008, kStrRefUniversalDamage},
            {0x0010, kStrRefAcidDamage},
            {0x0020, kStrRefColdDamage},
            {0x0040, kStrRefLightSideDamage},
            {0x0080, kStrRefElectricalDamage},
            {0x0100, kStrRefFireDamage},
            {0x0200, kStrRefDarkSideDamage},
            {0x0400, kStrRefSonicDamage},
            {0x0800, kStrRefIonDamage},
            {0x1000, kStrRefEnergyDamage},
            {0x2000, kStrRefPoisonDamage},
        };
        for (const auto &[flag, candidateStrRef] : kDamageTypes) {
            if ((damageFlags & flag) != 0) {
                strRef = candidateStrRef;
                break;
            }
        }
    }
    if (strRef < 0) {
        return {};
    }

    // Damage-family strings contain a CUSTOM0 placeholder used by other
    // feedback contexts. This message supplies that token as an empty string.
    return game.getFeedbackText(strRef, {{0, std::string()}});
}

/**
 * Builds the localized-template payload consumed by the message log.
 *
 * Immunity uses target and damage-type tokens. Ordinary mitigation uses
 * target and amount, while finite mitigation adds the remaining pool.
 *
 * @param targetName Resolved object name, or std::nullopt when object-name
 * resolution failed. Failed resolution uses the unknown-object string
 * (dialog.tlk 649); a resolved empty name remains empty.
 */
static MitigationFeedbackMessage buildMitigationFeedbackMessage(
    Game &game,
    std::optional<std::string> targetName,
    const MitigationFeedback &feedback) {

    std::string resolvedTargetName = resolveMitigationTargetName(
        game,
        std::move(targetName));

    switch (feedback.type) {
    case MitigationFeedbackType::DamageImmunity:
        return {
            kStrRefDamageImmunity,
            {
                std::move(resolvedTargetName),
                getMitigationDamageTypeName(game, feedback.damageFlags),
                {},
            },
            2,
        };
    case MitigationFeedbackType::DamageResistance:
        return {
            kStrRefDamageResistance,
            {
                std::move(resolvedTargetName),
                std::to_string(feedback.amount),
                {},
            },
            2,
        };
    case MitigationFeedbackType::DamageReduction:
        return {
            kStrRefDamageReduction,
            {
                std::move(resolvedTargetName),
                std::to_string(feedback.amount),
                {},
            },
            2,
        };
    case MitigationFeedbackType::FiniteDamageResistance:
        assert(feedback.remaining &&
               "finite damage-resistance feedback has no remainder");
        return {
            kStrRefFiniteDamageResistance,
            {
                std::move(resolvedTargetName),
                std::to_string(feedback.amount),
                std::to_string(feedback.remaining.value_or(0)),
            },
            3,
        };
    case MitigationFeedbackType::FiniteDamageReduction:
        assert(feedback.remaining &&
               "finite damage-reduction feedback has no remainder");
        return {
            kStrRefFiniteDamageReduction,
            {
                std::move(resolvedTargetName),
                std::to_string(feedback.amount),
                std::to_string(feedback.remaining.value_or(0)),
            },
            3,
        };
    }
    assert(false && "invalid mitigation feedback type");
    return {};
}

// Shared combat-message TLK identities; resolve through each title's resources.
static constexpr int kEffectOutcomePrefixStrRef = 42157;

static constexpr std::optional<int> getEffectOutcomeMessageStrRef(bool, int outcome) {
    if (outcome == -1 || outcome == 1) {
        return std::nullopt;
    }
    return outcome == 2 ? 42160 : 42158;
}

static constexpr int kStrRefAbilityDrain = 42408;
static constexpr int kStrRefStrength = 211;
static constexpr int kStrRefDexterity = 212;
static constexpr int kStrRefConstitution = 213;
static constexpr int kStrRefWisdom = 214;
static constexpr int kStrRefIntelligence = 215;
static constexpr int kStrRefCharisma = 216;

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

static constexpr int kStrRefDamageTaken = 1402;
static constexpr int kStrRefDamageDealt = 1403;

void addDamageFeedback(Game &game, ServicesView &services, const Object *creator, const Object &damaged,
                       const std::array<int, 15> &amounts) {
    const auto leader = game.party().getLeader();
    if (!leader) return;
    // A damaged creature tells its own side; a creator of another faction
    // tells its side too. A door or container leaves it to a creature
    // creator.
    const auto *creatorCreature = creator ? dyn_cast<const Creature>(creator) : nullptr;
    const auto *damagedCreature = dyn_cast<const Creature>(&damaged);
    int broadcasts = 0;
    if (damagedCreature) {
        broadcasts += static_cast<int>(canReceiveCombatFeedback(*leader, *damagedCreature));
        if (creatorCreature && creatorCreature->faction() != damagedCreature->faction())
            broadcasts += static_cast<int>(canReceiveCombatFeedback(*leader, *creatorCreature));
    } else if (creatorCreature) {
        broadcasts += static_cast<int>(canReceiveCombatFeedback(*leader, *creatorCreature));
    }
    if (broadcasts == 0) return;

    // The line names the creator and the damaged object, or only the damaged
    // object when the creator is gone, with the total; then, in parentheses,
    // the physical damage and each other damage type present.
    const auto total = std::to_string(amounts[DamageEffect::kTotal]);
    std::string text = creator
        ? game.getFeedbackText(kStrRefDamageDealt, {{0, creator->name()}, {1, damaged.name()}, {2, total}})
        : game.getFeedbackText(kStrRefDamageTaken, {{0, damaged.name()}, {1, total}});
    text += " (";
    bool first = true;
    const auto addPiece = [&](int strRef, int amount) {
        if (!first) text += " ";
        text += game.getFeedbackText(strRef, {{0, std::to_string(amount)}});
        first = false;
    };
    if (amounts[0] >= 0 || amounts[1] >= 0 || amounts[2] >= 0)
        addPiece(kStrRefPhysicalDamage, std::max(0, amounts[0]) + std::max(0, amounts[1]) + std::max(0, amounts[2]));
    static constexpr int kTypeStrRefs[] {
        kStrRefUniversalDamage, kStrRefAcidDamage, kStrRefColdDamage, kStrRefLightSideDamage,
        kStrRefElectricalDamage, kStrRefFireDamage, kStrRefDarkSideDamage, kStrRefSonicDamage,
        kStrRefIonDamage, kStrRefEnergyDamage, kStrRefPoisonDamage};
    for (size_t slot = 3; slot < DamageEffect::kTotal; ++slot) {
        if (amounts[slot] >= 0) addPiece(kTypeStrRefs[slot - 3], amounts[slot]);
    }
    text += ")";
    for (int i = 0; i < broadcasts; ++i)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
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
    std::map<int, std::string> tokens {{0, targetName}};
    if (const auto label = getEffectOutcomeLabelStrRef(game.isTSL(), values.effectType)) {
        tokens[1] = game.getFeedbackText(*label);
    }
    const std::string text = game.getFeedbackText(kEffectOutcomePrefixStrRef) + game.getFeedbackText(*message, tokens);
    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                              MessageLog::Buffer::Combat);
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
        std::string text = game.getFeedbackText(kStrRefAbilityDrain,
            {
                {0, target.name()},
                {1, game.getFeedbackText(
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

void addAttackEffectOutcomeFeedback(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                                    const Object &target, const EffectOutcomeBreakdown &values) {
    addEffectOutcomeFeedback(game, services, target.name(), values,
                             getCombatFeedbackBroadcastCount(game, attacker, target));
}

static constexpr int kStrRefWeaponIneffective = 1473;
static constexpr int kStrRefCriticalHitImmunity = 1480;
static constexpr int kStrRefSneakAttackImmunity = 1487;
static constexpr int kStrRefDamageImmunityLine = 1458;

// A target's immunity to the attacker's critical hit or sneak attack. Both
// the target and the attacker are sent the line; only the controlled creature
// displays it.
static void addTargetImmunityFeedback(
    Game &game,
    ServicesView &services,
    const std::shared_ptr<Object> &attacker,
    const Object &target,
    int strRef) {

    auto leader = game.party().getLeader();
    if (!leader) return;
    const bool targetHasRecipient = &target == leader.get();
    const bool attackerHasRecipient = attacker.get() == leader.get();
    if (!targetHasRecipient && !attackerHasRecipient) return;
    const std::string text = game.getFeedbackText(strRef, {{0, target.name()}});
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text,
                          MessageLog::Buffer::Combat);
}

// Only the attacker is told, once per round, that its weapon cannot hurt the target.
static void addWeaponIneffectiveFeedback(Game &game, ServicesView &services, const Object *attacker) {
    auto leader = game.party().getLeader();
    if (!leader || !attacker || attacker->id() != leader->id()) return;
    game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal,
                          game.getFeedbackText(kStrRefWeaponIneffective));
}

// One mitigation line names the damaged object, or the unknown-object string
// once it is gone. The damaged object and the other party are both sent the
// line; only the controlled creature displays it.
static void addMitigationLine(Game &game, ServicesView &services, const Object *damaged,
                              const Object *other, const MitigationFeedback &feedback) {
    auto leader = game.party().getLeader();
    if (!leader) return;
    const bool damagedHasRecipient = damaged && damaged->id() == leader->id();
    const bool otherHasRecipient = other && other->id() == leader->id();
    if (!damagedHasRecipient && !otherHasRecipient) return;
    const MitigationFeedbackMessage message = buildMitigationFeedbackMessage(
        game, damaged ? std::optional<std::string>(damaged->name()) : std::nullopt, feedback);
    std::map<int, std::string> tokens;
    for (std::size_t token = 0; token < message.customTokenCount; ++token) {
        tokens[static_cast<int>(token)] = message.customTokens[token];
    }
    const std::string text = game.getFeedbackText(message.strRef, tokens);
    // Damage immunity is a combat line; resistance and reduction are messages.
    const auto buffer = message.strRef == kStrRefDamageImmunityLine ? MessageLog::Buffer::Combat
                                                                    : MessageLog::Buffer::Messages;
    if (damagedHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text, buffer);
    if (otherHasRecipient)
        game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal, text, buffer);
}

// Delivers the lines of queued feedback-log messages. Recipients are the
// target and the attacker, each shown the line only while it is the
// controlled creature, so a door or placeable target leaves the attacker's
// line alone.
static void addDeferredCombatFeedback(
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
                } else if constexpr (std::is_same_v<Value, SneakAttackImmunityFeedback>) {
                    addTargetImmunityFeedback(game, services, attacker, target, kStrRefSneakAttackImmunity);
                } else if constexpr (std::is_same_v<Value, CriticalHitImmunityFeedback>) {
                    addTargetImmunityFeedback(game, services, attacker, target, kStrRefCriticalHitImmunity);
                } else if constexpr (std::is_same_v<Value, WeaponIneffectiveFeedback>) {
                    addWeaponIneffectiveFeedback(game, services, attacker.get());
                } else if constexpr (std::is_same_v<Value, MitigationFeedback>) {
                    addMitigationLine(game, services, &target, attacker.get(), value);
                } else if constexpr (std::is_same_v<Value, ReturnedMitigationFeedback>) {
                    addMitigationLine(game, services, attacker.get(), &target, value.feedback);
                } else if constexpr (std::is_same_v<
                                         Value,
                                         SavingThrowFeedback>) {
                    if (const auto *creature = dyn_cast<Creature>(&target)) {
                        addSavingThrowFeedback(
                            game,
                            services,
                            *creature,
                            attacker.get(),
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

// Feedback message types and identifiers of a queued feedback-log message.
static constexpr int kFeedbackToBoth = 0;
static constexpr int kFeedbackSavingThrow = 1;
static constexpr int kFeedbackToAttacker = 3;
static constexpr int kFeedbackToTarget = 4;
static constexpr int kFeedbackDamageImmunity = 62;
static constexpr int kFeedbackDamageResistance = 63;
static constexpr int kFeedbackDamageReduction = 64;
static constexpr int kFeedbackFiniteDamageResistance = 66;
static constexpr int kFeedbackFiniteDamageReduction = 67;
static constexpr int kFeedbackWeaponIneffective = 117;
static constexpr int kFeedbackCriticalHitImmunity = 126;
static constexpr int kFeedbackSneakAttackImmunity = 134;
static constexpr int kFeedbackAbilityDrain = 142;
// The structure of a queued message and of each entry of its lists.
static constexpr uint32_t kFeedbackMessageStructType = 0xcccc;
static constexpr uint32_t kFeedbackMessageEntryStructType = 0xbaad;

static std::shared_ptr<SavedStruct> makeFeedbackMessageEntry(
    const char *label, resource::Gff::FieldType type, SavedFieldValue value) {
    auto entry = std::make_shared<SavedStruct>();
    entry->type = kFeedbackMessageEntryStructType;
    entry->fields.push_back(SavedField {type, label, std::move(value)});
    return entry;
}

// A queued message: its type, its integers (the first names the message
// except in a saving throw), its floats and its objects. The string list is
// always present and empty.
static SavedFeedbackMessage makeFeedbackMessage(int type, const std::vector<int> &integers,
                                                const std::vector<float> &floats,
                                                const std::vector<uint32_t> &objects) {
    using FieldType = resource::Gff::FieldType;
    SavedFeedbackMessage message;
    message.data.type = kFeedbackMessageStructType;
    SavedStructChildren integerEntries, floatEntries, objectEntries;
    for (int value : integers)
        integerEntries.push_back(makeFeedbackMessageEntry("IntegerValue", FieldType::Int, int64_t {value}));
    for (float value : floats)
        floatEntries.push_back(makeFeedbackMessageEntry("FloatValue", FieldType::Float, double {value}));
    for (uint32_t id : objects) {
        objectEntries.push_back(makeFeedbackMessageEntry("ObjectValue", FieldType::Dword, uint64_t {id}));
        message.objects.push_back(SavedObjectReference::fromRuntimeId(id));
    }
    message.data.fields.push_back(SavedField {FieldType::Byte, "Type", static_cast<uint64_t>(type)});
    message.data.fields.push_back(SavedField {FieldType::List, "IntList", std::move(integerEntries)});
    message.data.fields.push_back(SavedField {FieldType::List, "FloatList", std::move(floatEntries)});
    message.data.fields.push_back(SavedField {FieldType::List, "ObjectIDList", std::move(objectEntries)});
    message.data.fields.push_back(SavedField {FieldType::List, "StringList", SavedStructChildren {}});
    return message;
}

// A mitigation line names the damaged object; finite pools add what remains.
static SavedFeedbackMessage makeMitigationMessage(const MitigationFeedback &feedback, uint32_t damaged) {
    switch (feedback.type) {
    case MitigationFeedbackType::DamageImmunity:
        return makeFeedbackMessage(kFeedbackToBoth, {kFeedbackDamageImmunity, feedback.amount, feedback.damageFlags},
                                   {}, {damaged});
    case MitigationFeedbackType::DamageResistance:
        return makeFeedbackMessage(kFeedbackToBoth, {kFeedbackDamageResistance, feedback.amount}, {}, {damaged});
    case MitigationFeedbackType::DamageReduction:
        return makeFeedbackMessage(kFeedbackToBoth, {kFeedbackDamageReduction, feedback.amount}, {}, {damaged});
    case MitigationFeedbackType::FiniteDamageResistance:
        return makeFeedbackMessage(kFeedbackToBoth,
            {kFeedbackFiniteDamageResistance, feedback.amount, feedback.remaining.value_or(0)}, {}, {damaged});
    case MitigationFeedbackType::FiniteDamageReduction:
        return makeFeedbackMessage(kFeedbackToBoth,
            {kFeedbackFiniteDamageReduction, feedback.amount, feedback.remaining.value_or(0)}, {}, {damaged});
    }
    assert(false && "invalid mitigation feedback type");
    return {};
}

SavedFeedbackMessage makeCombatFeedbackMessage(const DeferredCombatFeedback &record, const Object &target,
                                               const Object *attacker) {
    return std::visit(
        [&](const auto &value) -> SavedFeedbackMessage {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, AbilityDrainFeedback>) {
                return makeFeedbackMessage(kFeedbackToBoth,
                    {kFeedbackAbilityDrain, static_cast<int>(value.ability), value.amount},
                    {static_cast<float>(value.durationSeconds)}, {target.id()});
            } else if constexpr (std::is_same_v<Value, SavingThrowFeedback>) {
                // The category, subtype, roll, effect modifier, base save,
                // difficulty class and the saving object, the target.
                auto message = makeFeedbackMessage(kFeedbackSavingThrow,
                    {static_cast<int>(value.savingThrow), static_cast<int>(value.savingThrowType), value.roll,
                     value.modifier, value.base, value.difficultyClass, static_cast<int>(target.id())},
                    {}, {});
                message.saver = SavedObjectReference::fromRuntimeId(target.id());
                return message;
            } else if constexpr (std::is_same_v<Value, SneakAttackImmunityFeedback>) {
                return makeFeedbackMessage(kFeedbackToBoth, {kFeedbackSneakAttackImmunity}, {}, {target.id()});
            } else if constexpr (std::is_same_v<Value, CriticalHitImmunityFeedback>) {
                return makeFeedbackMessage(kFeedbackToBoth, {kFeedbackCriticalHitImmunity}, {}, {target.id()});
            } else if constexpr (std::is_same_v<Value, WeaponIneffectiveFeedback>) {
                return makeFeedbackMessage(kFeedbackToAttacker, {kFeedbackWeaponIneffective}, {}, {});
            } else if constexpr (std::is_same_v<Value, MitigationFeedback>) {
                return makeMitigationMessage(value, target.id());
            } else {
                // Only a round with an attacker returns a deflected shot.
                static_assert(std::is_same_v<Value, ReturnedMitigationFeedback>);
                return makeMitigationMessage(value.feedback, attacker->id());
            }
        },
        record);
}

void addSavedFeedbackMessage(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                             const Object &target, const SavedFeedbackMessage &message) {
    int type = kFeedbackToBoth;
    std::vector<int> values;
    std::vector<float> floats;
    for (const auto &field : message.data.fields) {
        if (field.label == "Type") {
            type = static_cast<int>(std::get<uint64_t>(field.value));
        } else if (field.label == "IntList") {
            for (const auto &item : std::get<SavedStructChildren>(field.value)) {
                for (const auto &value : item->fields) {
                    if (value.label == "IntegerValue") values.push_back(static_cast<int>(std::get<int64_t>(value.value)));
                }
            }
        } else if (field.label == "FloatList") {
            for (const auto &item : std::get<SavedStructChildren>(field.value)) {
                for (const auto &value : item->fields) {
                    if (value.label == "FloatValue") floats.push_back(static_cast<float>(std::get<double>(value.value)));
                }
            }
        }
    }
    const auto integer = [&](size_t index) { return index < values.size() ? values[index] : 0; };
    const auto floating = [&](size_t index) { return index < floats.size() ? floats[index] : 0.0f; };
    if (type == kFeedbackSavingThrow) {
        // The saving throw carries its category, subtype, roll, effect
        // modifier, base save and difficulty class.
        addDeferredCombatFeedback(game, services, attacker, target,
            {SavingThrowFeedback {static_cast<SavingThrow>(integer(0)), integer(4), integer(3), integer(2), integer(5),
                                  static_cast<SavingThrowType>(integer(1))}});
        return;
    }
    // Otherwise the first integer names the message and the rest are its values.
    const int id = integer(0);
    if (type == kFeedbackToAttacker) {
        if (id == kFeedbackWeaponIneffective) addWeaponIneffectiveFeedback(game, services, attacker.get());
        return;
    }
    // A mitigation line names its first object: the target, or the attacker
    // that its own deflected shot came back to.
    const auto damaged = message.objects.empty() ? nullptr : message.objects.front().boundObject();
    const bool returned = attacker && damaged == attacker;
    const auto mitigation = [&](MitigationFeedback feedback) -> DeferredCombatFeedback {
        if (returned) return ReturnedMitigationFeedback {feedback};
        return feedback;
    };
    std::vector<DeferredCombatFeedback> records;
    switch (id) {
    case kFeedbackDamageImmunity:
        records.push_back(mitigation({MitigationFeedbackType::DamageImmunity, integer(1), std::nullopt, integer(2)}));
        break;
    case kFeedbackDamageResistance:
        records.push_back(mitigation({MitigationFeedbackType::DamageResistance, integer(1), std::nullopt, 0}));
        break;
    case kFeedbackDamageReduction:
        records.push_back(mitigation({MitigationFeedbackType::DamageReduction, integer(1), std::nullopt, 0}));
        break;
    case kFeedbackFiniteDamageResistance:
        records.push_back(mitigation({MitigationFeedbackType::FiniteDamageResistance, integer(1), integer(2), 0}));
        break;
    case kFeedbackFiniteDamageReduction:
        records.push_back(mitigation({MitigationFeedbackType::FiniteDamageReduction, integer(1), integer(2), 0}));
        break;
    case kFeedbackCriticalHitImmunity:
        records.emplace_back(CriticalHitImmunityFeedback {});
        break;
    case kFeedbackSneakAttackImmunity:
        records.emplace_back(SneakAttackImmunityFeedback {});
        break;
    case kFeedbackAbilityDrain:
        // The ability, the decrease and the duration in seconds, shown whole.
        records.emplace_back(AbilityDrainFeedback {
            static_cast<Ability>(integer(1)), integer(2), static_cast<int>(floating(0))});
        break;
    default:
        break;
    }
    // A door or placeable passes every line on to the attacker.
    const bool toTargetOnly = type == kFeedbackToTarget && dyn_cast<Creature>(&target);
    addDeferredCombatFeedback(game, services, toTargetOnly ? nullptr : attacker, target, records);
}

void showCombatFeedback(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                        const Object &target, const std::vector<DeferredCombatFeedback> &records) {
    for (const auto &record : records) {
        auto message = makeCombatFeedbackMessage(record, target, attacker.get());
        for (auto &object : message.objects) game.bindSavedObjectReference(object);
        addSavedFeedbackMessage(game, services, attacker, target, message);
    }
}

} // namespace reone::game
