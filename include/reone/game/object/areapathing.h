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
#include <vector>

#include <boost/noncopyable.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "reone/game/pathfinder.h"

namespace reone {

namespace game {

class Area;
class Creature;

/**
 * Keeps an area's pathfinder in step with the area. Its walkmesh is put
 * together from the rooms' walkmeshes, cut by the placeables as they stand,
 * once the area is loaded, and again as placeables come or go. The filter it
 * gives keeps planned paths off the walls the walkmesh does not show: those
 * of doors, which open and close.
 */
class AreaPathing : boost::noncopyable {
public:
    AreaPathing(Area &area, Pathfinder &pathfinder);

    /** Take a room's walkmesh, in the layout's order, to put the walkmesh together from. */
    void addRoomWalkmesh(std::shared_ptr<graphics::Walkmesh> walkmesh) {
        _roomWalkmeshes.push_back(std::move(walkmesh));
    }

    /**
     * Put the pathfinder's walkmesh together again from the rooms', cut by the
     * placeables as they stand, at once. A pathfinder not put together from
     * rooms' walkmeshes is left as it is.
     */
    void rebuild();

    /** Put the walkmesh together again if the placeables with walkmeshes changed. */
    void update();

    /**
     * A placeable's walkmesh may have come or gone: the walkmesh is put
     * together again if so.
     */
    void markObstaclesStale();

    /**
     * The faces of the pathfinder's walkmesh a walker's planned path may pass
     * over: those whose centre has no wall of a door, as it stands, within
     * the walker's personal space. Placeables are cut out of the walkmesh
     * instead.
     */
    FacePassable facesClearOfStaticWalls(const Creature &walker) const;

private:
    /** A placeable's blocking faces, three corners each in its walkmesh's own coordinates, and where the walkmesh stands. */
    struct ObstacleSource {
        glm::mat4 transform {1.0f};
        std::vector<glm::vec3> corners;
    };

    Area &_area;
    Pathfinder &_pathfinder;

    std::vector<std::shared_ptr<graphics::Walkmesh>> _roomWalkmeshes;
    /** A placeable may have come or gone since they were last looked at. */
    bool _stale {false};
    /** The placeables, by id, the walkmesh in place was cut by. */
    std::vector<uint32_t> _cutIds;

    /** The placeables' blocking faces as they stand. */
    std::vector<ObstacleSource> obstacleSources() const;
    /**
     * The placeables' outlines on the walkable ground, from the blocking faces
     * of each placeable's walkmesh: a flat face's triangle across the ground
     * plan, and a thin strip along an upright face that does not stand at
     * the side of a flat one. Where floors lie over others, each cuts the
     * floors a walker standing on would meet its face from.
     */
    static std::vector<Obstacle> obstacles(const std::vector<ObstacleSource> &sources, bool stacked);
};

} // namespace game

} // namespace reone
