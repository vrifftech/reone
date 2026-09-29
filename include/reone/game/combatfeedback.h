/*
 * Copyright (c) 2025 The reone project contributors
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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "reone/game/effect/damage.h"
#include "reone/game/types.h"

namespace reone {
namespace game {

class Game;
class Creature;
class Object;
struct ServicesView;
struct SavingThrowBreakdown;
struct SavedFeedbackMessage;
enum class CreatureState : int;

/** Stores the displayed values for one ability-drain result. */
struct AbilityDrainFeedback {
    /** The affected ability. */
    Ability ability {Ability::Strength};
    /** The ability-score decrease. */
    int amount {0};
    /** The effect duration in seconds. */
    int durationSeconds {0};
};

/** Stores the displayed values for one saving throw. */
struct SavingThrowFeedback {
    /** The saving throw category. */
    SavingThrow savingThrow {SavingThrow::None};
    /** The base saving throw value. */
    int base {0};
    /** The total saving throw modifier. */
    int modifier {0};
    /** The d20 roll. */
    int roll {0};
    /** The difficulty class. */
    int difficultyClass {0};
    /** The subtype the saving throw was made against. */
    SavingThrowType savingThrowType {SavingThrowType::All};
};

/** Reports a target immune to the attacker's sneak attack. */
struct SneakAttackImmunityFeedback {
};

/** Reports a confirmed critical hit that the target's immunity turned into an ordinary hit. */
struct CriticalHitImmunityFeedback {
};

/** Tells the attacker that its ranged weapon cannot hurt the target. */
struct WeaponIneffectiveFeedback {
};

/**
 * One damage mitigation line about the attacker itself: its own defenses
 * against the damage its deflected shot brought back. The line names the
 * attacker.
 */
struct ReturnedMitigationFeedback {
    MitigationFeedback feedback;
};

/**
 * Represents feedback that a round gathers while its attacks resolve and
 * sends with its first release, one queued feedback-log message each: a
 * ranged round's first discharge, or a melee round's first hit. Damage
 * mitigation lines are among them.
 */
using DeferredCombatFeedback = std::variant<
    AbilityDrainFeedback,
    SavingThrowFeedback,
    SneakAttackImmunityFeedback,
    MitigationFeedback,
    CriticalHitImmunityFeedback,
    WeaponIneffectiveFeedback,
    ReturnedMitigationFeedback>;

/** Stores effect-outcome values for combat feedback. */
struct EffectOutcomeBreakdown {
    /** True if the record contains an effect outcome; false otherwise. */
    bool present {false};
    /** The saving throw category used by the effect. */
    int saveType {0};
    /** The effect subtype displayed in feedback. */
    int effectType {0};
    /** The effect-specific saving throw mode. */
    int saveMode {0};
    /** The d20 roll. */
    int saveRoll {0};
    /** The total saving throw modifier. */
    int modifierTotal {0};
    /** The base saving throw value. */
    int baseSave {0};
    /** The final saving throw value. */
    int finalTotal {0};
    /** The difficulty class. */
    int difficultyClass {0};
    /** The encoded effect outcome, or -1 if no outcome is available. */
    int outcome {-1};
};

constexpr std::optional<int> getEffectOutcomeLabelStrRef(bool tsl, int effectType) {
    // Type zero leaves CUSTOM1 untouched in the handler.
    if (effectType == 0) {
        return std::nullopt;
    }
    constexpr int labels[] {0, 42031, 42035, 42036, 42032, 42037,
                            42038, 42033, 42040, 42034, 42039};
    if (effectType > 0 && effectType <= 10) return labels[effectType];
    // TSL adds knockdown (14) and four more; the switch fetches StrRef 0 for
    // any other nonzero type.
    if (tsl) {
        switch (effectType) {
        case 14: return 48178;
        case 15: return 48541;
        case 16: return 42031;
        case 18: return 113790;
        case 19: return 124290;
        default: break;
        }
    }
    return 0;
}

/** The effect-application line of a hit whose feat attempted a saving throw. */
void addAttackEffectOutcomeFeedback(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                                    const Object &target, const EffectOutcomeBreakdown &values);
/**
 * The queued feedback-log message that carries one piece of a round's
 * feedback from the attacker to the target.
 */
SavedFeedbackMessage makeCombatFeedbackMessage(const DeferredCombatFeedback &record, const Object &target,
                                               const Object *attacker);
/**
 * A queued feedback-log message on the target. Its type sends it to both
 * sides (0), the attacker (3), the target (4), or reports a saving throw (1).
 * A door or placeable target passes every line to the attacker.
 */
void addSavedFeedbackMessage(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                             const Object &target, const SavedFeedbackMessage &message);
/**
 * Feedback-log messages sent at once rather than queued: each piece goes to
 * the target and to the attacker, if any, as a queued message would.
 */
void showCombatFeedback(Game &game, ServicesView &services, const std::shared_ptr<Object> &attacker,
                        const Object &target, const std::vector<DeferredCombatFeedback> &records);

/**
 * A saving-throw line about the saver, shown to the saver and to the creature
 * the throw was made against, whichever is the controlled creature.
 */
void addSavingThrowFeedback(Game &, ServicesView &, const Creature &saver, const Object *source, SavingThrow,
                           const SavingThrowBreakdown &, int roll, int difficultyClass);
void addPoisonImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addSlowImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addEntangleImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
/**
 * A target's immunity to a state, told to the creator and to the target,
 * whichever is the controlled creature.
 */
void addStateImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &creator, const Creature &target,
                              CreatureState);
/**
 * The spell-immunity line (1458): the target and the name of the spell it is
 * immune to, told to the target and then to the caster, whichever is the
 * controlled creature.
 */
void addSpellImmunityFeedback(Game &, ServicesView &, const Creature &target, const Object &caster,
                              const std::string &spellName);
void addHitPointHealingFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &, int amount);
void addBlindnessImmunityFeedback(Game &, ServicesView &, const std::shared_ptr<Object> &, const Creature &);
void addForceHealingFeedback(Game &, ServicesView &, const Creature &, int amount);
void addPoisonedFeedback(Game &, ServicesView &, Creature &, int poisonNameStrRef);
/**
 * The dispel line: TLK 0, the target and the names of the powers dispelled
 * from it, told to the target and to a different caster (when one is given),
 * whichever is the controlled creature.
 */
void addDispelFeedback(Game &, ServicesView &, const Object &target, const Object *caster,
                       const std::vector<uint32_t> &spellIds);
/**
 * The death-experience line (1407): the recipient, the victim and the award
 * as reported. TSL writes it to the combat list, KotOR to its only list.
 */
void addDeathExperienceFeedback(Game &, ServicesView &, const std::string &recipientName,
                                const std::string &victimName, int reportedExperience);

} // namespace game
} // namespace reone
