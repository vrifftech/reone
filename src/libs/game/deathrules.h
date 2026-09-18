/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <optional>
#include "reone/game/effect.h"
#include "reone/resource/2da.h"

namespace reone::game {

// OnApplyResurrection sign-extends the virtual HP returns from AX.
constexpr int getResurrectionHitPoints(bool tsl, int current, int maximum, int percentage) {
    current = static_cast<int16_t>(current);
    if (current > 0) return current;
    if (!tsl || percentage <= 0) return 1;
    maximum = static_cast<int16_t>(maximum);
    const int product = maximum * percentage;
    // Use the quotient outside [-99, 99], including for negative numerators.
    const int result = product <= -100 || product >= 100 ? product / 100 : 1;
    return result; // SetCurrentHitPoints receives the full quotient, not a narrowed word.
}

constexpr bool shouldCheckDeathImmunity(uint32_t spellId, uint16_t linkClass) {
    return spellId != UINT32_MAX && (static_cast<unsigned>(linkClass) & 0x18u) == 0x08u;
}

// Beam classification uses the low 16 bits of the visual ID.
constexpr bool isBeamVisual(bool tsl, int visualId) {
    const auto id = static_cast<std::uint16_t>(visualId);
    switch (id) {
    case 2026: case 2027: case 2028: case 2029:
    case 2037: case 2038:
    case 2049: case 2050: case 2051: case 2052: case 2053:
    case 2061: case 2065: case 2066: case 4037: case 6000:
        return true;
    case 2068: case 2069:
        return tsl;
    default:
        return false;
    }
}

inline bool isEffectPreservedOnDeath(const EffectInstance &record,
    const resource::TwoDA &table, bool tsl) {
    if (record.durationType() == DurationType::Innate ||
        record.durationType() == DurationType::Equipped) return true;
    const auto type = record.type();
    const int scriptType = static_cast<int>(type) < 0x100 ? static_cast<int>(type) : 0;
    for (int row = 0; row < table.getRowCount(); ++row) {
        const auto exempt = table.getIntOpt(row, "effecttype");
        if (!exempt) continue;
        if (*exempt == scriptType) return true;
        if (*exempt == static_cast<int>(EffectType::Beam) && record.serializedType == 30 &&
            isBeamVisual(tsl, record.integerParameter(0))) return true;
    }
    return false;
}

// Advance the live-array index even after removing the current entry.
// Do not restart the scan or consume the shifted entry at the same index.
template <class Collection, class RemovePackage>
void removeResurrectionEffects(Collection &effects, RemovePackage removePackage) {
    for (std::size_t index = 0; index < effects.size(); ++index) {
        const auto &record = effects[index];
        if (record.serializedType > 57) break;
        if (record.serializedType == 57 && record.durationType() == DurationType::Temporary)
            removePackage(record.id);
    }
}

} // namespace reone::game
