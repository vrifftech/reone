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
#include <string>
#include <unordered_map>

#include "reone/graphics/texture.h"

#include "../types.h"

namespace reone {

namespace game {

struct Feat {
    std::string name;
    std::string description;
    std::shared_ptr<graphics::Texture> icon;
    int category {0}; // talent category
    int usesPerDay {0}; // zero for a feat whose uses are not counted
    int maxCR {0}; // the challenge rating that ranks the feat as a talent
    uint32_t exclusion {0}; // talent inclusion mask
    uint32_t minCharLevel {0};
    FeatType preReqFeat1 {FeatType::Invalid};
    FeatType preReqFeat2 {FeatType::Invalid};
    FeatType successor {FeatType::Invalid};
    uint32_t pips {1}; // 1-3, position in a feat chain
    std::optional<int> spellId; // the spell whose effects count as this feat's effects
    // TSL companion columns by lower-case companion tag: the character level
    // at which the companion gains the feat, or 255 for a class feat the
    // companion is never granted. Only nonzero values are kept.
    std::unordered_map<std::string, int> companionLevels;
};

} // namespace game

} // namespace reone
