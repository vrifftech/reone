/*
 * Copyright (c) 2026 The reone project contributors
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

#include <cstdint>

#include "reone/game/types.h"
#include "reone/game/runtimeref.h"
#include "reone/game/combatfeedback.h"
#include <optional>
#include <vector>
#include "reone/script/types.h"

namespace reone {

namespace game {

class Object;
class Game;
class ServicesView;

/** Identifies an item-on-hit effect subtype. */
enum class ItemOnHitSubtype : uint16_t {
    Sleep = 0,
    Stun = 1,
    Paralyze = 2,
    Confusion = 3,
    Fear = 4,
    Slow = 5,
    AbilityDrain = 6,
    ItemPoison = 7,
    SlayRG = 8,
    SlayAG = 9,
    InstantDeath = 10,
    Knockdown = 11,
};

/** Stores one active item property before attack resolution. */
struct ItemOnHitProperty {
    /** The item-on-hit effect subtype. */
    ItemOnHitSubtype subtype {ItemOnHitSubtype::Sleep};
    /** The activation chance as a percentage. */
    int chance {0};
    /** The effect duration in seconds. */
    float duration {0.0f};
    /** The saving throw difficulty class. */
    int difficultyClass {20};
    /** The required saving throw category. */
    SavingThrow savingThrow {SavingThrow::None};
    /** The saving throw subtype. */
    SavingThrowType savingThrowType {SavingThrowType::All};
    /** A subtype-specific value. */
    int parameter {0};
    /** True for a state-duration handler, independently of table selection. */
    bool durationBranch {false};
};

/**
 * Chance and duration carry across the entire on-hit property iteration.
 * Slay properties inherit the previous chance, initially zero. Table selection
 * and handler selection are separate steps.
 */
struct ItemOnHitSelectionState {
    int chance {0};
    int rounds {0};

    void select(ItemOnHitSubtype subtype) {
        if (subtype == ItemOnHitSubtype::AbilityDrain ||
            subtype == ItemOnHitSubtype::ItemPoison ||
            subtype == ItemOnHitSubtype::InstantDeath ||
            subtype == ItemOnHitSubtype::Knockdown) {
            chance = 100;
        }
    }
};

constexpr bool usesItemOnHitDurationHandler(ItemOnHitSubtype subtype) {
    return subtype >= ItemOnHitSubtype::Sleep && subtype <= ItemOnHitSubtype::Slow;
}

// The property is a byte and the race is an unsigned word. No wildcard.
constexpr bool matchesSlayRacialGroup(int race, int parameter) {
    return static_cast<uint16_t>(race) == static_cast<uint8_t>(parameter);
}

/** Stores effect-outcome values for combat feedback. */
struct EffectOutcomeBreakdown {
    /** True if the record contains an effect outcome; false otherwise. */
    bool present {false};
    /** The saving throw category used by the effect. */
    int saveType {0};
    /** The effect subtype displayed in feedback. */
    int effectType {0};
    /** The effect-specific saving throw mode. */
    int saveMode {0};
    /** The d20 roll. */
    int saveRoll {0};
    /** The total saving throw modifier. */
    int modifierTotal {0};
    /** The base saving throw value. */
    int baseSave {0};
    /** The final saving throw value. */
    int finalTotal {0};
    /** The difficulty class. */
    int difficultyClass {0};
    /** The encoded effect outcome, or -1 if no outcome is available. */
    int outcome {-1};
};

/** Stores one admitted item-on-hit effect until its impact resolves. */
struct ItemOnHitApplication {
    /** The item-on-hit effect subtype. */
    ItemOnHitSubtype subtype {ItemOnHitSubtype::Sleep};
    /** The effect duration in seconds. */
    float duration {0.0f};
    /** A subtype-specific value. */
    int parameter {0};
    /** Bound attacker retained across resolution and impact. */
    RuntimeObjectRef<Object> creator;
    /** The retained feedback values for the effect outcome. */
    EffectOutcomeBreakdown effectOutcome;
    /** True if the impact emits effect-outcome feedback; false otherwise. */
    bool emitEffectOutcome {false};
};

std::optional<int> getItemOnHitEffectOutcomeType(ItemOnHitSubtype subtype);
void applyItemOnHitApplications(std::vector<ItemOnHitApplication> applications,
                               Object &target, Game &game, ServicesView &services);
void addDeferredCombatFeedback(Game &game, ServicesView &services,
                               const std::shared_ptr<Object> &attacker, const Object &target,
                               const std::vector<DeferredCombatFeedback> &records);

} // namespace game

} // namespace reone
