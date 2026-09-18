/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <optional>
#include "types.h"

namespace reone::game {
// These subtypes qualify for immunity after a failed numerical save.
inline std::optional<ImmunityType> savingThrowImmunity(SavingThrowType type, int spellUserType) {
    switch (type) {
    case SavingThrowType::MindAffecting: return ImmunityType::MindSpells;
    case SavingThrowType::Poison: return ImmunityType::Poison;
    case SavingThrowType::Disease: return ImmunityType::Disease;
    case SavingThrowType::Fear: return ImmunityType::Fear;
    case SavingThrowType::Trap: return ImmunityType::Trap;
    case SavingThrowType::Death:
        if (static_cast<uint8_t>(spellUserType) == 1 ||
            static_cast<uint8_t>(spellUserType) == 2) return ImmunityType::Death;
        break;
    default: break;
    }
    return std::nullopt;
}

template <typename CheckImmunity>
SavingThrowResult resolveSavingThrow(int total, int dc, CheckImmunity &&checkImmunity) {
    if (total >= static_cast<uint16_t>(dc)) return SavingThrowResult::Saved;
    return checkImmunity() ? SavingThrowResult::Immune : SavingThrowResult::Failed;
}
} // namespace reone::game
