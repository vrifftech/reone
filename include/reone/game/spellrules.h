/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
namespace reone::game {
inline int calculateSpellLevel(int classLevels, bool autoBalance, uint8_t spawnLevel,
                               uint8_t globalPlayerLevel, float multiplier) {
    if (autoBalance) {
        int level = static_cast<int8_t>(spawnLevel == 0 ? globalPlayerLevel : spawnLevel);
        if (level <= 0) level = 1;
        classLevels += static_cast<int>(static_cast<float>(level) * multiplier) - 1;
        if (classLevels < 0) classLevels = 1;
    }
    return static_cast<uint8_t>(classLevels);
}
inline int lightsaberThrowSpell(int mode) { return mode != 0 ? 4 : 49; }
inline int lightsaberThrowDice(int level) { return static_cast<uint8_t>(level) / 2; }
inline int calculateSpellSaveDC(bool tsl, int spell, int level,
                               int wisdom, int charisma,
                               bool sense, bool advanced, bool mastery) {
    int base = tsl && spell >= 159 && spell <= 161 ? 10 : 5;
    int focus = mastery ? 4 : advanced ? (tsl ? 3 : 2) : sense ? (tsl ? 2 : 1) : 0;
    return static_cast<uint8_t>(level) + static_cast<int8_t>(wisdom) +
           static_cast<int8_t>(charisma) + base + focus;
}
} // namespace reone::game
