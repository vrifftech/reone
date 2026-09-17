/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <optional>

namespace reone::game {

// Shared combat-message TLK identities; resolve through each title's resources.
constexpr int kEffectOutcomePrefixStrRef = 42157;

constexpr std::optional<int> getEffectOutcomeMessageStrRef(bool, int outcome) {
    if (outcome == -1 || outcome == 1) {
        return std::nullopt;
    }
    return outcome == 2 ? 42160 : 42158;
}

constexpr std::optional<int> getEffectOutcomeLabelStrRef(int effectType) {
    // Type zero leaves CUSTOM1 untouched in the handler.
    if (effectType == 0) {
        return std::nullopt;
    }
    constexpr int labels[] {0, 42031, 42035, 42036, 42032, 42037,
                            42038, 42033, 42040, 42034, 42039};
    // The switch fetches StrRef 0 for an unknown nonzero type.
    return effectType > 0 && effectType <= 10 ? labels[effectType] : 0;
}

} // namespace reone::game
