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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "types.h"

namespace reone::game {

inline bool isForceUsingClass(ClassType type, bool tsl) {
    return (type >= ClassType::JediGuardian && type <= ClassType::JediSentinel) ||
           (tsl && type >= ClassType::JediWeaponMaster && type <= ClassType::SithAssassin);
}

inline int boundedResource(int64_t value) {
    return static_cast<int>(std::clamp<int64_t>(value, 0, std::numeric_limits<int16_t>::max()));
}

inline int narrowSignedResource(int64_t value) {
    // Both originals store the low 16 bits. Interpret the sign explicitly,
    // without an implementation-defined unsigned-to-signed narrowing cast.
    const int bits = static_cast<uint16_t>(value);
    return bits <= 0x7fff ? bits : bits - 0x10000;
}

inline int forcePointMaximum(int base, int level, int wisdomModifier,
                             int charismaModifier, bool tsl, int64_t bonuses) {
    level = static_cast<uint8_t>(level);
    const auto signedByte = [](int value) {
        const int bits = static_cast<uint8_t>(value);
        return bits < 0x80 ? bits : bits - 0x100;
    };
    const int modifier = signedByte(wisdomModifier) + (tsl ? 0 : signedByte(charismaModifier));
    return std::max(level, narrowSignedResource(base + level * modifier + bonuses));
}

inline int healedForcePointPool(int base, int temporary, int amount, int maximum) {
    // Healing observes the total current pool, but writes the ordinary pool.
    // The temporary grant remains independently removable.
    return boundedResource(std::min<int64_t>(maximum,
        static_cast<int64_t>(base) + temporary + amount));
}

inline int damagedForcePointPool(int base, int temporary, int amount) {
    // Direct damage observes total FP, writes ordinary FP, and leaves the
    // temporary grant intact. Spell expenditure uses a different contract.
    return boundedResource(static_cast<int64_t>(base) + temporary - amount);
}

inline int consumeTemporaryResource(int &temporary, int amount) {
    amount = std::max(0, amount);
    if (temporary >= amount) { temporary -= amount; return 0; }
    const int remainder = amount - temporary;
    temporary = 0;
    return remainder;
}

inline int adjustedForcePointCost(int base, float alignmentMultiplier, bool tsl,
                                 int charismaModifier, int alignment, int roomRating,
                                 CombatForm form) {
    if (base <= 0) return 0;
    if (!std::isfinite(alignmentMultiplier))
        throw std::invalid_argument("Invalid spell alignment cost multiplier");
    if (tsl && alignmentMultiplier > 1.0f && charismaModifier > 0)
        alignmentMultiplier = std::max(1.0f, alignmentMultiplier - 0.05f * charismaModifier);
    const float scaled = static_cast<float>(base) * alignmentMultiplier;
    if (!std::isfinite(scaled) || scaled < static_cast<float>(std::numeric_limits<int>::min()))
        throw std::invalid_argument("Spell alignment cost is outside the supported range");
    int64_t cost = static_cast<int>(std::min(scaled, static_cast<float>(std::numeric_limits<int16_t>::max())));
    if (!tsl) return static_cast<int>(cost);
    const int roomAdjustment = roomRating / 20;
    if (alignment >= 60) cost -= roomAdjustment;
    else if (alignment < 50) cost += roomAdjustment;
    cost = std::max<int64_t>(1, cost);
    if (form == CombatForm::ForceIIPotency || form == CombatForm::ForceIVMastery)
        cost = static_cast<int>(cost * 1.2f);
    return boundedResource(cost);
}

struct ForcePointCharge {
    int requiredForce {0};
    int spentForce {0};
    int spentHitPoints {0};
    bool usesHitPoints {false};
};

inline ForcePointCharge forcePointCharge(int cost, int forceBodyLevel) {
    cost = std::max(0, cost);
    if (forceBodyLevel == -1) return {cost, cost, 0, false};
    const int percentage = forceBodyLevel >= 0 && forceBodyLevel <= 2
        ? 50 - 10 * forceBodyLevel : 0;
    const int share = static_cast<int>(static_cast<int64_t>(cost) * percentage / 100);
    // Readiness and the actual debit are distinct for improved Force Body.
    // Even a zero life share uses the Force Body HP-readiness branch.
    return {cost - share, share, share, true};
}

inline bool canPayForcePointCharge(const ForcePointCharge &charge, int force, int hp) {
    // The total is narrowed to a signed word, then compared with the
    // unsigned cost. Negative and overflowing pools must not be saturated.
    return static_cast<uint32_t>(narrowSignedResource(force)) >=
               static_cast<uint32_t>(charge.requiredForce) &&
           (!charge.usesHitPoints || narrowSignedResource(hp) > charge.spentHitPoints);
}

struct ForcePointPools {
    int force;
    int temporaryForce;
    int hitPoints;
    int temporaryHitPoints;
};

inline bool payForcePointCharge(ForcePointPools &pools, const ForcePointCharge &charge) {
    if (!canPayForcePointCharge(charge, pools.force + pools.temporaryForce,
                               narrowSignedResource(static_cast<int64_t>(pools.hitPoints) + pools.temporaryHitPoints))) return false;
    const int remainder = consumeTemporaryResource(pools.temporaryForce, charge.spentForce);
    if (remainder > 0) {
        pools.force = std::max(0, narrowSignedResource(static_cast<int64_t>(pools.force) - remainder));
    }
    // Temporary Force which covers the entire debit also supplies the life share.
    if (remainder > 0 && charge.spentHitPoints > 0) {
        const int hpRemainder = consumeTemporaryResource(pools.temporaryHitPoints, charge.spentHitPoints);
        pools.hitPoints = boundedResource(static_cast<int64_t>(pools.hitPoints) - hpRemainder);
    }
    return true;
}

} // namespace reone::game
