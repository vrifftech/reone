/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include "reone/resource/2da.h"
namespace reone::game {
template<class Roll>
int rollDamageShieldDice(const resource::TwoDA &table, int selector, Roll roll) {
    const auto count = static_cast<uint8_t>(table.getIntOpt(selector, "numdice").value_or(0));
    const auto sides = static_cast<uint8_t>(table.getIntOpt(selector, "die").value_or(0));
    int result = 0;
    for (int index = 0; index < count; ++index) result += roll(1, sides);
    return static_cast<uint16_t>(result);
}
inline float readDestroyObjectDelay(const resource::TwoDA &table, int row) {
    return table.getFloat(row, "destroyobjectdelay", 3.0f);
}
} // namespace reone::game
