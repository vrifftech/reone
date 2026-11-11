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
#include <memory>
#include <optional>
#include <set>

#include <glm/vec3.hpp>

#include "reone/graphics/walkmesh.h"

namespace reone {

namespace scene {

class WalkmeshSceneNode;

}

namespace game {

class Door;
class ISurfaces;

/**
 * The surfaces of the faces walking is checked against that are not walked
 * on: the walls of rooms, doors and placeables alike.
 */
std::set<uint32_t> blockingSurfaces(const ISurfaces &surfaces);

/**
 * Whether an enabled walkmesh has a blocking face within \p radius of the
 * level segment between two points, between two heights.
 */
bool hasBlockingFaceWithin(const std::shared_ptr<scene::WalkmeshSceneNode> &node, const std::set<uint32_t> &blocking,
                           const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ);

/**
 * Where a walker of \p radius, swept along the level segment between two
 * points, first meets a blocking face of an enabled walkmesh between two
 * heights; the contact's normal in world coordinates.
 */
std::optional<graphics::Walkmesh::Contact> sweepWalkmesh(const std::shared_ptr<scene::WalkmeshSceneNode> &node, const std::set<uint32_t> &blocking,
                                                         const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ);

/**
 * Whether a wall of a door as it stands lies within \p radius of the level
 * segment between two points, between two heights.
 */
bool hasDoorWallWithin(const Door &door, const std::set<uint32_t> &blocking,
                       const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ);

} // namespace game

} // namespace reone
