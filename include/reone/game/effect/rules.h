/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <algorithm>
#include <cstddef>

namespace reone::game {

// Effect type identifiers, distinct from visualeffects.2da row IDs and
// EffectType enum values.
constexpr int kEffectIconType = 67;
constexpr int kVisualEffectType = 30;

constexpr size_t getAbilityEffectSourceCapacity(bool tsl) {
    return tsl ? 108u : 36u;
}
constexpr int getAbilityEffectIncreaseCap(bool tsl) { return tsl ? 60 : 20; }
constexpr int getAbilityEffectDecreaseCap(bool tsl) { return tsl ? 90 : 30; }

// Fresh application only. Save restoration requires its own load-mode contract.
constexpr bool admitsAbilityEffect(
    bool dead, bool temporarilyDead, int amount, bool decrease, bool plot) {
    return !dead && !temporarilyDead && amount > 0 && !(decrease && plot);
}

// The VM stores the argument unchanged. The apply handler converts
// values <= 99 to a rate percentage and retains that mutated payload.
constexpr int normalizeMovementSpeedIncrease(int percent) {
    return percent <= 99 ? percent + 100 : percent;
}
constexpr float getMovementSpeedMultiplier(bool increase, int payload) {
    return increase ? payload / 100.0f : 1.0f - payload / 100.0f;
}
constexpr float clampMovementRate(float rate) {
    return std::clamp(rate, 0.125f, 1.5f);
}

inline float getMovementRateFactor(float stored, bool tsl, bool applyMobility, bool ownsMobility) {
    return clampMovementRate(stored + (tsl && applyMobility && ownsMobility ? 0.1f : 0.0f));
}
} // namespace reone::game
