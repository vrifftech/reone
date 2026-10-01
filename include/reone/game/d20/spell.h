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

#include <array>
#include <optional>

#include "reone/graphics/texture.h"

#include "../types.h"

namespace reone {

namespace audio {
class AudioClip;
}

namespace graphics {
class Model;
}

namespace game {

// The spells.2da CastAnim code, which picks both the conjure and the cast clip.
enum class SpellCastAnimation {
    Other = 0,
    Self = 1,
    Dark = 2,
    Up = 3,
    Area = 4,
    Touch = 6,
    Throw = 7,
    Jump = 8,
    Monster = 9,
    Fury = 10,
    Crush = 11,
    MonsterFury = 12
};

struct Spell {
    SpellType type;
    std::string name;
    std::string description;
    std::shared_ptr<graphics::Texture> icon;
    uint32_t pips {1};
    std::vector<SpellType> prerequisites;
    std::optional<SpellType> masterSpell;
    int userType {-1};
    uint8_t innateLevel {0xff};
    int forcePointCost {0};
    char alignment {'N'};
    std::unordered_map<ClassType, int> classLevelRequirements;
    uint32_t category {0};
    int maxCR {0}; // the challenge rating that ranks the spell as a talent
    std::string impactScript;
    SpellCastAnimation castAnimation {SpellCastAnimation::Other};
    std::shared_ptr<audio::AudioClip> castSound;
    float conjTime {0.0f};
    float castTime {0.0f};
    float catchTime {0.0f};
    uint32_t itemTargeting {0};
    uint32_t requireItemMask {0};
    uint32_t forbidItemMask {0};
    float range {0.0f};
    std::string rangeTag;
    std::optional<float> minimumRange;
    uint32_t formMask {0};
    bool hostile {false};
    int hostileSlot {-1};
    int friendlySlot {-1};
    int menuPriority {0};
    std::shared_ptr<graphics::Model> projModel;
    bool projectile {false};
    ProjectilePathType projectilePath {ProjectilePathType::Default};
    std::string projectileSpawn;
    std::string projectileOrientation;
    // The head, hand and ground models shown while conjuring and after the release.
    std::array<std::string, 3> conjureVisuals;
    std::array<std::string, 3> castVisuals;
    // Target kinds a hostile power menu does not offer the power against. Talent
    // selection also tests it against the requested inclusion.
    uint32_t exclusion {0};

    bool isForbiddenByEquipment(uint32_t equippedMask) const {
        return (equippedMask & forbidItemMask) != 0;
    }

    bool hasRequiredEquipment(uint32_t equippedMask) const {
        return (equippedMask & requireItemMask) == requireItemMask;
    }

    std::optional<int> getClassLevelRequirement(ClassType clazz) const {
        auto maybeRequirement = classLevelRequirements.find(clazz);
        return maybeRequirement != classLevelRequirements.end()
                   ? std::optional<int>(maybeRequirement->second)
                   : std::nullopt;
    }
};

} // namespace game

} // namespace reone
