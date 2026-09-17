/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "integerarithmetic.h"
#include "reone/resource/2da.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace reone::game {
struct DeathExperience {
    int row {-1};
    int column {-1};
    float amount {0.0f};
    int awarded {0}; // ceilf: party award and world floaty
    int reported {0}; // truncate: death feedback packet
};

inline std::vector<std::uint32_t> readExperienceThresholds(
    const resource::TwoDA &table, bool tsl) {
    const int size = tsl ? 51 : 21;
    if (table.getRowCount() < size)
        throw ValidationException("Missing required exptable thresholds");
    std::vector<std::uint32_t> values;
    values.reserve(size);
    for (int row = 0; row < size; ++row) {
        const auto value = table.getStringOpt(row, "xp");
        if (!value || value->empty())
            throw ValidationException("Missing required exptable XP at row " + std::to_string(row));
        // INT storage is compared unsigned. The stock last row is
        // 0xFFFFFFFF, which cannot be read through TwoDA::getIntOpt's stoi.
        const bool hex = value->size() > 2 && (*value)[0] == '0' &&
                         ((*value)[1] == 'x' || (*value)[1] == 'X');
        std::size_t end = 0;
        const auto number = std::stoll(*value, &end, hex ? 16 : 10);
        if (end != value->size() || number < -2147483648LL || number > 4294967295LL)
            throw ValidationException("Invalid XP threshold at row " + std::to_string(row));
        values.push_back(static_cast<std::uint32_t>(number));
    }
    return values;
}

inline int getDeathExperienceRow(
    const std::vector<std::uint32_t> &thresholds, std::uint32_t experience, bool tsl) {
    const int top = tsl ? 50 : 20;
    for (int row = top; row >= 0; --row) {
        if (experience >= thresholds[row]) return tsl ? std::min(row, 30) : row;
    }
    return -1;
}

inline int getDeathExperienceColumn(bool tsl, float challengeRating,
    bool autoBalance, std::uint8_t spawnLevel, int challengeModifier) {
    int challenge = truncateToInteger32(challengeRating);
    if (tsl && autoBalance) {
        int level = spawnLevel < 128 ? int(spawnLevel) : int(spawnLevel) - 256;
        if (!level) level = 1;
        challenge = std::max(0, level + challengeModifier);
    }
    if (tsl && challenge > 30) return 31;
    return challenge + 1;
}

inline DeathExperience resolveDeathExperience(
    const resource::TwoDA &experienceTable, const resource::TwoDA &npcTable,
    int row, int column, bool tsl, int companionCount) {
    DeathExperience result;result.row = row;result.column = column;
    float base = 0.0f;
    if (row >= 0 && row < experienceTable.getRowCount() &&
        column >= 0 && column < experienceTable.getColumnCount()) {
        base = experienceTable.getFloatOpt(row, experienceTable.columns()[column]).value_or(0.0f);
    }
    // These individual FLOAT/INT output slots initialize to zero;
    // only this consumer permits missing/blank cells. Tables remain required.
    const float percent = npcTable.getFloatOpt(tsl ? 13 : 9, "percentxp").value_or(0.0f);
    result.amount = (percent / 100.0f) * base;
    const int bonus = npcTable.getIntOpt(tsl ? 14 : 10, "percentxp").value_or(0);
    if (bonus > 0) {
        const float perCompanion = static_cast<float>(bonus) / 100.0f;
        const float factor = static_cast<float>(companionCount) * perCompanion + 1.0f;
        result.amount *= factor;
    }
    result.awarded = truncateToInteger32(std::ceil(result.amount));
    result.reported = truncateToInteger32(result.amount);
    return result;
}

struct ExperienceFloaty {
    static constexpr int labelStrRef = 38551;
    static constexpr float duration = 3.0f;
    // Purple feedback color.
    static constexpr std::array<float, 3> color {0.95f, 0.0f, 0.85f};
    static std::string text(const std::string &label, int amount) {
        return label + " " + std::to_string(amount); // "%s %d"
    }
};
} // namespace reone::game
