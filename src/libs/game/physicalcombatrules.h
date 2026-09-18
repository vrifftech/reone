/*
 * Copyright (c) 2026 The reone project contributors
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

#include "reone/game/types.h"

#include <algorithm>
#include <cstdint>

namespace reone {

namespace game {

constexpr bool qualifiesForWeaponFinesse(
    bool tsl,
    bool lightsaber,
    WeaponWield weaponWield,
    bool hasFinesseLightsabers,
    bool hasFinesseMeleeWeapons) {

    if (!tsl) {
        if (lightsaber) {
            return true;
        }
        switch (weaponWield) {
        case WeaponWield::StunBaton:
        case WeaponWield::SingleSword:
        case WeaponWield::DoubleBladedSword:
            return hasFinesseMeleeWeapons;
        default:
            return false;
        }
    }
    if (lightsaber && hasFinesseLightsabers) {
        return true;
    }

    // TSL falls through to the ordinary wield-category test when a
    // lightsaber user does not have Finesse: Lightsabers.
    switch (weaponWield) {
    case WeaponWield::StunBaton:
    case WeaponWield::SingleSword:
    case WeaponWield::DoubleBladedSword:
        return hasFinesseMeleeWeapons;
    default:
        return false;
    }
}

constexpr int getCloseProximityRangedAttackBonus(
    bool tsl,
    bool withinFiveUnits,
    bool hasCloseCombat,
    bool hasImprovedCloseCombat) {

    if (!withinFiveUnits) {
        return 0;
    }
    if (!tsl) {
        return 10;
    }
    return static_cast<int>(hasCloseCombat) +
           static_cast<int>(hasImprovedCloseCombat);
}

constexpr int getMeleeOnRangedAttackBonus(
    bool tsl,
    bool targetHasRangedRightHandWeapon,
    bool targetHasCloseCombat,
    bool targetHasImprovedCloseCombat) {

    if (!targetHasRangedRightHandWeapon) {
        return 0;
    }
    if (!tsl) {
        return 10;
    }
    return 6 -
           2 * (static_cast<int>(targetHasCloseCombat) +
                static_cast<int>(targetHasImprovedCloseCombat));
}

constexpr bool grantsJuyoExtraOnHandAttack(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm) {

    return tsl &&
           rightHandLightsaber &&
           currentForm == CombatForm::SaberVIIJuyo;
}

constexpr int getOrdinaryUnarmedDamageDie(
    bool tsl,
    CreatureSize creatureSize) {

    if (tsl) {
        return 4;
    }
    return creatureSize <= CreatureSize::Small ? 2 : 1;
}

constexpr bool isAutoBalanceEligible(
    bool tsl,
    bool partyMember,
    std::uint8_t multiplierSet) {

    return tsl && !partyMember && multiplierSet != 0;
}

constexpr int getAutoBalanceLevelBonus(
    std::uint8_t playerLevelAtSpawn,
    float multiplier) {

    int level = static_cast<int8_t>(playerLevelAtSpawn);
    float factor = level == 0 ? 1.0f : static_cast<float>(level);
    return static_cast<int>(factor * multiplier);
}

constexpr int getAutoBalanceDamageFactor(
    std::uint8_t playerLevelAtSpawn,
    float multiplier) {

    int level = static_cast<int8_t>(playerLevelAtSpawn);
    float factor = level == 0 ? 0.0f : static_cast<float>(level - 1);
    return 1 + static_cast<int>(factor * multiplier);
}

/** Base and derived save stats are interpreted as signed bytes.
 * Effect/room/Survival modifiers are added AFTER the derived-byte boundary.
 */
constexpr int getBaseSavingThrowBonus(int classes, int conditioning, int autoBalance) {
    return static_cast<int8_t>(classes + conditioning + autoBalance);
}

constexpr int getDerivedSavingThrowBonus(int base, int ability, int utcBonus) {
    return static_cast<int8_t>(base + ability + utcBonus);
}

inline int getSurvivalSavingThrowBonus(bool tsl, bool survival, int vitality, int maximum) {
    if (!tsl || !survival) {
        return 0;
    }
    // Both virtual HP returns are sign-extended from AX before division.
    vitality = static_cast<int16_t>(vitality);
    maximum = static_cast<int16_t>(maximum);
    if (maximum == 0) {
        // Zero-denominator infinities/NaN match no threshold interval.
        return 0;
    }
    // Divide first in single precision; do not quantize HP to an
    // integer percentage, which moves the exact 10/20/30/40/50 boundaries.
    float percent = static_cast<float>(vitality) / static_cast<float>(maximum) * 100.0f;
    if (percent < 0.0f || percent >= 50.0f) {
        return 0;
    }
    if (percent >= 40.0f) return 1;
    if (percent >= 30.0f) return 2;
    if (percent >= 20.0f) return 3;
    if (percent >= 10.0f) return 4;
    return 5;
}

constexpr int getRoomSavingThrowModifier(bool tsl, int goodEvil, int forceRating) {
    if (!tsl) return 0;
    return goodEvil >= 60 ? forceRating : (goodEvil <= 40 ? -forceRating : 0);
}

constexpr int getSavingThrowEffectTotal(bool tsl, int increases, int decreases, int survival) {
    const int cap = tsl ? 60 : 20;
    const int positive = increases + (tsl ? survival : 0);
    return std::min(cap, positive) - std::min(cap, decreases);
}

constexpr int getSavingThrowStat(int base, int effects) {
    return static_cast<int8_t>(base + effects);
}

constexpr int getSavingThrowModifier(bool tsl, int effects, int room, int survival) {
    int modifier = effects + (tsl ? room + survival : 0);
    int cap = tsl ? 60 : 20;
    return modifier > cap ? cap : modifier;
}

/** Highest rank wins even for non-cumulative/malformed feat sets. */
constexpr int getCombatDamageFeatBonus(bool tsl, bool ranged, int combatRank, int meleeRank) {
    if (!tsl) return 0;
    int rank = !ranged && meleeRank > combatRank ? meleeRank : combatRank;
    return rank * 2;
}

constexpr int getPreciseShotDamageBonus(bool tsl, bool ranged, int highestRank) {
    if (!tsl || !ranged || highestRank <= 0) return 0;
    return highestRank == 1 ? 1 : 2 * (highestRank - 1);
}

constexpr int getLightsaberFormDamageBonus(bool tsl, bool rightHandLightsaber, CombatForm form) {
    return tsl && rightHandLightsaber && form == CombatForm::SaberIIMakashi ? 3 : 0;
}

struct UnarmedFeatDamage {
    int rank209 {0};
    int rank212 {0};
    int total() const { return rank209 + rank212; }
};

// The damage-bonus call samples these dice once before the critical
// loop. Keeping the sampling stage distinct prevents accidental per-critical
// rerolls or applying the base-dice-only autobalance factor to these dice.
template <class RollDie>
UnarmedFeatDamage rollUnarmedFeatDamage(int dice209, int dice212, RollDie &&rollDie) {
    UnarmedFeatDamage result;
    for (int i = 0; i < dice209; ++i) result.rank209 += rollDie(4);
    for (int i = 0; i < dice212; ++i) result.rank212 += rollDie(4);
    return result;
}

constexpr bool qualifiesForDualStrike(bool tsl, bool partyMember, bool rank1, bool rank2, bool rank3) {
    // Only rank 1 requires party membership; higher ranks use the party-list
    // search for any attacker.
    return tsl && ((partyMember && rank1) || rank2 || rank3);
}

constexpr int getDualStrikeAttackBonus(bool qualifies, bool matchingAlly, bool rank1, bool rank2, bool rank3) {
    return qualifies && matchingAlly ? 2 * (static_cast<int>(rank1) +
        static_cast<int>(rank2) + static_cast<int>(rank3)) : 0;
}

struct ResistanceFeatReduction {
    int percentage {0};
    int improvedToughness {0};
    int endurance {0};

    int total() const { return percentage + improvedToughness + endurance; }
};

constexpr int getEnduranceDamageReduction(int remaining, int minimum, int percent) {
    if (remaining <= minimum) {
        return minimum;
    }
    return minimum + (remaining - minimum) * percent / 100;
}

inline ResistanceFeatReduction getResistanceFeatReduction(
    bool tsl, int damage, int percentageRank, bool improvedToughness,
    bool endurance95, bool endurance224, bool endurance225) {
    ResistanceFeatReduction result;
    if (!tsl) {
        result.improvedToughness = improvedToughness ? 2 : 0;
        result.endurance = endurance95 ? 2 : 0;
        return result;
    }
    // Consume resistance pools using the original incoming damage.
    // Apply these feat reductions before subtracting the selected resistance.
    float percent = percentageRank >= 3 ? 0.15f : percentageRank == 2 ? 0.10f :
                    percentageRank == 1 ? 0.05f : 0.0f;
    result.percentage = static_cast<int>(static_cast<float>(damage) * percent);
    int remaining = damage - result.percentage;
    if (improvedToughness) {
        int tenPercent = static_cast<int>(static_cast<float>(remaining) * 0.10f);
        result.improvedToughness = tenPercent < 2 ? 2 : tenPercent;
        remaining -= result.improvedToughness;
    }
    // Feat IDs 225, 224 and 95 select one endurance rank; they are not additive.
    if (endurance225) {
        result.endurance = getEnduranceDamageReduction(remaining, 8, 10);
    } else if (endurance224) {
        result.endurance = getEnduranceDamageReduction(remaining, 5, 5);
    } else if (endurance95) {
        result.endurance = 2;
    }
    return result;
}

constexpr int getAttackEffectModifierCap(bool tsl) {
    return tsl ? 60 : 20;
}

constexpr int getTargetingAttackBonus(
    bool tsl,
    bool rangedWeapon,
    int highestRank) {

    if (!tsl || !rangedWeapon || highestRank <= 0) {
        return 0;
    }
    return highestRank > 10 ? 10 : highestRank;
}

constexpr int getSuperiorWeaponFocusLightsaberBonus(
    bool tsl,
    bool lightsaber,
    int highestRank) {

    if (!tsl || !lightsaber || highestRank <= 0) {
        return 0;
    }
    return highestRank > 3 ? 3 : highestRank;
}

constexpr int getSuperiorTwoWeaponPenaltyReduction(
    bool tsl,
    bool offHand,
    int highestRank) {

    if (!tsl || highestRank <= 0) {
        return 0;
    }
    if (offHand) {
        return highestRank >= 3 ? 1 : 0;
    }
    return highestRank >= 2 ? 2 : 1;
}

constexpr bool qualifiesForDueling(
    bool tsl,
    bool rightHandEquipped,
    WeaponWield rightHandWield,
    bool leftHandEquipped) {

    if (leftHandEquipped) {
        return false;
    }
    if (!rightHandEquipped) {
        return tsl;
    }
    return rightHandWield == WeaponWield::SingleSword ||
           rightHandWield == WeaponWield::BlasterPistol;
}

constexpr int getEffectiveArmorMaxDexterityBonus(
    bool tsl,
    int baseMaximum,
    int itemAdjustment) {

    if (!tsl || baseMaximum < 0) {
        return baseMaximum;
    }
    return baseMaximum + itemAdjustment;
}

constexpr int kMaximumDodgeBonus = 10;

constexpr int getTotalDefenseBonus(
    bool tsl,
    bool totalDefense,
    int highestQualifyingClassLevel) {

    if (!tsl || !totalDefense) {
        return 0;
    }
    return highestQualifyingClassLevel > 3 ? 6 : 4;
}

constexpr int getLightsaberFormAttackBonus(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm,
    bool targetWieldsLightsaber) {

    if (!tsl || !rightHandLightsaber) {
        return 0;
    }

    switch (currentForm) {
    case CombatForm::SaberIShiiCho:
    case CombatForm::SaberVINiman:
        return 1;
    case CombatForm::SaberIIMakashi:
        return targetWieldsLightsaber ? 3 : 0;
    case CombatForm::SaberVShien:
        return 2;
    default:
        return 0;
    }
}

constexpr int getLightsaberFormDefenseBonus(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm,
    bool attackerIsCombatTarget) {

    if (!tsl || !rightHandLightsaber) {
        return 0;
    }

    switch (currentForm) {
    case CombatForm::SaberIShiiCho:
        return attackerIsCombatTarget ? 0 : 3;
    case CombatForm::SaberIIMakashi:
        return 0;
    case CombatForm::SaberIIISoresu:
        return attackerIsCombatTarget ? 2 : 0;
    case CombatForm::SaberIVAtaru:
        return attackerIsCombatTarget ? 3 : -2;
    case CombatForm::SaberVShien:
        return attackerIsCombatTarget ? -5 : 0;
    case CombatForm::SaberVINiman:
        return 1;
    case CombatForm::SaberVIIJuyo:
        return attackerIsCombatTarget ? -2 : -4;
    default:
        return 0;
    }
}

constexpr int getCriticalThreatThreshold(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm,
    int baseThreshold) {

    if (!tsl || !rightHandLightsaber) {
        return baseThreshold;
    }

    switch (currentForm) {
    case CombatForm::SaberIIISoresu:
        return baseThreshold < 20 ? baseThreshold + 1 : baseThreshold;
    case CombatForm::SaberIVAtaru:
        return baseThreshold > 1 ? baseThreshold - 1 : baseThreshold;
    default:
        return baseThreshold;
    }
}

constexpr int getCriticalConfirmationBonus(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm) {

    if (!tsl || !rightHandLightsaber) {
        return 0;
    }
    return currentForm == CombatForm::SaberVIIJuyo ? 4 : 0;
}

constexpr int getCriticalMultiplierBonus(
    bool tsl,
    bool rightHandLightsaber,
    CombatForm currentForm) {

    if (!tsl || !rightHandLightsaber) {
        return 0;
    }
    return currentForm == CombatForm::SaberVShien ? 1 : 0;
}

constexpr bool isMeleePowerAttackFeat(FeatType feat) {
    switch (feat) {
    case FeatType::PowerAttack:
    case FeatType::ImprovedPowerAttack:
    case FeatType::MasterPowerAttack:
        return true;
    default:
        return false;
    }
}

constexpr bool isMeleeFlurryFeat(FeatType feat) {
    switch (feat) {
    case FeatType::Flurry:
    case FeatType::ImprovedFlurry:
    case FeatType::MasterFlurry:
        return true;
    default:
        return false;
    }
}

constexpr bool isMeleeCriticalStrikeFeat(FeatType feat) {
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::ImprovedCriticalStrike:
    case FeatType::MasterCriticalStrike:
        return true;
    default:
        return false;
    }
}

constexpr int getMeleeSpecialAttackRollBonus(bool tsl, FeatType feat) {
    switch (feat) {
    case FeatType::PowerAttack:
    case FeatType::ImprovedPowerAttack:
    case FeatType::MasterPowerAttack:
        return -3;
    case FeatType::Flurry:
        return -4;
    case FeatType::ImprovedFlurry:
        return -2;
    case FeatType::MasterFlurry:
        return tsl ? 0 : -1;
    default:
        return 0;
    }
}

constexpr int getMeleeSpecialAttackDamageBonus(bool tsl, FeatType feat) {
    if (tsl) {
        switch (feat) {
        case FeatType::PowerAttack:
            return 3;
        case FeatType::ImprovedPowerAttack:
            return 7;
        case FeatType::MasterPowerAttack:
            return 12;
        default:
            return 0;
        }
    }

    switch (feat) {
    case FeatType::PowerAttack:
        return 5;
    case FeatType::ImprovedPowerAttack:
        return 8;
    case FeatType::MasterPowerAttack:
        return 10;
    default:
        return 0;
    }
}

constexpr int getMeleeSpecialAttackDefensePenalty(bool tsl, FeatType feat) {
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::ImprovedCriticalStrike:
    case FeatType::MasterCriticalStrike:
        return 5;
    case FeatType::Flurry:
        return tsl ? 2 : 4;
    case FeatType::ImprovedFlurry:
        return tsl ? 1 : 2;
    case FeatType::MasterFlurry:
        return tsl ? 0 : 1;
    default:
        return 0;
    }
}

constexpr int getPowerAttackCriticalMultiplierBonus(
    bool tsl,
    FeatType feat) {

    return tsl && isMeleePowerAttackFeat(feat) ? 1 : 0;
}

constexpr bool shouldAttemptPowerAttackKnockdown(
    bool tsl,
    FeatType feat,
    AttackResultType result) {

    return tsl &&
           isMeleePowerAttackFeat(feat) &&
           result == AttackResultType::CriticalHit;
}

constexpr bool shouldAttemptCriticalStrikeStun(
    FeatType feat,
    AttackResultType result) {

    return isMeleeCriticalStrikeFeat(feat) &&
           (result == AttackResultType::HitSuccessful ||
            result == AttackResultType::CriticalHit ||
            result == AttackResultType::AutomaticHit);
}

constexpr int getMeleeSpecialAttackSaveDC(
    bool tsl,
    FeatType feat,
    int attackerLevel,
    int strengthModifier) {

    if (!tsl) {
        return isMeleeCriticalStrikeFeat(feat) ? attackerLevel + strengthModifier : 0;
    }

    // stores the level in a byte, reads Strength as a signed byte,
    // then passes the completed DC as an unsigned 16-bit value.
    int levelByte = static_cast<std::uint8_t>(attackerLevel);
    int strengthByte = static_cast<std::uint8_t>(strengthModifier);
    int signedStrength = strengthByte < 0x80
                             ? strengthByte
                             : strengthByte - 0x100;

    int difficultyClass = 0;
    if (isMeleePowerAttackFeat(feat)) {
        difficultyClass = levelByte + 2 * signedStrength;
    } else if (isMeleeCriticalStrikeFeat(feat)) {
        difficultyClass = levelByte + signedStrength;
    } else {
        return 0;
    }
    return static_cast<std::uint16_t>(difficultyClass);
}

constexpr bool acBonusVsDamageTypeApplies(
    std::uint16_t rawSubtype,
    int damageFlags) {

    // Both games compare the raw iprp_combatdam row with runtime flags.
    switch (rawSubtype) {
    case 1:
        return damageFlags == 0;
    case 2:
        return damageFlags == static_cast<int>(DamageType::Bludgeoning);
    case 4:
        return damageFlags == static_cast<int>(DamageType::Piercing);
    default:
        return false;
    }
}

} // namespace game

} // namespace reone
