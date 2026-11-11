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

#include "staticwalls.h"

#include "reone/game/object/door.h"
#include "reone/game/surfaces.h"
#include "reone/scene/node/walkmesh.h"

namespace reone {

namespace game {

// Blocking faces are the non-walkable ones that walking is checked against,
// in the rooms and in doors and placeables alike.
std::set<uint32_t> blockingSurfaces(const ISurfaces &surfaces) {
    const auto walkable = surfaces.getWalkableSurfaces();
    std::set<uint32_t> blocking;
    for (uint32_t material : surfaces.getWalkcheckSurfaces()) {
        if (walkable.count(material) == 0) {
            blocking.insert(material);
        }
    }
    return blocking;
}

// Whether an enabled walkmesh has a blocking face within radius of the level
// segment between two points, between two heights.
bool hasBlockingFaceWithin(const std::shared_ptr<scene::WalkmeshSceneNode> &node, const std::set<uint32_t> &blocking,
                           const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ) {
    if (!node || !node->isEnabled()) {
        return false;
    }
    const glm::vec3 localFrom {node->absoluteTransformInverse() * glm::vec4(from, 1.0f)};
    const glm::vec3 localTo {node->absoluteTransformInverse() * glm::vec4(to, 1.0f)};
    const float shift = localFrom.z - from.z;
    return node->walkmesh().hasFaceWithin(blocking, localFrom, localTo, radius, minZ + shift, maxZ + shift);
}

// Where a walker of the given radius, swept along the level segment between
// two points, first meets a blocking face of an enabled walkmesh between two
// heights; the contact's normal in world coordinates.
std::optional<graphics::Walkmesh::Contact> sweepWalkmesh(const std::shared_ptr<scene::WalkmeshSceneNode> &node, const std::set<uint32_t> &blocking,
                                                         const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ) {
    if (!node || !node->isEnabled()) {
        return std::nullopt;
    }
    const glm::vec3 localFrom {node->absoluteTransformInverse() * glm::vec4(from, 1.0f)};
    const glm::vec3 localTo {node->absoluteTransformInverse() * glm::vec4(to, 1.0f)};
    const float shift = localFrom.z - from.z;
    auto contact = node->walkmesh().sweep(blocking, localFrom, localTo, radius, minZ + shift, maxZ + shift);
    if (contact) {
        contact->normal = glm::vec3(node->absoluteTransform() * glm::vec4(contact->normal, 0.0f));
    }
    return contact;
}

// Whether a wall of a door as it stands lies within radius of the level
// segment between two points, between two heights.
bool hasDoorWallWithin(const Door &door, const std::set<uint32_t> &blocking,
                       const glm::vec3 &from, const glm::vec3 &to, float radius, float minZ, float maxZ) {
    for (const auto &node : {door.walkmeshOpen1(), door.walkmeshOpen2(), door.walkmeshClosed()}) {
        if (hasBlockingFaceWithin(node, blocking, from, to, radius, minZ, maxZ)) {
            return true;
        }
    }
    return false;
}

} // namespace game

} // namespace reone
