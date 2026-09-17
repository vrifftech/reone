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
#include <array>
#include <cstdint>
#include <cstddef>

namespace reone::game {

// AC effect categories and selectors are distinct from damage-packet flags.
struct ArmorClassEffectData {
    int category {0}; // dodge, natural, armour, shield, deflection
    int amount {0};
    int race {7};
    int lawChaos {0};
    int goodEvil {0};
    int selector {0x4007};
    bool decrease {false};

    bool validCategory() const { return category >= 0 && category < 5; }
    bool unqualified() const { return race == 7 && lawChaos == 0 && goodEvil == 0; }
    bool cached(bool tsl) const {
        if (!validCategory() || !unqualified()) return false;
        return !tsl || decrease || category != 0 || selector == 0x2007 || selector == 0x4007;
    }
    bool conditional(int attackerRace, int attackerAlignment, int damageFlags) const {
        // The skip test ignores lawChaos. PHYSICAL is an exact selector, not a bitmask
        // intersection; neither 0x2007 nor DAMAGE_TYPE_ALL is a wildcard.
        if (race == 7 && goodEvil == 0 && selector == 0x4007) return false;
        if ((race != 7 && race != attackerRace) ||
            (goodEvil != 0 && goodEvil != attackerAlignment)) return false;
        return selector == 0x4007 || (selector == 1 && damageFlags == 0) ||
               (selector == 2 && damageFlags == 1) || (selector == 4 && damageFlags == 2);
    }
};

constexpr int armorClassByte(std::int64_t value) {
    auto byte = static_cast<std::uint8_t>(value);
    return byte < 128 ? byte : static_cast<int>(byte) - 256;
}
constexpr int armorClassShort(std::int64_t value) {
    auto word = static_cast<std::uint16_t>(value);
    return word < 32768 ? word : static_cast<int>(word) - 65536;
}

// The cursor changes only when an AC increase exists and retains its previous
// offset when the last increase disappears. Queries and removals both start
// here, including decrease-only scans.
struct ArmorClassCursor {
    std::uint16_t position {0};

    template<class ReadType>
    void update(std::size_t count, ReadType type) {
        for (std::size_t i = 0; i < count; ++i) {
            if (type(i) == 48) {
                position = static_cast<std::uint16_t>(i);
                return;
            }
        }
    }
};

template<class ReadType, class ReadEffect, class IsRemoved>
int remainingArmorClassMaximum(const ArmorClassCursor &cursor, std::size_t count,
                             const ArmorClassEffectData &removed, ReadType type,
                             ReadEffect effect, IsRemoved isRemoved) {
    int maximum = 0;
    for (std::size_t i = cursor.position; i < count; ++i) {
        // In particular, decrease removal does not skip an increase prefix.
        if (type(i) != (removed.decrease ? 49 : 48)) break;
        if (isRemoved(i)) continue;
        const auto candidate = effect(i);
        // Decrease removal checks race only; increase removal also checks the
        // two alignment fields. Neither recomputation checks the selector.
        if (candidate.category == removed.category && candidate.race == 7 &&
            (removed.decrease || candidate.unqualified()))
            maximum = std::max(maximum, candidate.amount);
    }
    return maximum;
}

struct ArmorClassCache {
    // Cache values use signed-byte arithmetic. Preserve update history: computing
    // a fresh maximum at query time would lose overflow and removal effects.
    std::array<int, 5> increases {};
    std::array<int, 5> decreases {};

    void add(const ArmorClassEffectData &effect, bool tsl) {
        if (!effect.cached(tsl)) return;
        auto &value = (effect.decrease ? decreases : increases)[effect.category];
        if (effect.category == 0) value = armorClassByte(std::int64_t(value) + effect.amount);
        else if (effect.amount > value) value = armorClassByte(effect.amount);
    }
    void remove(const ArmorClassEffectData &effect, bool tsl, int remainingMaximum) {
        if (!effect.cached(tsl)) return;
        auto &value = (effect.decrease ? decreases : increases)[effect.category];
        if (effect.category == 0) value = armorClassByte(std::int64_t(value) - effect.amount);
        else value = armorClassByte(remainingMaximum);
    }
    int dodge() const {
        int difference = increases[0] - decreases[0];
        return difference > 10 ? 10 : armorClassByte(difference);
    }
};

struct ConditionalArmorClass {
    std::int64_t dodge {0};
    int deflection {0};
    int shieldDecrease {0};

    explicit ConditionalArmorClass(const ArmorClassCache &cache) : deflection(cache.increases[4]) {}
    void add(const ArmorClassEffectData &effect, int race, int alignment, int damageFlags) {
        if (!effect.conditional(race, alignment, damageFlags)) return;
        if (effect.category == 0) dodge += effect.decrease ? -std::int64_t(effect.amount) : effect.amount;
        if (!effect.decrease && effect.category == 4)
            deflection = std::max(deflection, effect.amount);
        if (effect.decrease && effect.category == 3)
            shieldDecrease = std::max(shieldDecrease, effect.amount);
        // The versus AC result does not use conditional natural, armour, or shield
        // increases. Shield decreases add to the residual sum; deflection decreases
        // do not contribute to it.
    }
};

struct ArmorClassParts {
    int natural {0};
    int armour {0};
    int shield {0};
    std::int64_t dodgeAndDeflection {0};
};

inline ArmorClassParts armorClassParts(const ArmorClassCache &cache,
                                  const ConditionalArmorClass &conditional,
                                  int natural, int armour, int shield,
                                  bool versus, bool attackerSeen = true) {
    ArmorClassParts result;
    result.natural = armorClassByte(std::int64_t(natural) + cache.increases[1] - cache.decreases[1]);
    result.armour = armorClassByte(std::int64_t(armour) + cache.increases[2] - cache.decreases[2]);
    result.shield = armorClassByte(std::int64_t(shield) + cache.increases[3] - cache.decreases[3]);
    if (versus) {
        result.dodgeAndDeflection = (attackerSeen ? cache.dodge() : 0) + conditional.dodge +
                                   conditional.deflection + std::int64_t(conditional.shieldDecrease);
    } else {
        // The general AC query includes both deflection increases and decreases.
        result.dodgeAndDeflection = cache.dodge() + armorClassByte(cache.increases[4] - cache.decreases[4]);
    }
    return result;
}

} // namespace reone::game
