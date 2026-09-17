/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>

namespace reone::game {

// All six attribute getters combine the unsigned base byte with signed
// effect and racial bonuses. Apply the minimum before narrowing the result
// to an unsigned byte.
constexpr int getAbilityScoreFromParts(int base, int bonus, int racial) {
    const auto signedByte = [](int value) constexpr {
        const auto byte = static_cast<std::uint8_t>(value);
        return byte < 128u ? static_cast<int>(byte) : static_cast<int>(byte) - 256;
    };
    const int total = static_cast<std::uint8_t>(base) +
                      signedByte(bonus) + signedByte(racial);
    return total <= 3 ? 3 : static_cast<std::uint8_t>(total);
}

} // namespace reone::game
