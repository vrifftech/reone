/*
 * Copyright (c) 2025 The reone project contributors
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

#include <variant>

#include "reone/game/effect/damage.h"

namespace reone {
namespace game {

/** Stores the displayed values for one ability-drain result. */
struct AbilityDrainFeedback {
    /** The affected ability. */
    Ability ability {Ability::Strength};
    /** The ability-score decrease. */
    int amount {0};
    /** The effect duration in seconds. */
    int durationSeconds {0};
};

/** Stores the displayed values for one saving throw. */
struct SavingThrowFeedback {
    /** The saving throw category. */
    SavingThrow savingThrow {SavingThrow::None};
    /** The base saving throw value. */
    int base {0};
    /** The total saving throw modifier. */
    int modifier {0};
    /** The d20 roll. */
    int roll {0};
    /** The difficulty class. */
    int difficultyClass {0};
};

/** Represents feedback that an attack emits when its impact resolves. */
using DeferredCombatFeedback = std::variant<
    AbilityDrainFeedback,
    SavingThrowFeedback>;

} // namespace game
} // namespace reone
