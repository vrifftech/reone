/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include <optional>

#include "../combattables.h"
#include "../effect.h"

namespace reone {

namespace game {

struct ForceShieldDefinition {
    int visual;
    int damageFlags;
    int vulnerabilities;
    int resistance;
    int amount;
};
// The shield's own visual gives way to that of the first of its appearances
// the creature has.
inline ForceShieldDefinition forceShieldDefinition(const ForceShieldRow &row, int appearance) {
    int visual = row.visual;
    for (size_t index = 0; index < row.appearances.size(); ++index) {
        if (row.appearances[index] && *row.appearances[index] == appearance) {
            visual = row.appearanceVisuals[index];
            break;
        }
    }
    return {visual, row.damageFlags, row.vulnerabilities, row.resistance, row.amount};
}
template<class Records>
inline std::optional<EffectId> replacedForceShield(const Records &records) {
    for (const auto &record : records) {
        if (record.serializedType > 107) break;
        if (record.serializedType != 107) continue;
        // Return the first Force Shield root's argument, including zero.
        return record.integerParameter(0) != 0 ? std::optional<EffectId>(record.id) : std::nullopt;
    }
    return std::nullopt;
}

class ForceShieldEffect : public CopyableEffect<ForceShieldEffect> {
public:
    ForceShieldEffect(int shield) :
        CopyableEffect(EffectType::ForceShield) { setSaveFacingInteger(0, shield); }

    EffectApplicationResult onApply(Object &object, EffectInstance &) override;

};

} // namespace game

} // namespace reone
