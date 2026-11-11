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
#include <cfloat>
#include <functional>
#include <optional>

#include "reone/graphics/aabb.h"
#include "reone/graphics/walkmesh.h"

namespace reone {

namespace game {

/// Walkable face that is used for pathfinding.
struct Uniface {
    /// Indices into Uniwalk::vertices array.
    uint32_t vertices[3];

    /// Indices of adjecent faces. Edges [0, 1], [1, 2], [2, 0] are
    // adjecent when they are common with any other face.
    uint32_t adjecent[3];

    /// Center of a face. Used to estimate distance between two faces.
    glm::vec3 centroid;
};

/// Subdivision of a walkmesh. Uniroom associates a range of faces [begin, end)
/// to AABB.
struct Uniroom {
    uint32_t begin;
    uint32_t end;
    glm::vec3 min;
    glm::vec3 max;
    /** The room walkmesh the faces came from. */
    const graphics::Walkmesh *walkmesh {nullptr};
    /**
     * For each face of the room walkmesh, by its index there, the first face
     * of the unified walkmesh it became and the one past its last: more than
     * one once obstacles cut it. UINT32_MAX for a face that is not walkable.
     */
    std::vector<uint32_t> unifiedFaces;
    std::vector<uint32_t> unifiedFaceEnds;
};

/**
 * A convex outline across the ground plan, counter-clockwise seen from above,
 * that walks keep off: a blocking face of a placeable, or a thin strip along
 * one that stands upright. It cuts the faces of the walkable ground lying
 * between two heights of floor.
 */
struct Obstacle {
    std::vector<glm::vec2> outline;
    float floorMin {-FLT_MAX};
    float floorMax {FLT_MAX};
};

/**
 * The edges of the walkable ground, filed by the cells of a grid across the
 * ground plan that each passes over, for the pushes away from them.
 */
struct EdgeGrid {
    glm::vec2 origin {0.0f};
    float cellSize {1.0f};
    int columns {0};
    int rows {0};
    /** For each cell, row by row, where its edges start in edges; one more entry than cells. */
    std::vector<uint32_t> cellStarts;
    /** Edges, each as its face's index times three plus the edge's. */
    std::vector<uint32_t> edges;
};

/// Unified walkmesh, assembled from walkmeshes of all rooms in the area.
struct Uniwalk {
    std::vector<glm::vec3> vertices;
    std::vector<Uniface> faces;
    std::vector<Uniroom> rooms;
    /** Region of each face: faces joined through shared edges share one. */
    std::vector<uint32_t> regions;
    /**
     * Whether each vertex lies on the edge of the walkable ground: on an edge
     * no other walkable face shares, or one between a face and one inside an
     * obstacle.
     */
    std::vector<uint8_t> boundary;
    /**
     * Whether each face lies inside an obstacle: a path keeps off it while
     * another way is found. Empty when no obstacle cut the walkmesh.
     */
    std::vector<uint8_t> blocked;
    /**
     * The edges of the walkable ground, filed across the ground plan: those
     * no other walkable face shares, and those between a face and one inside
     * an obstacle.
     */
    EdgeGrid edgeGrid;
};

/// State of a face for A* algorithm.
struct AStarFace {
    enum Flag {
        Open = 1,
        Closed = 2,
    };

    Flag flag;
    uint32_t parent;
    /** The length of the best way found to the face, from centroid to centroid. */
    float travelled;
};

/// Element of a list of faces to consider next for A* algorithm.
struct AStarOpenFace {
    uint32_t index;
    /** The way travelled to the face, and the straight line from it to the goal. */
    float cost;
    /** When the face was put on the list in its search: of equal costs, the earliest is taken first. */
    uint32_t order;
};

struct AStarContext {
    std::vector<AStarFace> state;
    std::vector<AStarOpenFace> open;
    /** Per face, whether the walker may pass over it: -1 not asked yet, 0 no, 1 yes. */
    std::vector<int8_t> passes;
};

/**
 * Whether a walker may pass over a face of the unified walkmesh (an index
 * into Uniwalk::faces).
 */
using FacePassable = std::function<bool(uint32_t)>;

/// Fully calculated path.
struct AStarPath {
    std::vector<uint32_t> faces;
    glm::vec3 from;
    glm::vec3 to;
    /** The face of the path the walker was last found on. */
    uint32_t current {0};
    /** The walker's radius: the path keeps that far from the corners it turns at. */
    float radius {0.0f};
    /**
     * The path is one straight leg from its start to its end, walked as it is
     * and never planned again; it has no faces.
     */
    bool straight {false};
    /**
     * The points of the walk, walked straight one after another, and the index
     * of the one being walked to. While the path follows its faces they are its
     * start, the corners the funnel turns at and its end, planned with the
     * path. A straight path's are its start and end. Once a way round a
     * creature has been spliced into the path, which nothing does while the
     * detours along a hexagon are switched off, they are never planned again.
     */
    std::vector<glm::vec3> points;
    uint32_t pointNext {1};
    /**
     * While the path follows its faces, the gap each point lies in, by the
     * index in faces of the face before it; the end's is the last face.
     */
    std::vector<uint32_t> pointFaces;
    /** The path takes each next point from its faces, rather than walking its points. */
    bool followsFaces {true};
    /** The faces the walker may pass over, kept for planning the path again. */
    FacePassable passable;
    /** The unified walkmesh the path was planned over (see Pathfinder::generation). */
    uint32_t generation {0};
    /**
     * The path is planned again as it is next followed, as when a placeable
     * came or moved across it (see markPathsAcross).
     */
    bool replan {false};

    int32_t index;
    bool active;
};

/// Handle to a calculated path.
struct Path {
    int32_t index;
};

/** A face of a room walkmesh, by its index there. */
struct RoomFaceRef {
    const graphics::Walkmesh *walkmesh {nullptr};
    uint32_t face {0};
};

/**
 * The face of a room walkmesh a point stands on, as the area finds it; empty
 * when the point stands on none.
 */
using FaceLocator = std::function<std::optional<RoomFaceRef>(const glm::vec3 &)>;

struct Pathfinder : public boost::noncopyable {
    Uniwalk uni;
    AStarContext astar;
    std::vector<AStarPath> paths;
    /**
     * The face a point stands on, as the area finds it. Without one, the
     * pathfinder finds it over the unified walkmesh by the same rule for its
     * walkable faces (see graphics::Walkmesh::standingFace).
     */
    FaceLocator locate;
    /**
     * Counts the unified walkmeshes put in place: a path planned over an
     * earlier one is planned again as it is next followed.
     */
    uint32_t generation {0};
};

void uniwalkLoadRoom(struct Uniwalk &wm, const graphics::Walkmesh &data, const std::set<uint32_t> &walkableMaterial);
/**
 * Join the rooms' faces into one walkable ground. Each obstacle cuts the faces
 * under it into pieces, those inside it marked blocked; pieces are joined to
 * their neighbours as whole faces are. It touches nothing but \p uni, so it
 * may run away from the main thread.
 */
void uniwalkFinalize(struct Uniwalk &uni, const std::vector<Obstacle> &obstacles = {});
/**
 * Put a unified walkmesh in place of the pathfinder's own, and draw it for
 * debugging. A path planned over the old one keeps its faces when every face
 * of a room's walkmesh it crosses was cut into the same pieces in both, and
 * blocked or not alike; any other is planned again as it is next followed.
 */
void replaceUniwalk(Pathfinder &pf, Uniwalk uni);

/**
 * A path from one point to another for a walker of \p radius: over the faces
 * \p passable lets it cross, off those inside obstacles and through the gaps
 * wide enough for it, when there is such a path, and keeping that far from the
 * corners it turns at. The faces it starts and ends on always count as
 * passable. When no such path is found, one over every face and gap but those
 * inside obstacles is looked for, then one over every face and gap.
 */
std::optional<Path> createPath(Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to, const FacePassable &passable = {},
                               float radius = 0.0f);
/**
 * A path along the straight line from one point to another, when the walkmesh
 * joins them.
 */
std::optional<Path> createStraightPath(Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to);
/**
 * Plan a path that follows its faces again from \p from, as it was planned,
 * except that the faces \p keepOff turns down are never crossed, even when no
 * other way is found. False, the path left as it was, when no way is found. A
 * straight path, or one walking explicit points, is left as it is.
 */
bool replanPath(Pathfinder &pf, Path path, const glm::vec3 &from, const FacePassable &keepOff = {});
/** Whether the walkmesh joins two points, without holding a path. */
bool pathExists(const Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to);
bool updatePath(Pathfinder &pf, Path p, const glm::vec3 &current);
/**
 * Have each path that follows its faces, and whose way ahead passes through a
 * box across the ground plan widened by its walker's radius, planned again as
 * it is next followed.
 */
void markPathsAcross(Pathfinder &pf, const glm::vec2 &min, const glm::vec2 &max);
void releasePath(Pathfinder &pf, Path path);

glm::vec3 getNextPathPoint(Pathfinder &pf, Path path);
glm::vec3 getLastPathPoint(Pathfinder &pf, Path path);
/** Whether the path is one straight leg (see createStraightPath). */
bool isStraightPath(const Pathfinder &pf, Path path);
/**
 * The points the path is walked through, from its start to its end, and in
 * \p next the index of the one being walked to.
 */
const std::vector<glm::vec3> &getPathPoints(const Pathfinder &pf, Path path, uint32_t &next);

// Explicit points and the way round a creature along a hexagon, spliced into
// a path. A walk steers round creatures and plans again past them instead;
// these are kept, but only reached while Creature's detours along a hexagon
// are switched on.

/**
 * The points of a path, from the one last left behind to the destination, and
 * in \p next the index of the one being walked to.
 */
std::vector<glm::vec3> pathPoints(Pathfinder &pf, Path path, uint32_t &next);
/** Whether the path walks explicit points rather than following its faces. */
bool followsPathPoints(Pathfinder &pf, Path path);
/** Walk explicit points from now on, heading for the one at \p next. */
void setPathPoints(Pathfinder &pf, Path path, std::vector<glm::vec3> points, uint32_t next);

// Planning a way round a creature that blocks a walk. The creature is ringed
// by a hexagon; the walk leaves its path where the path enters the hexagon,
// follows the hexagon's edges and rejoins the path where it leaves.

/// Space kept beyond both creatures' personal spaces.
constexpr float kAvoidanceSpace = 0.2f;

/// Corners of the hexagon, clockwise seen from above. Edge e joins corner e to
/// corner e + 1.
struct AvoidanceHex {
    std::array<glm::vec3, 6> v;
};

/// Where a path crosses the hexagon: the edge, the index of the path point
/// ending the crossing segment, and the crossing point. A way in taken from a
/// corner has the corner for its edge and the point last left for its index.
struct AvoidanceCrossing {
    int edge {-1};
    int index {-1};
    glm::vec3 point {0.0f};
};

/**
 * The hexagon round \p center whose edges lie \p inradius from it. Its first
 * corner lies back along \p moverDir, at the height of the centre; the others
 * stand on the ground as \p height gives it.
 */
AvoidanceHex computeAvoidanceHex(const glm::vec3 &center, const glm::vec3 &moverDir, float inradius,
                                 const std::function<float(const glm::vec2 &)> &height);
/** Whether a point lies inside the hexagon or on an edge, seen from above. */
bool isPointInAvoidanceHex(const AvoidanceHex &hex, const glm::vec3 &point);
/** The corner nearest a point; a tie goes to the lower index. */
int closestAvoidanceHexCorner(const AvoidanceHex &hex, const glm::vec3 &point);
/**
 * Where the path, walked to its point \p next, enters and leaves the hexagon:
 * the last two crossings over the whole path, each segment crossing at most
 * once. A single crossing is left through; the way in is then the corner
 * nearest the point last left.
 */
bool findAvoidanceEntryAndExit(const AvoidanceHex &hex, const std::vector<glm::vec3> &points, uint32_t next,
                               AvoidanceCrossing &entry, AvoidanceCrossing &exit);
/**
 * The way along the hexagon from the entry to the exit: clockwise, keeping the
 * creature on the right, or counter-clockwise when \p left is set.
 */
std::vector<glm::vec3> findAvoidanceWay(const AvoidanceHex &hex, const AvoidanceCrossing &entry,
                                        const AvoidanceCrossing &exit, bool left);
/**
 * Put the way in place of the path points from the entry's up to the exit's,
 * that one kept unless both are the same, and bring the point being walked to
 * back to the way's start when it lay beyond.
 */
void insertAvoidanceWay(std::vector<glm::vec3> &points, uint32_t &next, int entryIndex, int exitIndex,
                        const std::vector<glm::vec3> &way);

/// Height of the walkable face under (x, y) that lies nearest to \p nearZ,
/// searched over every room of the unified walkmesh. Empty when no face lies
/// under (x, y).
std::optional<float> walkmeshElevationAt(const Uniwalk &uni, const glm::vec2 &xy, float nearZ);

// Steering near the edges of the walkable ground. Only edges within
// graphics::Walkmesh::kFloorHeadroom of the walker's height count, and only
// from the side of the walkable ground, so an edge beyond a thin wall or on
// another floor does not; everything is measured across the ground plan.

/**
 * The push away from the edges within \p radius and a margin of a walker at
 * \p position: for each, by how far the walker stands inside that reach, as a
 * share of it. It never pushes the walker on along \p heading, and is at most
 * one long.
 */
glm::vec2 computeKeepoutForce(const Uniwalk &uni, const glm::vec3 &position, float radius, const glm::vec2 &heading);
/**
 * A direction of travel slid along the edges within \p radius of a walker at
 * \p position: what heads into an edge, or along it, is turned a little away.
 */
glm::vec2 slideAlongEdges(const Uniwalk &uni, const glm::vec3 &position, float radius, glm::vec2 direction);
/**
 * Whether a point stands on the walkable ground, as a path's ends do, outside
 * the obstacles and with no edge within \p radius of it.
 */
bool isClearGround(const Pathfinder &pf, const glm::vec3 &point, float radius);

} // namespace game

} // namespace reone
