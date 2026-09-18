/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <algorithm>
#include <cstdint>
#include "effect.h"
namespace reone::game {
/** Derived creature bytes. EffectInstance retains each unmodified source amount. */
struct ForceResistanceState {
    int8_t base {0};
    int8_t penalty {0};

    int value() const {
        return std::max(0, static_cast<int>(static_cast<int8_t>(base - penalty)));
    }
    void applyIncrease(int amount) {
        const int candidate = std::min(amount, 128);
        if (candidate > value()) base = static_cast<int8_t>(candidate);
    }
    void applyDecrease(int amount) {
        const int candidate = std::min(amount, 128);
        if (penalty <= candidate) penalty = static_cast<int8_t>(candidate);
    }
    template<class Records>
    void removeIncrease(const Records &records, const EffectInstance &removed) {
        int highest = 0;
        int lastPenalty = 0;
        for (const auto &record : records) {
            if (record.id == removed.id) continue;
            if (record.serializedType > 34) break;
            if (record.serializedType == 33) highest = std::max(highest, record.integerParameter(0));
            else if (record.serializedType == 34) lastPenalty = record.integerParameter(0);
        }
        // Removing an increase subtracts the last decrease while leaving the penalty
        // byte unchanged. This is not the inverse of admission.
        base = static_cast<int8_t>(static_cast<uint8_t>(highest) - static_cast<uint8_t>(lastPenalty));
    }
    template<class Records>
    void removeDecrease(const Records &records, const EffectInstance &removed) {
        int highest = 0;
        for (const auto &record : records) {
            if (record.serializedType > 34) break;
            if (record.applicationOrder == removed.applicationOrder) continue;
            // Select surviving increase records when removing this effect.
            if (record.serializedType == 33) highest = std::max(highest, record.integerParameter(0));
        }
        penalty = static_cast<int8_t>(highest);
    }
};
template<class Roll>
int resolveForceResistance(int userType, int resistance, int casterLevel, Roll &&roll) {
    if (static_cast<uint8_t>(userType) == 2) return 0;
    if (static_cast<uint8_t>(userType) != 1) return -1;
    if (resistance <= 0) return 0;
    return resistance > casterLevel + roll() ? 1 : 0;
}
} // namespace reone::game
