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
#include <unordered_map>
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
 * once the area is loaded, and again, away from the area, as placeables come,
 * go or move. The filters it gives keep planned paths off the walls the
 * walkmesh does not show: those of doors, which open and close, and of
 * placeables not yet cut out as they stand.
 */
class AreaPathing : boost::noncopyable {
public:
    AreaPathing(Area &area, Pathfinder &pathfinder);
    ~AreaPathing();

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

    /**
     * Follow the placeables that came, went or moved. The walkmesh is put
     * together again by a worker of the thread pool and put in place a
     * quarter of a second of game time later, whatever the frame rate; one at
     * a time, changes made meanwhile going into the next. Until then, planned
     * paths are kept off a placeable not yet cut out as it stands, and paths
     * whose way ahead it crosses are planned again.
     */
    void update(float dt);

    /**
     * A placeable's walkmesh may have come, gone or moved: the walkmesh is put
     * together again if so.
     */
    void markObstaclesStale();

    /**
     * The faces of the pathfinder's walkmesh a walker's planned path may pass
     * over: those whose centre has no wall within the walker's personal space
     * of a door, or of a placeable not yet cut out of the walkmesh, as they
     * stand.
     */
    FacePassable facesClearOfStaticWalls(const Creature &walker) const;

    /**
     * Where a walker's planned leg first meets a wall, within its personal
     * space, of a door, or of a placeable not yet cut out of the walkmesh, as
     * they stand: the walls facesClearOfStaticWalls tests at the faces'
     * centres, met along the whole leg. A wall the walker stands within reach
     * of at the leg's start is not counted.
     */
    LegBlocker legsClearOfStaticWalls(const Creature &walker) const;

private:
    /** A placeable with a walkmesh: its id, where its walkmesh stands, and the box round it across the ground plan. */
    struct Pose {
        uint32_t id {0};
        glm::mat4 transform {1.0f};
        glm::vec2 min {0.0f};
        glm::vec2 max {0.0f};

        /** The same placeable, standing exactly where it stood. */
        bool operator==(const Pose &other) const { return id == other.id && transform == other.transform; }
        bool operator!=(const Pose &other) const { return !(*this == other); }
    };

    /** A placeable's blocking faces, three corners each in its walkmesh's own coordinates, and where the walkmesh stands. */
    struct ObstacleSource {
        glm::mat4 transform {1.0f};
        std::vector<glm::vec3> corners;
    };

    struct Build;

    Area &_area;
    Pathfinder &_pathfinder;

    std::vector<std::shared_ptr<graphics::Walkmesh>> _roomWalkmeshes;
    /** A placeable may have come, gone or moved since they were last looked at. */
    bool _stale {false};
    /** The placeables as they stood when last looked at, by id. */
    std::vector<Pose> _seen;
    /** The placeables as they stood when the walkmesh in place was cut, by id. */
    std::vector<Pose> _cut;
    /**
     * The placeables whose way across paths was marked since the walkmesh in
     * place was cut, by id, and where each stood when it was.
     */
    std::unordered_map<uint32_t, glm::mat4> _marked;
    /** The placeables not cut out of the walkmesh in place as they stand, taken when asked for. */
    mutable std::vector<Pose> _uncut;
    mutable bool _uncutStale {true};
    /** The walkmesh being put together away from the area. */
    std::shared_ptr<Build> _build;

    /** The placeables with walkmeshes as they stand, by id. */
    std::vector<Pose> poses() const;
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
    /** The placeables not cut out of the walkmesh in place as they stand. */
    const std::vector<Pose> &uncutPlaceables() const;
    /** Whether a placeable as it stands is among \p poses, sorted by id. */
    static bool hasPose(const std::vector<Pose> &poses, const Pose &pose);
    /** Have the paths whose way ahead crosses a placeable's box planned again. */
    void replanAcross(const Pose &pose);
    /** Put a walkmesh in place, cut by the placeables as they stood in \p cut. */
    void putInPlace(Uniwalk uni, std::vector<Pose> cut);
};

} // namespace game

} // namespace reone
