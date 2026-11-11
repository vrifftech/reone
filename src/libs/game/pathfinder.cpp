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

/**
   == Summary

   Pathfinding is implemented as a collection of algorithms:

   1. Global pathfinding algorithm builds a coarse-grained path from A to B as a
   sequence of walkmesh faces. It ensures that a path exists, but makes no
   effort to make it "natural" from the player perspective.

   2. Funnel algorithm takes a sequence of faces from the global pathfinding,
   and turns it into straight legs: the path's start, each corner the funnel
   turns at and its end. Corners on the edge of the walkable ground are drawn
   in by the walker's radius, and a leg ends where the path climbs or drops
   onto another floor.

   3. Steering behaviour takes a direction from the funnel algorithm, and turns
      it into a "driving" force, flat across the ground plan. It heads round a
      creature in the way instead, and sidesteps for a while when the walk
      gets nowhere. A "keepout" force steers away from the edges of the
      walkable ground. The walker turns toward the combined force at a
      limited rate, and slides along creatures and edges it presses on.

   == Data structures

   The primary input for these algorithms is a Uniwalk. It is a combined mesh of
   all room walkmeshes of an area. Walkmeshes are loaded into an area Uniwalk,
   and then combined using uniwalkFinalize. Uniwalk still tracks what faces
   belong to what room, and computes room AABBs to make spatial queries faster.

   Pathfinder struct keeps the Uniwalk, global pathfinding context, and a list
   of active paths. Global pathfinding is expensive, so the implementation puts
   an arbitrary limit on the maximum number of active paths. AStarPath objects
   are re-used to avoid frequent re-allocation.

   == Omissions

   1. Dynamic objects (creatures) are not handled by the global pathfinding
   as it plans. A creature met on the way is steered round; one walked into is
   planned past, its faces kept off. (A way along a hexagon round it, spliced
   into the path as explicit points, is kept but switched off.)

   2. Static objects that do not have a carve-out for them in the room walkmesh
      are not reflected in the Uniwalk yet. Each object has it is own
      "non-walkable" mesh, which must be subtracted from the room mesh.
 */

#include "reone/game/pathfinder.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <unordered_map>

#include <boost/functional/hash.hpp>

#include "reone/game/debug.h"
#include "reone/game/shaperules.h"
#include "reone/scene/drawdebug.h"

namespace reone {

namespace game {

static const uint32_t kTriangleEdges[3][2] = {{0, 1}, {1, 2}, {2, 0}};

// A floor more than this over a point is a floor over the one at its level,
// and a face more than this from a point's height is on another floor (see
// graphics::Walkmesh::kFloorHeadroom).
static constexpr float kFloorHeadroom = graphics::Walkmesh::kFloorHeadroom;

// The cells of the grid filing the edges of the walkable ground are this
// wide, unless the ground plan would need more than kMaxEdgeGridCells of
// them; they are widened until it does not.
static constexpr float kEdgeGridCellSize = 2.0f;
static constexpr int64_t kMaxEdgeGridCells = 1 << 16;
// The push away from an edge reaches this far beyond the walker's radius.
static constexpr float kKeepoutMargin = 0.25f;
// A direction slid along an edge heads away from it by at least this much.
static constexpr float kSlideOffEdge = 0.01f;

// Import a walkmesh of a single room into Uniwalk structure. Once all rooms are
// imported, call uniwalkFinalize to establish connections between rooms.
void uniwalkLoadRoom(struct Uniwalk &uni, const graphics::Walkmesh &data, const std::set<uint32_t> &walkableMaterial) {
    // Base of the vertex array for this room.
    uint32_t beginVertex = uni.vertices.size();

    // Copy all vertices without de-duplicating them with the previously loaded
    // meshes. Calculate AABB.
    glm::vec3 min = {FLT_MAX, FLT_MAX, FLT_MAX};
    glm::vec3 max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uni.vertices.reserve(uni.vertices.size() + data.vertices.size());
    for (glm::vec3 v : data.vertices) {
        uni.vertices.push_back(v);
        min = glm::min(min, v);
        max = glm::max(max, v);
    }

    // Expand AABB a bit to avoid rounding errors.
    glm::vec3 expand = {0.1f, 0.1f, 0.1f};
    min -= expand;
    max += expand;

    // Base of the face array for this room.
    uint32_t beginFace = uni.faces.size();

    // Copy all walkable faces, noting which unified face each became.
    uni.faces.reserve(uni.faces.size() + data.faces.size());
    std::vector<uint32_t> unifiedFaces(data.faces.size(), UINT32_MAX);
    std::vector<uint32_t> unifiedFaceEnds(data.faces.size(), UINT32_MAX);
    for (uint32_t i = 0; i < data.faces.size(); ++i) {
        uint32_t material = data.materials[i];
        if (!graphics::Walkmesh::hasSurfaceMaterial(walkableMaterial, material)) {
            continue;
        }
        unifiedFaces[i] = static_cast<uint32_t>(uni.faces.size());
        unifiedFaceEnds[i] = unifiedFaces[i] + 1;

        const graphics::Walkmesh::FaceVertices inputFace = data.faces[i];
        Uniface face = {0};
        face.centroid = {0.0f, 0.0f, 0.0f};
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t index = inputFace.indices[i] + beginVertex;
            face.vertices[i] = index;
            face.adjecent[i] = UINT32_MAX;
            face.centroid += uni.vertices[index];
        }
        face.centroid *= 0.333333f;
        uni.faces.push_back(face);
    }

    uint32_t endFace = uni.faces.size();
    Uniroom room = {
        beginFace,
        endFace,
        min, max};
    room.walkmesh = &data;
    room.unifiedFaces = std::move(unifiedFaces);
    room.unifiedFaceEnds = std::move(unifiedFaceEnds);

    uni.rooms.push_back(std::move(room));
}

static void drawDebugFace(const Uniwalk &uni, uint32_t index) {
    uint32_t colorAdj = 0x0000FFFF;
    uint32_t colorBorder = 0x9400D3FF;
    float thickness = 0.03f;

    const Uniface &face = uni.faces[index];
    bool isBorderFace = false;
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t v0 = face.vertices[kTriangleEdges[i][0]];
        uint32_t v1 = face.vertices[kTriangleEdges[i][1]];
        uint32_t color = colorAdj;
        if (face.adjecent[i] == UINT32_MAX) {
            isBorderFace = true;
            color = colorBorder;
        }
        drawdebug::line(uni.vertices[v0], uni.vertices[v1], color, thickness);
    }

    glm::vec3 textOffset(0.0f, 0.0f, 0.5f);
    uint32_t color = isBorderFace ? colorBorder : colorAdj;
    drawdebug::text(std::to_string(index), face.centroid + textOffset, color);
}

static void drawDebugUniwalk(const Uniwalk &uni) {
    drawdebug::pushId("uniwalk");
    drawdebug::clear();

    if (!isShowPathEnabled()) {
        drawdebug::popId();
        return;
    }

    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        // Draw contour and index of each face.
        drawDebugFace(uni, i);

        // Draw connections to adjecent faces.
        for (uint32_t j = 0; j < 3; ++j) {

            uint32_t adj = uni.faces[i].adjecent[j];
            if (adj == UINT32_MAX) {
                continue;
            }
            glm::vec3 centroidAdj = uni.faces[adj].centroid;
            drawdebug::line(uni.faces[i].centroid, centroidAdj, 0x00ff00ff, 0.02f);
        }
    }

    drawdebug::popId();
}

// Vertices closer than this are taken as one.
static constexpr float kVertexMergeDistance = 0.1f;

// A cell of a grid across space, and its hash.
struct GridCell {
    int x, y, z;

    bool operator==(const GridCell &other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridCellHash {
    size_t operator()(const GridCell &cell) const {
        size_t hash = 0;
        boost::hash_combine(hash, cell.x);
        boost::hash_combine(hash, cell.y);
        boost::hash_combine(hash, cell.z);
        return hash;
    }
};

static GridCell gridCellOf(const glm::vec3 &v, float cellSize) {
    return {static_cast<int>(std::floor(v.x / cellSize)), static_cast<int>(std::floor(v.y / cellSize)),
            static_cast<int>(std::floor(v.z / cellSize))};
}

// Which vertex each vertex is merged into; UINT32_MAX for one that is kept.
// Each vertex not yet merged takes every later vertex closer than
// kVertexMergeDistance, even one an earlier vertex took. Vertices are filed
// by the cells of a grid twice that wide, so only the 27 cells round a vertex
// hold any it takes.
static std::vector<uint32_t> mergeVertices(const std::vector<glm::vec3> &vertices) {
    const float cellSize = 2.0f * kVertexMergeDistance;
    // The vertices in each cell, in order.
    std::unordered_map<GridCell, std::vector<uint32_t>, GridCellHash> cells;
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        cells[gridCellOf(vertices[i], cellSize)].push_back(i);
    }

    const float eps2 = kVertexMergeDistance * kVertexMergeDistance;
    std::vector<uint32_t> dedup(vertices.size(), UINT32_MAX);
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        if (dedup[i] != UINT32_MAX) {
            // This vertex, and all following vertices that are close to it are
            // already deduplicated.
            continue;
        }
        const GridCell home = gridCellOf(vertices[i], cellSize);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto found = cells.find({home.x + dx, home.y + dy, home.z + dz});
                    if (found == cells.end()) {
                        continue;
                    }
                    // The vertices after this one in the cell.
                    const std::vector<uint32_t> &inCell = found->second;
                    for (auto it = std::upper_bound(inCell.begin(), inCell.end(), i); it != inCell.end(); ++it) {
                        if (glm::distance2(vertices[i], vertices[*it]) < eps2) {
                            dedup[*it] = i;
                        }
                    }
                }
            }
        }
    }
    return dedup;
}

// Each edge of a face is joined to a face whose edge runs between the same
// two vertices the other way; where several do, to the highest-numbered.
// The edges are sorted by their vertices, so each is found by a search.
static void joinFaces(Uniwalk &uni) {
    struct DirectedEdge {
        uint64_t key;
        uint32_t face;
    };
    const auto keyOf = [](uint32_t from, uint32_t to) {
        return (static_cast<uint64_t>(from) << 32) | to;
    };
    std::vector<DirectedEdge> edges;
    edges.reserve(uni.faces.size() * 3);
    for (uint32_t f = 0; f < uni.faces.size(); ++f) {
        const Uniface &face = uni.faces[f];
        for (uint32_t i = 0; i < 3; ++i) {
            edges.push_back({keyOf(face.vertices[kTriangleEdges[i][0]], face.vertices[kTriangleEdges[i][1]]), f});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const DirectedEdge &a, const DirectedEdge &b) {
        return a.key < b.key || (a.key == b.key && a.face < b.face);
    });
    const auto byKey = [](const DirectedEdge &a, const DirectedEdge &b) {
        return a.key < b.key;
    };
    for (uint32_t f = 0; f < uni.faces.size(); ++f) {
        Uniface &face = uni.faces[f];
        for (uint32_t i = 0; i < 3; ++i) {
            const DirectedEdge reversed {keyOf(face.vertices[kTriangleEdges[i][1]], face.vertices[kTriangleEdges[i][0]]), 0};
            const auto range = std::equal_range(edges.begin(), edges.end(), reversed, byKey);
            for (auto it = range.second; it != range.first; --it) {
                if (std::prev(it)->face != f) {
                    face.adjecent[i] = std::prev(it)->face;
                    break;
                }
            }
        }
    }
}

// Cutting the walkable ground by obstacles. A face under an obstacle is cut
// into convex pieces: those outside it, and the one inside, which is marked
// blocked. Each edge of a piece lies on a carrier: an edge of a face, or the
// line through an edge of an obstacle. The corner where two carriers cross is
// made once and shared by every piece that has it, and a corner lying on a
// carrier between the ends of a piece's edge there is put in that edge, so
// pieces and faces still join edge to edge.

// A corner this near a cutting line, in metres, lies on it, and corners this
// near each other are one: where three lines or more cross at a point, each
// pair makes its own corner there.
static constexpr float kCutTolerance = 1e-4f;
// Obstacles are filed by the cells of a grid this wide.
static constexpr float kCutCellSize = 4.0f;
// A piece is fanned from one of its corners only if every triangle has more
// than this, twice its ground in square metres, under it.
static constexpr float kMinFanArea = 1e-6f;

struct CutPiece {
    std::vector<uint32_t> corners;
    /** For each corner, the carrier of the edge from it to the next. */
    std::vector<uint64_t> carriers;
    bool blocked {false};
};

// Twice a polygon's area across the ground plan, positive when its corners
// run counter-clockwise seen from above.
static float doubleArea(const std::vector<uint32_t> &corners, const std::vector<glm::vec3> &vertices) {
    float area = 0.0f;
    for (size_t i = 0; i < corners.size(); ++i) {
        const glm::vec3 &p = vertices[corners[i]];
        const glm::vec3 &q = vertices[corners[(i + 1) % corners.size()]];
        area += p.x * q.y - q.x * p.y;
    }
    return area;
}

// The carrier of a face's edge between two vertices.
static uint64_t faceEdgeCarrier(uint32_t a, uint32_t b) {
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
}

// Obstacles' edges that lie on one line, as the faces of one placeable's
// walkmesh share edges, share its carrier: each edge's carrier, by obstacle
// and edge. Edges are sorted by the angle of their line; edges of near the
// same angle lie on one line when both ends of the one lie this near the
// other's line, in metres.
static constexpr float kSameLineDistance = 1e-4f;
static constexpr float kSameLineAngle = 1e-3f;

static std::vector<std::vector<uint64_t>> obstacleLineCarriers(const std::vector<Obstacle> &obstacles) {
    struct EdgeLine {
        float angle;
        uint32_t edge;
    };
    std::vector<glm::vec2> from, to;
    std::vector<EdgeLine> lines;
    std::vector<std::vector<uint64_t>> carriers(obstacles.size());
    for (uint32_t o = 0; o < obstacles.size(); ++o) {
        const auto &outline = obstacles[o].outline;
        for (size_t k = 0; k < outline.size(); ++k) {
            const glm::vec2 &p = outline[k];
            const glm::vec2 &q = outline[(k + 1) % outline.size()];
            float angle = std::atan2(q.y - p.y, q.x - p.x);
            if (angle < 0.0f) {
                angle += glm::pi<float>();
            }
            if (angle >= glm::pi<float>()) {
                angle -= glm::pi<float>();
            }
            const uint32_t edge = static_cast<uint32_t>(from.size());
            from.push_back(p);
            to.push_back(q);
            lines.push_back({angle, edge});
            carriers[o].push_back(edge);
        }
    }
    // A line near the end of the turn is the same as one near its start.
    const size_t count = lines.size();
    for (size_t i = 0; i < count; ++i) {
        if (lines[i].angle < kSameLineAngle) {
            lines.push_back({lines[i].angle + glm::pi<float>(), lines[i].edge});
        }
    }
    std::sort(lines.begin(), lines.end(), [](const EdgeLine &a, const EdgeLine &b) {
        return a.angle < b.angle || (a.angle == b.angle && a.edge < b.edge);
    });
    std::vector<uint32_t> root(from.size());
    for (uint32_t i = 0; i < root.size(); ++i) {
        root[i] = i;
    }
    const auto find = [&root](uint32_t e) {
        while (root[e] != e) {
            root[e] = root[root[e]];
            e = root[e];
        }
        return e;
    };
    const auto onLine = [&](uint32_t line, const glm::vec2 &point) {
        const glm::vec2 along {to[line] - from[line]};
        const float length = glm::length(along);
        return length > 0.0f && std::fabs(along.x * (point.y - from[line].y) - along.y * (point.x - from[line].x)) / length < kSameLineDistance;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        for (size_t j = i + 1; j < lines.size() && lines[j].angle - lines[i].angle < kSameLineAngle; ++j) {
            const uint32_t a = lines[i].edge;
            const uint32_t b = lines[j].edge;
            if (a != b && onLine(a, from[b]) && onLine(a, to[b])) {
                root[find(b)] = find(a);
            }
        }
    }
    for (auto &obstacleCarriers : carriers) {
        for (uint64_t &carrier : obstacleCarriers) {
            carrier = (1ull << 63) | find(static_cast<uint32_t>(carrier));
        }
    }
    return carriers;
}

// The corners made by the cuts: where two carriers cross, and the corners on
// each carrier.
struct CarrierPairHash {
    size_t operator()(const std::pair<uint64_t, uint64_t> &pair) const {
        size_t hash = 0;
        boost::hash_combine(hash, pair.first);
        boost::hash_combine(hash, pair.second);
        return hash;
    }
};

struct CutCorners {
    std::unordered_map<std::pair<uint64_t, uint64_t>, uint32_t, CarrierPairHash> crossings;
    std::unordered_map<uint64_t, std::vector<uint32_t>> onCarrier;
};

// The part of a convex piece on one side of the line through a and b, the
// carrier \p line: the left, seen from a toward b, when keepLeft is set. Where
// an edge crosses the line, the corner there is taken from \p cut, or made,
// its height taken along the edge.
static CutPiece clipToSide(const CutPiece &piece, std::vector<glm::vec3> &vertices, CutCorners &cut,
                           const glm::vec2 &a, const glm::vec2 &b, uint64_t line, bool keepLeft) {
    const glm::vec2 along {b - a};
    const float length = glm::length(along);
    CutPiece kept;
    kept.blocked = piece.blocked;
    if (length <= 0.0f) {
        return kept;
    }
    kept.corners.reserve(piece.corners.size() + 1);
    kept.carriers.reserve(piece.corners.size() + 1);
    const auto side = [&](const glm::vec3 &p) {
        const float left = (along.x * (p.y - a.y) - along.y * (p.x - a.x)) / length;
        return keepLeft ? left : -left;
    };
    const auto crossing = [&](uint32_t pi, uint32_t qi, uint64_t carrier, float t) {
        const auto key = std::minmax(carrier, line);
        const auto found = cut.crossings.find(key);
        if (found != cut.crossings.end()) {
            return found->second;
        }
        const uint32_t index = static_cast<uint32_t>(vertices.size());
        vertices.push_back(vertices[pi] + (vertices[qi] - vertices[pi]) * t);
        cut.crossings.emplace(key, index);
        cut.onCarrier[carrier].push_back(index);
        cut.onCarrier[line].push_back(index);
        return index;
    };
    const size_t count = piece.corners.size();
    for (size_t i = 0; i < count; ++i) {
        const uint32_t pi = piece.corners[i];
        const uint32_t qi = piece.corners[(i + 1) % count];
        const uint64_t carrier = piece.carriers[i];
        const float sp = side(vertices[pi]);
        const float sq = side(vertices[qi]);
        const bool pIn = sp >= -kCutTolerance;
        const bool qOut = sq < -kCutTolerance;
        if (pIn) {
            // From a corner on the line, an edge leaving the kept side is
            // followed by the line instead.
            kept.corners.push_back(pi);
            kept.carriers.push_back(qOut && sp <= kCutTolerance ? line : carrier);
        }
        if ((sp > kCutTolerance && qOut) || (sp < -kCutTolerance && sq > kCutTolerance)) {
            kept.corners.push_back(crossing(pi, qi, carrier, sp / (sp - sq)));
            kept.carriers.push_back(qOut ? line : carrier);
        }
    }
    if (kept.corners.size() < 3) {
        kept.corners.clear();
        kept.carriers.clear();
    }
    return kept;
}

// A convex piece less a convex obstacle: the parts outside each of the
// obstacle's edges in turn, then what is left, inside the obstacle and so
// blocked.
static void subtractObstacle(const CutPiece &piece, const Obstacle &obstacle, const std::vector<uint64_t> &lines,
                             std::vector<glm::vec3> &vertices, CutCorners &cut, std::vector<CutPiece> &out) {
    CutPiece rest = piece;
    const uint32_t count = static_cast<uint32_t>(obstacle.outline.size());
    for (uint32_t k = 0; k < count && !rest.corners.empty(); ++k) {
        const glm::vec2 &a = obstacle.outline[k];
        const glm::vec2 &b = obstacle.outline[(k + 1) % count];
        const uint64_t line = lines[k];
        CutPiece outside = clipToSide(rest, vertices, cut, a, b, line, false);
        if (!outside.corners.empty()) {
            out.push_back(std::move(outside));
        }
        rest = clipToSide(rest, vertices, cut, a, b, line, true);
    }
    if (!rest.corners.empty()) {
        rest.blocked = true;
        out.push_back(std::move(rest));
    }
}

// Cells of a grid across the ground plan, keyed for a hash map.
static uint64_t cutCellKey(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
}

static int cutCell(float coordinate) {
    return static_cast<int>(std::floor(coordinate / kCutCellSize));
}

// Cuts every face under an obstacle, between the obstacle's heights of floor,
// into pieces; puts each corner the cuts made in the edges it lies on; and
// puts the pieces, as triangles, in place of the faces, each room's together
// and in order. Uniwalk::blocked marks the triangles inside an obstacle.
static void cutByObstacles(Uniwalk &uni, const std::vector<Obstacle> &obstacles) {
    // The faces are put back room by room, so a face outside every room's
    // range would be lost: a walkmesh put together otherwise is left whole.
    size_t inRooms = 0;
    for (const Uniroom &room : uni.rooms) {
        inRooms += room.end - room.begin;
    }
    if (inRooms != uni.faces.size()) {
        return;
    }

    // Obstacles are filed by the cells their boxes cover.
    std::vector<glm::vec2> obstacleMin(obstacles.size()), obstacleMax(obstacles.size());
    std::unordered_map<uint64_t, std::vector<uint32_t>> obstacleCells;
    for (uint32_t o = 0; o < obstacles.size(); ++o) {
        glm::vec2 min {FLT_MAX};
        glm::vec2 max {-FLT_MAX};
        for (const glm::vec2 &p : obstacles[o].outline) {
            min = glm::min(min, p);
            max = glm::max(max, p);
        }
        obstacleMin[o] = min;
        obstacleMax[o] = max;
        if (obstacles[o].outline.size() < 3) {
            continue;
        }
        for (int y = cutCell(min.y); y <= cutCell(max.y); ++y) {
            for (int x = cutCell(min.x); x <= cutCell(max.x); ++x) {
                obstacleCells[cutCellKey(x, y)].push_back(o);
            }
        }
    }

    // Each face is cut by the obstacles over it, one after another.
    const std::vector<std::vector<uint64_t>> lineCarriers = obstacleLineCarriers(obstacles);
    CutCorners cut;
    std::vector<std::vector<CutPiece>> pieces(uni.faces.size());
    std::vector<uint32_t> seen(obstacles.size(), UINT32_MAX);
    bool anyCut = false;
    for (uint32_t f = 0; f < uni.faces.size(); ++f) {
        const uint32_t *v = uni.faces[f].vertices;
        pieces[f].push_back({{v[0], v[1], v[2]},
                             {faceEdgeCarrier(v[0], v[1]), faceEdgeCarrier(v[1], v[2]), faceEdgeCarrier(v[2], v[0])},
                             false});
        glm::vec3 min {FLT_MAX};
        glm::vec3 max {-FLT_MAX};
        for (int i = 0; i < 3; ++i) {
            min = glm::min(min, uni.vertices[v[i]]);
            max = glm::max(max, uni.vertices[v[i]]);
        }
        for (int y = cutCell(min.y); y <= cutCell(max.y); ++y) {
            for (int x = cutCell(min.x); x <= cutCell(max.x); ++x) {
                const auto found = obstacleCells.find(cutCellKey(x, y));
                if (found == obstacleCells.end()) {
                    continue;
                }
                for (uint32_t o : found->second) {
                    if (seen[o] == f) {
                        continue;
                    }
                    seen[o] = f;
                    const Obstacle &obstacle = obstacles[o];
                    if (obstacleMin[o].x > max.x || obstacleMax[o].x < min.x || obstacleMin[o].y > max.y || obstacleMax[o].y < min.y ||
                        obstacle.floorMin > max.z || obstacle.floorMax < min.z) {
                        continue;
                    }
                    std::vector<CutPiece> next;
                    for (const CutPiece &piece : pieces[f]) {
                        subtractObstacle(piece, obstacle, lineCarriers[o], uni.vertices, cut, next);
                    }
                    pieces[f] = std::move(next);
                    anyCut = true;
                }
            }
        }
    }
    if (!anyCut) {
        return;
    }

    // Corners at one point are made one, and each carries the carriers of
    // all: each corner of a cut face's pieces, in order, joins the first
    // corner before it within kCutTolerance. Only those corners are looked
    // at, not vertices the first merge left unused, which no face shares. A
    // piece left with fewer than three corners is dropped.
    std::vector<uint32_t> dedup(uni.vertices.size(), UINT32_MAX);
    {
        std::vector<uint32_t> corners;
        for (uint32_t f = 0; f < pieces.size(); ++f) {
            if (pieces[f].size() == 1 && pieces[f][0].corners.size() == 3 && !pieces[f][0].blocked) {
                continue;
            }
            for (const CutPiece &piece : pieces[f]) {
                corners.insert(corners.end(), piece.corners.begin(), piece.corners.end());
            }
        }
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        const float cellSize = 2.0f * kCutTolerance;
        const float eps2 = kCutTolerance * kCutTolerance;
        std::unordered_map<GridCell, std::vector<uint32_t>, GridCellHash> cells;
        for (uint32_t v : corners) {
            const GridCell home = gridCellOf(uni.vertices[v], cellSize);
            uint32_t into = UINT32_MAX;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto found = cells.find({home.x + dx, home.y + dy, home.z + dz});
                        if (found == cells.end()) {
                            continue;
                        }
                        for (uint32_t u : found->second) {
                            if (u < into && glm::distance2(uni.vertices[u], uni.vertices[v]) < eps2) {
                                into = u;
                            }
                        }
                    }
                }
            }
            if (into != UINT32_MAX) {
                dedup[v] = dedup[into] != UINT32_MAX ? dedup[into] : into;
            } else {
                cells[home].push_back(v);
            }
        }
    }
    const auto merged = [&dedup](uint32_t v) {
        return dedup[v] != UINT32_MAX ? dedup[v] : v;
    };
    for (auto &facePieces : pieces) {
        for (CutPiece &piece : facePieces) {
            CutPiece kept;
            kept.blocked = piece.blocked;
            for (size_t i = 0; i < piece.corners.size(); ++i) {
                const uint32_t v = merged(piece.corners[i]);
                if (!kept.corners.empty() && kept.corners.back() == v) {
                    // The edge before this corner has gone: the corner keeps
                    // the carrier of the edge after it.
                    kept.carriers.back() = piece.carriers[i];
                    continue;
                }
                kept.corners.push_back(v);
                kept.carriers.push_back(piece.carriers[i]);
            }
            if (kept.corners.size() > 1 && kept.corners.back() == kept.corners.front()) {
                kept.corners.pop_back();
                kept.carriers.pop_back();
            }
            if (kept.corners.size() < 3) {
                kept.corners.clear();
                kept.carriers.clear();
            }
            piece = std::move(kept);
        }
    }
    for (auto &[carrier, corners] : cut.onCarrier) {
        for (uint32_t &v : corners) {
            v = merged(v);
        }
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
    }

    // The pieces become triangles, each fanned out from a corner whose
    // triangles all turn the piece's way with ground under them; failing one,
    // from a corner added at its middle.
    std::vector<Uniface> faces;
    std::vector<uint8_t> blocked;
    std::vector<uint32_t> newIndex(uni.faces.size(), UINT32_MAX);
    std::vector<uint32_t> newEnd(uni.faces.size(), UINT32_MAX);
    const auto addFace = [&](uint32_t a, uint32_t b, uint32_t c, bool isBlocked) {
        if (a == b || b == c || c == a) {
            return;
        }
        Uniface face;
        face.vertices[0] = a;
        face.vertices[1] = b;
        face.vertices[2] = c;
        for (uint32_t &adj : face.adjecent) {
            adj = UINT32_MAX;
        }
        face.centroid = (uni.vertices[a] + uni.vertices[b] + uni.vertices[c]) * 0.333333f;
        faces.push_back(face);
        blocked.push_back(isBlocked ? 1 : 0);
    };
    for (Uniroom &room : uni.rooms) {
        const uint32_t begin = static_cast<uint32_t>(faces.size());
        for (uint32_t f = room.begin; f < room.end; ++f) {
            newIndex[f] = static_cast<uint32_t>(faces.size());
            for (const CutPiece &piece : pieces[f]) {
                std::vector<uint32_t> corners;
                for (size_t e = 0; e < piece.corners.size(); ++e) {
                    const uint32_t a = piece.corners[e];
                    const uint32_t b = piece.corners[(e + 1) % piece.corners.size()];
                    corners.push_back(a);
                    const auto found = cut.onCarrier.find(piece.carriers[e]);
                    if (found == cut.onCarrier.end()) {
                        continue;
                    }
                    // The corners on this edge's carrier between its ends, in
                    // order along it.
                    const glm::vec3 &pa = uni.vertices[a];
                    const glm::vec3 along {uni.vertices[b] - pa};
                    const float length2 = glm::dot(along, along);
                    std::vector<std::pair<float, uint32_t>> between;
                    for (uint32_t v : found->second) {
                        if (v == a || v == b || length2 <= 0.0f) {
                            continue;
                        }
                        const float t = glm::dot(uni.vertices[v] - pa, along) / length2;
                        if (t > 1e-6f && t < 1.0f - 1e-6f) {
                            between.push_back({t, v});
                        }
                    }
                    std::sort(between.begin(), between.end());
                    for (const auto &[t, v] : between) {
                        if (corners.back() != v) {
                            corners.push_back(v);
                        }
                    }
                }
                if (corners.size() < 3) {
                    continue;
                }
                const size_t n = corners.size();
                const float way = doubleArea(corners, uni.vertices) > 0.0f ? 1.0f : -1.0f;
                const auto fans = [&](size_t root) {
                    const glm::vec3 &a = uni.vertices[corners[root]];
                    for (size_t j = 1; j + 1 < n; ++j) {
                        const glm::vec3 &b = uni.vertices[corners[(root + j) % n]];
                        const glm::vec3 &c = uni.vertices[corners[(root + j + 1) % n]];
                        if (way * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) <= kMinFanArea) {
                            return false;
                        }
                    }
                    return true;
                };
                size_t root = n;
                for (size_t i = 0; i < n && root == n; ++i) {
                    if (fans(i)) {
                        root = i;
                    }
                }
                if (root < n) {
                    for (size_t j = 1; j + 1 < n; ++j) {
                        addFace(corners[root], corners[(root + j) % n], corners[(root + j + 1) % n], piece.blocked);
                    }
                    continue;
                }
                glm::vec3 middle {0.0f};
                for (uint32_t v : corners) {
                    middle += uni.vertices[v];
                }
                const uint32_t centre = static_cast<uint32_t>(uni.vertices.size());
                uni.vertices.push_back(middle / static_cast<float>(corners.size()));
                for (size_t i = 0; i < corners.size(); ++i) {
                    addFace(centre, corners[i], corners[(i + 1) % corners.size()], piece.blocked);
                }
            }
            newEnd[f] = static_cast<uint32_t>(faces.size());
        }
        room.begin = begin;
        room.end = static_cast<uint32_t>(faces.size());
        room.unifiedFaceEnds.resize(room.unifiedFaces.size(), UINT32_MAX);
        for (size_t i = 0; i < room.unifiedFaces.size(); ++i) {
            const uint32_t f = room.unifiedFaces[i];
            if (f == UINT32_MAX) {
                continue;
            }
            room.unifiedFaces[i] = newIndex[f];
            room.unifiedFaceEnds[i] = newEnd[f];
        }
    }
    uni.faces = std::move(faces);
    uni.blocked = std::move(blocked);
}

// The cells of the edge grid a box across the ground plan covers, clamped to
// the grid.
struct GridSpan {
    int x0, y0, x1, y1;
};

static GridSpan gridSpan(const EdgeGrid &grid, const glm::vec2 &min, const glm::vec2 &max) {
    const auto cell = [&grid](float offset, int count) {
        return static_cast<int>(std::clamp(std::floor(offset / grid.cellSize), 0.0f, static_cast<float>(count - 1)));
    };
    return {cell(min.x - grid.origin.x, grid.columns), cell(min.y - grid.origin.y, grid.rows),
            cell(max.x - grid.origin.x, grid.columns), cell(max.y - grid.origin.y, grid.rows)};
}

static void edgeBounds(const Uniwalk &uni, uint32_t edge, glm::vec2 &min, glm::vec2 &max) {
    const Uniface &face = uni.faces[edge / 3];
    const glm::vec2 a {uni.vertices[face.vertices[kTriangleEdges[edge % 3][0]]]};
    const glm::vec2 b {uni.vertices[face.vertices[kTriangleEdges[edge % 3][1]]]};
    min = glm::min(a, b);
    max = glm::max(a, b);
}

// Whether an edge of a face bounds the walkable ground: no other walkable face
// shares it, or the face across it lies inside an obstacle and this one does
// not.
static bool isGroundEdge(const Uniwalk &uni, uint32_t face, uint32_t j) {
    const uint32_t adj = uni.faces[face].adjecent[j];
    return adj == UINT32_MAX || (!uni.blocked.empty() && uni.blocked[adj] && !uni.blocked[face]);
}

// Files every edge of the walkable ground in each cell its box across the
// ground plan covers.
static void buildEdgeGrid(Uniwalk &uni) {
    EdgeGrid &grid = uni.edgeGrid;
    grid = EdgeGrid();
    std::vector<uint32_t> edges;
    glm::vec2 min {FLT_MAX};
    glm::vec2 max {-FLT_MAX};
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            if (!isGroundEdge(uni, i, j)) {
                continue;
            }
            const uint32_t edge = i * 3 + j;
            glm::vec2 edgeMin, edgeMax;
            edgeBounds(uni, edge, edgeMin, edgeMax);
            min = glm::min(min, edgeMin);
            max = glm::max(max, edgeMax);
            edges.push_back(edge);
        }
    }
    if (edges.empty()) {
        return;
    }

    const glm::vec2 extent {max - min};
    float cellSize = kEdgeGridCellSize;
    const auto cellsAcross = [](float length, float size) {
        return static_cast<int64_t>(std::floor(length / size)) + 1;
    };
    while (cellsAcross(extent.x, cellSize) * cellsAcross(extent.y, cellSize) > kMaxEdgeGridCells) {
        cellSize *= 2.0f;
    }
    grid.origin = min;
    grid.cellSize = cellSize;
    grid.columns = static_cast<int>(cellsAcross(extent.x, cellSize));
    grid.rows = static_cast<int>(cellsAcross(extent.y, cellSize));

    // Count each cell's edges, then file them.
    grid.cellStarts.assign(static_cast<size_t>(grid.columns) * grid.rows + 1, 0);
    const auto forCells = [&](uint32_t edge, const auto &visit) {
        glm::vec2 edgeMin, edgeMax;
        edgeBounds(uni, edge, edgeMin, edgeMax);
        const GridSpan span = gridSpan(grid, edgeMin, edgeMax);
        for (int y = span.y0; y <= span.y1; ++y) {
            for (int x = span.x0; x <= span.x1; ++x) {
                visit(static_cast<size_t>(y) * grid.columns + x);
            }
        }
    };
    for (uint32_t edge : edges) {
        forCells(edge, [&grid](size_t cell) { ++grid.cellStarts[cell + 1]; });
    }
    for (size_t i = 1; i < grid.cellStarts.size(); ++i) {
        grid.cellStarts[i] += grid.cellStarts[i - 1];
    }
    grid.edges.resize(grid.cellStarts.back());
    std::vector<uint32_t> filled(grid.cellStarts.begin(), grid.cellStarts.end() - 1);
    for (uint32_t edge : edges) {
        forCells(edge, [&](size_t cell) { grid.edges[filled[cell]++] = edge; });
    }
}

void uniwalkFinalize(struct Uniwalk &uni, const std::vector<Obstacle> &obstacles) {
    // Deduplicate vertices, so that the same vertex gets the same index in all
    // faces. This way we can compare indices of two vertices instead of
    // calculating distance between them every time.
    const std::vector<uint32_t> dedup = mergeVertices(uni.vertices);
    for (Uniface &face : uni.faces) {
        for (uint32_t &v : face.vertices) {
            if (dedup[v] != UINT32_MAX) {
                v = dedup[v];
            }
        }
    }

    uni.blocked.clear();
    if (!obstacles.empty()) {
        cutByObstacles(uni, obstacles);
    }

    // For each faces, find faces that are adjecent to it.
    joinFaces(uni);

    // A vertex of an edge of the walkable ground lies on its edge.
    uni.boundary.assign(uni.vertices.size(), 0);
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        const Uniface &face = uni.faces[i];
        for (uint32_t j = 0; j < 3; ++j) {
            if (isGroundEdge(uni, i, j)) {
                uni.boundary[face.vertices[kTriangleEdges[j][0]]] = 1;
                uni.boundary[face.vertices[kTriangleEdges[j][1]]] = 1;
            }
        }
    }

    buildEdgeGrid(uni);

    // Faces joined through shared edges form one region; a path exists
    // between two faces exactly when they share a region.
    uni.regions.resize(uni.faces.size());
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        uni.regions[i] = i;
    }
    auto root = [&uni](uint32_t face) {
        while (uni.regions[face] != face) {
            uni.regions[face] = uni.regions[uni.regions[face]];
            face = uni.regions[face];
        }
        return face;
    };
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        for (uint32_t adj : uni.faces[i].adjecent) {
            if (adj == UINT32_MAX) {
                continue;
            }
            uni.regions[root(adj)] = root(i);
        }
    }
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        uni.regions[i] = root(i);
    }
}

// The paths planned over the walkmesh being replaced keep their faces where
// each face of a room's walkmesh they cross was cut into the same pieces in
// both, blocked or not alike: the same triangles, under their new indices. A
// path over any other face is left to be planned again as it is next
// followed.
static void carryPathsOver(Pathfinder &pf, const Uniwalk &old, uint32_t oldGeneration) {
    // Corners nearer than this are the same.
    static constexpr float kSameCorner = 1e-4f;
    const Uniwalk &uni = pf.uni;
    if (old.rooms.size() != uni.rooms.size()) {
        return;
    }
    for (size_t r = 0; r < uni.rooms.size(); ++r) {
        if (old.rooms[r].walkmesh != uni.rooms[r].walkmesh ||
            old.rooms[r].unifiedFaces.size() != uni.rooms[r].unifiedFaces.size()) {
            return;
        }
    }
    const auto pieces = [](const Uniroom &room, size_t face) -> std::pair<uint32_t, uint32_t> {
        const uint32_t first = room.unifiedFaces[face];
        if (first == UINT32_MAX) {
            return {0, 0};
        }
        const uint32_t end = face < room.unifiedFaceEnds.size() ? room.unifiedFaceEnds[face] : first + 1;
        return {first, std::max(first, end)};
    };
    const auto blocked = [](const Uniwalk &walk, uint32_t face) {
        return !walk.blocked.empty() && walk.blocked[face] != 0;
    };
    const auto same = [&](uint32_t before, uint32_t after) {
        if (blocked(old, before) != blocked(uni, after)) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (glm::distance2(old.vertices[old.faces[before].vertices[i]], uni.vertices[uni.faces[after].vertices[i]]) >
                kSameCorner * kSameCorner) {
                return false;
            }
        }
        return true;
    };
    // Each old face's new index, when it is carried over.
    std::vector<uint32_t> carried;
    for (AStarPath &path : pf.paths) {
        if (!path.active || path.straight || !path.followsFaces || path.generation != oldGeneration) {
            continue;
        }
        if (carried.empty()) {
            carried.assign(old.faces.size(), UINT32_MAX);
            for (size_t r = 0; r < old.rooms.size(); ++r) {
                for (size_t f = 0; f < old.rooms[r].unifiedFaces.size(); ++f) {
                    const auto [beforeFirst, beforeEnd] = pieces(old.rooms[r], f);
                    const auto [afterFirst, afterEnd] = pieces(uni.rooms[r], f);
                    if (beforeEnd - beforeFirst != afterEnd - afterFirst) {
                        continue;
                    }
                    bool all = true;
                    for (uint32_t k = 0; all && k < beforeEnd - beforeFirst; ++k) {
                        all = same(beforeFirst + k, afterFirst + k);
                    }
                    for (uint32_t k = 0; all && k < beforeEnd - beforeFirst; ++k) {
                        carried[beforeFirst + k] = afterFirst + k;
                    }
                }
            }
        }
        std::vector<uint32_t> faces;
        faces.reserve(path.faces.size());
        for (uint32_t face : path.faces) {
            if (face >= carried.size() || carried[face] == UINT32_MAX) {
                break;
            }
            faces.push_back(carried[face]);
        }
        if (faces.size() == path.faces.size()) {
            path.faces = std::move(faces);
            path.generation = pf.generation;
        }
    }
}

void replaceUniwalk(Pathfinder &pf, Uniwalk uni) {
    Uniwalk old = std::move(pf.uni);
    pf.uni = std::move(uni);
    const uint32_t oldGeneration = pf.generation++;
    carryPathsOver(pf, old, oldGeneration);
    drawDebugUniwalk(pf.uni);
}

struct CastResult {
    glm::vec3 intersection;
    bool intersects;
};

static CastResult castFace(const Uniwalk &uni, uint32_t face, glm::vec3 position) {
    // Offset position slightly to prevent rounding errors.
    glm::vec3 castPosition = position + glm::vec3(0.0f, 0.0f, 0.1f);
    float distance = 0.0f;
    glm::vec2 baryPosition(0.0f);
    glm::vec3 down(0.0f, 0.0f, -1.0f);

    const uint32_t *indices = uni.faces[face].vertices;
    glm::vec3 v0 = uni.vertices[indices[0]];
    glm::vec3 v1 = uni.vertices[indices[1]];
    glm::vec3 v2 = uni.vertices[indices[2]];

    bool intersects = glm::intersectRayTriangle(castPosition, down, v0, v1, v2, baryPosition, distance);
    glm::vec3 intersection = castPosition + down * distance;
    return {intersection, intersects};
}

// Whether a point stands on a face: the vertical through it meets the face no
// more than kFloorHeadroom under it, so a floor beneath its own does not count.
static bool standsOnFace(const Uniwalk &uni, uint32_t face, const glm::vec3 &position) {
    const CastResult res = castFace(uni, face, position);
    return res.intersects && position.z - res.intersection.z <= kFloorHeadroom;
}

// The unified face a point stands on. With the area's locator, it is the face
// the area finds, so a walk and its path keep to the same floor; a point the
// area finds on a face that is not walkable stands on none. Without one, it is
// found by the same rule over the walkable faces: the highest face on the
// vertical through the point, unless that lies more than kFloorHeadroom over
// the point, when the highest within kFloorHeadroom of its height is stood on
// instead, if any.
static uint32_t findFaceAt(const Pathfinder &pf, const glm::vec3 &position) {
    const Uniwalk &uni = pf.uni;
    if (pf.locate) {
        const auto ref = pf.locate(position);
        if (!ref) {
            return UINT32_MAX;
        }
        for (const Uniroom &room : uni.rooms) {
            if (room.walkmesh != ref->walkmesh) {
                continue;
            }
            if (ref->face >= room.unifiedFaces.size() || room.unifiedFaces[ref->face] == UINT32_MAX) {
                return UINT32_MAX;
            }
            // A face cut by obstacles became several: the point stands on the
            // one whose outline holds it, or the first.
            const uint32_t first = room.unifiedFaces[ref->face];
            const uint32_t end = ref->face < room.unifiedFaceEnds.size() ? room.unifiedFaceEnds[ref->face] : first + 1;
            for (uint32_t f = first; f < end; ++f) {
                if (castFace(uni, f, glm::vec3(position.x, position.y, uni.faces[f].centroid.z + kFloorHeadroom)).intersects) {
                    return f;
                }
            }
            return first < end ? first : UINT32_MAX;
        }
        return UINT32_MAX;
    }

    uint32_t highest = UINT32_MAX;
    float highestZ = 0.0f;
    uint32_t level = UINT32_MAX;
    float levelZ = 0.0f;
    for (const Uniroom &room : uni.rooms) {
        if (position.x > room.max.x || position.y > room.max.y || position.x < room.min.x || position.y < room.min.y) {
            continue;
        }
        const glm::vec3 top {position.x, position.y, room.max.z + 1.0f};
        for (uint32_t i = room.begin; i < room.end; ++i) {
            const CastResult res = castFace(uni, i, top);
            if (!res.intersects) {
                continue;
            }
            const float z = res.intersection.z;
            if (highest == UINT32_MAX || z > highestZ) {
                highest = i;
                highestZ = z;
            }
            if (std::fabs(z - position.z) <= kFloorHeadroom && (level == UINT32_MAX || z > levelZ)) {
                level = i;
                levelZ = z;
            }
        }
    }
    if (highest != UINT32_MAX && highestZ > position.z + kFloorHeadroom && level != UINT32_MAX) {
        return level;
    }
    return highest;
}

/// Portal is an edge between two consecutive faces on a path.
struct Portal {
    uint32_t left;
    uint32_t right;
};

/// Find an edge between \p face and \p nextFace.
static Portal findPortal(const Uniwalk &uni, uint32_t face, uint32_t nextFace) {
    uint32_t found[2];
    uint32_t numFound = 0;

    for (uint32_t vi : uni.faces[face].vertices) {
        for (uint32_t vj : uni.faces[nextFace].vertices) {
            if (vi != vj) {
                continue;
            }
            found[numFound] = vi;
            ++numFound;
            if (numFound == 2) {
                return Portal {found[0], found[1]};
            }
        }
    }
    assert(0 && "faces are not adjecent");
    return Portal {0, 0};
}

// Whether the walker is too wide for the gap between two faces: both ends of
// the shared edge lie on the edge of the walkable ground, and they are less
// than the walker's width apart across the ground.
static bool isNarrowGap(const Uniwalk &uni, uint32_t face, uint32_t nextFace, float radius) {
    const Portal portal = findPortal(uni, face, nextFace);
    if (!uni.boundary[portal.left] || !uni.boundary[portal.right]) {
        return false;
    }
    const glm::vec2 across {glm::vec2(uni.vertices[portal.left]) - glm::vec2(uni.vertices[portal.right])};
    return glm::dot(across, across) < 4.0f * radius * radius;
}

// The open list is a binary heap with the face of least cost on top; of
// equal costs, the one put on the list first.
struct LaterOnList {
    bool operator()(const AStarOpenFace &a, const AStarOpenFace &b) const {
        return a.cost > b.cost || (a.cost == b.cost && a.order > b.order);
    }
};

static void pushOpenFace(std::vector<AStarOpenFace> &open, const AStarOpenFace &face) {
    open.push_back(face);
    std::push_heap(open.begin(), open.end(), LaterOnList());
}

/// Find a face with the minimum cost and remove it from the open list.
static AStarOpenFace popOpenFace(std::vector<AStarOpenFace> &open) {
    assert(!open.empty());
    std::pop_heap(open.begin(), open.end(), LaterOnList());
    AStarOpenFace face = open.back();
    open.pop_back();
    return face;
}

static bool findPathAStar(AStarPath &path, AStarContext &ctx, const Uniwalk &uni,
                          uint32_t fromFace, uint32_t toFace, const FacePassable &passable, float radius,
                          bool keepsOffBlocked) {
    // Reset the context.
    ctx.state.resize(uni.faces.size());
    memset(&ctx.state[0], 0xFF, sizeof(ctx.state[0]) * ctx.state.size());
    ctx.open.resize(0);

    // Initialize the algorithm. Find a reverse path - a path from toFace to
    // fromFace. When the algorithm reaches the fromFace, it backtracks and
    // reverses the path, so it transforms to natural order [fromFace, toFace].
    // A face is ranked by the way travelled to it, centroid to centroid, and
    // the straight line from it to the goal, fromFace; the line never
    // overstates what is left, so the first way to reach the goal is the
    // shortest over the centroids.
    const glm::vec3 goal = uni.faces[fromFace].centroid;
    ctx.state[toFace] = {AStarFace::Open, UINT32_MAX, 0.0f};
    uint32_t order = 0;
    pushOpenFace(ctx.open, {toFace, glm::distance(uni.faces[toFace].centroid, goal), order++});

    // Each face is asked once a search whether the walker may pass over it.
    // The search starts from the face the walk ends on and ends on the face
    // the walk starts from, so neither is asked.
    if (passable) {
        ctx.passes.assign(uni.faces.size(), -1);
    }
    const auto mayPass = [&](uint32_t face) {
        if (face == fromFace) {
            return true;
        }
        if (keepsOffBlocked && uni.blocked[face]) {
            return false;
        }
        if (!passable) {
            return true;
        }
        int8_t &passes = ctx.passes[face];
        if (passes < 0) {
            passes = passable(face) ? 1 : 0;
        }
        return passes != 0;
    };

    while (!ctx.open.empty()) {
        // Extract face with least total cost from open list and close it. A
        // face reached by a shorter way was put on the list again, so an
        // entry for a face already closed is passed over.
        AStarOpenFace current = popOpenFace(ctx.open);
        if (ctx.state[current.index].flag == AStarFace::Closed) {
            continue;
        }
        ctx.state[current.index].flag = AStarFace::Closed;

        if (current.index == fromFace) {
            // Reached the destination face. Now reconstruct the path by
            // following the parents.
            uint32_t index = fromFace;
            path.faces.resize(0);
            while (index != UINT32_MAX) {
                path.faces.push_back(index);
                index = ctx.state[index].parent;
            }
            return true;
        }

        for (uint32_t adj : uni.faces[current.index].adjecent) {
            if (adj == UINT32_MAX) {
                continue;
            }

            // Skip adjacent vertex if it is closed, or one the walker may not
            // pass over, or reached through a gap too narrow for the walker.
            if (ctx.state[adj].flag == AStarFace::Closed || !mayPass(adj) ||
                (radius > 0.0f && isNarrowGap(uni, current.index, adj, radius))) {
                continue;
            }

            const glm::vec3 &adjCentroid = uni.faces[adj].centroid;
            const float travelled = ctx.state[current.index].travelled +
                                    glm::distance(uni.faces[current.index].centroid, adjCentroid);
            const float cost = travelled + glm::distance(adjCentroid, goal);

            if (ctx.state[adj].flag == AStarFace::Open) {
                if (travelled >= ctx.state[adj].travelled) {
                    continue;
                }
                // The adjacent face is reached by a shorter way through the
                // current face: it goes on the list again at its new cost.
                pushOpenFace(ctx.open, {adj, cost, order++});
                ctx.state[adj].parent = current.index;
                ctx.state[adj].travelled = travelled;
            } else {
                // Open the adjecent face.
                pushOpenFace(ctx.open, {adj, cost, order++});
                ctx.state[adj] = {AStarFace::Open, current.index, travelled};
            }
        }
    }

    // Path not found.
    return false;
}

// The funnel works across the ground: a point lies to the left of the line
// from a to b when this is positive.
static float crossAcross(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool samePlace(const glm::vec3 &a, const glm::vec3 &b) {
    return glm::distance2(glm::vec2(a), glm::vec2(b)) < 1e-8f;
}

/// A gap of a path, its ends as seen walking through it.
struct FunnelPortal {
    glm::vec3 left;
    glm::vec3 right;
};

// The gaps between the path's faces in order. A gap's left and right are as
// seen going from its first face into the next: away from the first face's
// third corner. An end on the edge of the walkable ground is drawn in along
// the gap by the walker's radius, so the walk keeps that far from the corner;
// a gap too narrow for that closes to a point, its middle when both ends are
// drawn in.
static std::vector<FunnelPortal> pathPortals(const Uniwalk &uni, const AStarPath &path) {
    std::vector<FunnelPortal> portals;
    if (path.faces.size() < 2) {
        return portals;
    }
    portals.reserve(path.faces.size() - 1);
    for (size_t k = 0; k + 1 < path.faces.size(); ++k) {
        const Portal shared = findPortal(uni, path.faces[k], path.faces[k + 1]);
        const uint32_t *corners = uni.faces[path.faces[k]].vertices;
        uint32_t third = corners[0];
        for (int i = 0; i < 3; ++i) {
            if (corners[i] != shared.left && corners[i] != shared.right) {
                third = corners[i];
            }
        }
        const glm::vec3 &behind = uni.vertices[third];
        const glm::vec3 &a = uni.vertices[shared.left];
        const glm::vec3 &b = uni.vertices[shared.right];
        const glm::vec3 middle {(a + b) * 0.5f};
        const bool aLeft = crossAcross(behind, middle, a) > crossAcross(behind, middle, b);
        const uint32_t leftIndex = aLeft ? shared.left : shared.right;
        const uint32_t rightIndex = aLeft ? shared.right : shared.left;
        glm::vec3 left = uni.vertices[leftIndex];
        glm::vec3 right = uni.vertices[rightIndex];
        const float width = glm::length(glm::vec2(right) - glm::vec2(left));
        if (path.radius > 0.0f && width > 0.0f) {
            const bool leftDrawn = uni.boundary[leftIndex] != 0;
            const bool rightDrawn = uni.boundary[rightIndex] != 0;
            const float needed = (leftDrawn ? path.radius : 0.0f) + (rightDrawn ? path.radius : 0.0f);
            if (needed >= width) {
                if (leftDrawn && rightDrawn) {
                    left = right = (left + right) * 0.5f;
                } else if (leftDrawn) {
                    left = right;
                } else if (rightDrawn) {
                    right = left;
                }
            } else {
                const glm::vec3 inward {(right - left) * (path.radius / width)};
                if (leftDrawn) {
                    left += inward;
                }
                if (rightDrawn) {
                    right -= inward;
                }
            }
        }
        portals.push_back({left, right});
    }
    return portals;
}

// The straight legs from a start through the gaps [begin, end) to a goal,
// added to the path's points: each corner the funnel turns at, then the goal.
// Each point notes the gap it lies in, by the index of the face before it;
// the goal notes goalFace. A point in the same place as the one before is
// not added again.
static void funnelLegs(AStarPath &path, const std::vector<FunnelPortal> &portals, const glm::vec3 &start,
                       size_t begin, size_t end, const glm::vec3 &goal, uint32_t goalFace) {
    // The start and the goal stand at either end of the gaps as gaps of no
    // width.
    const size_t count = end - begin + 2;
    const auto gapAt = [&](size_t i) -> FunnelPortal {
        if (i == 0) {
            return {start, start};
        }
        if (i == count - 1) {
            return {goal, goal};
        }
        return portals[begin + i - 1];
    };
    const auto faceAt = [&](size_t i) {
        return i == 0 ? static_cast<uint32_t>(begin) : (i == count - 1 ? goalFace : static_cast<uint32_t>(begin + i - 1));
    };
    const auto add = [&path](const glm::vec3 &point, uint32_t face) {
        if (!path.points.empty() && samePlace(path.points.back(), point)) {
            return;
        }
        path.points.push_back(point);
        path.pointFaces.push_back(face);
    };

    glm::vec3 apex = start;
    glm::vec3 left = start;
    glm::vec3 right = start;
    size_t leftAt = 0;
    size_t rightAt = 0;
    for (size_t i = 1; i < count; ++i) {
        const FunnelPortal gap = gapAt(i);
        // A right end left of the funnel's right side narrows it, unless it
        // crosses over the left side: then the funnel turns at its left
        // corner, and goes on from the gap after it.
        if (crossAcross(apex, right, gap.right) >= 0.0f) {
            if (samePlace(apex, right) || crossAcross(apex, left, gap.right) < 0.0f) {
                right = gap.right;
                rightAt = i;
            } else {
                add(left, faceAt(leftAt));
                apex = left;
                right = left;
                rightAt = leftAt;
                i = leftAt;
                continue;
            }
        }
        // The same for a left end, mirrored.
        if (crossAcross(apex, left, gap.left) <= 0.0f) {
            if (samePlace(apex, left) || crossAcross(apex, right, gap.left) > 0.0f) {
                left = gap.left;
                leftAt = i;
            } else {
                add(right, faceAt(rightAt));
                apex = right;
                left = right;
                leftAt = rightAt;
                i = rightAt;
                continue;
            }
        }
    }
    add(goal, goalFace);
}

// The path's points, from its start through the funnel's corners to its end.
// Where the gaps climb or drop more than kFloorHeadroom from where the
// current straight leg started, the leg ends at the middle of the gap and the
// next starts there, so no leg crosses over or under another floor of the
// path.
static void planPoints(AStarPath &path, const Uniwalk &uni) {
    path.points.clear();
    path.pointFaces.clear();
    path.points.push_back(path.from);
    path.pointFaces.push_back(0);
    const std::vector<FunnelPortal> portals = pathPortals(uni, path);
    glm::vec3 start = path.from;
    size_t begin = 0;
    for (size_t j = 0; j < portals.size(); ++j) {
        const glm::vec3 middle {(portals[j].left + portals[j].right) * 0.5f};
        if (std::fabs(middle.z - start.z) > kFloorHeadroom) {
            funnelLegs(path, portals, start, begin, j, middle, static_cast<uint32_t>(j));
            start = middle;
            begin = j + 1;
        }
    }
    funnelLegs(path, portals, start, begin, portals.size(), path.to,
               static_cast<uint32_t>(path.faces.size() - 1));
    path.pointNext = 1;
}

// A path keeps clear of the faces the walker may not pass over, of those
// inside obstacles, and of gaps too narrow for it; when no path does, it is
// found over every face and gap but those inside obstacles, then over every
// one, and the funnel closes the gaps too narrow to a point. Faces \p keepOff
// turns down are kept off throughout. \p tier counts the searches it took
// beyond the first.
static bool searchPath(AStarPath &path, Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to,
                       const FacePassable &passable, float radius, const FacePassable &keepOff, int &tier) {
    const Uniwalk &uni = pf.uni;
    AStarContext &astar = pf.astar;
    uint32_t fromFace = findFaceAt(pf, from);
    uint32_t toFace = findFaceAt(pf, to);

    // Faces in regions of their own have no way between them: the search
    // would go over the whole region first.
    if (fromFace == UINT32_MAX || toFace == UINT32_MAX || uni.regions[fromFace] != uni.regions[toFace]) {
        return false;
    }

    FacePassable first = passable;
    if (keepOff) {
        first = [&passable, &keepOff](uint32_t face) {
            return keepOff(face) && (!passable || passable(face));
        };
    }
    // Faces inside obstacles are kept off as the faces the walker may not pass
    // over are, and longer: a path over every face and gap but those is
    // looked for before one over them.
    const bool hasBlocked = !uni.blocked.empty();
    tier = 0;
    bool found = findPathAStar(path, astar, uni, fromFace, toFace, first, radius, hasBlocked);
    if (!found && hasBlocked) {
        tier = 1;
        found = findPathAStar(path, astar, uni, fromFace, toFace, keepOff, 0.0f, true);
    }
    if (!found && (passable || radius > 0.0f || hasBlocked)) {
        tier = 2;
        found = findPathAStar(path, astar, uni, fromFace, toFace, keepOff, 0.0f, false);
    }
    if (!found) {
        return false;
    }

    drawdebug::pushId(path.index);
    drawdebug::pushId("findPath");
    drawdebug::clear();
    if (isShowPathEnabled()) {
        for (uint32_t i = 1; i < path.faces.size(); ++i) {
            glm::vec3 from = uni.faces[path.faces[i - 1]].centroid;
            glm::vec3 to = uni.faces[path.faces[i]].centroid;
            drawdebug::line(from, to, 0xCD2626FF, 0.1);
        }
    }
    drawdebug::popId();
    drawdebug::popId();

    path.from = from;
    path.to = to;
    path.radius = radius;
    path.current = 0;
    path.active = true;
    path.generation = pf.generation;
    path.replan = false;
    path.straight = false;
    path.followsFaces = true;
    planPoints(path, uni);

    if (isShowPathEnabled()) {
        drawdebug::pushId(path.index);
        drawdebug::pushId("funnel");
        drawdebug::clear();
        for (size_t i = 1; i < path.points.size(); ++i) {
            drawdebug::line(path.points[i - 1], path.points[i], 0xC0FF3EFF, 0.1f);
        }
        drawdebug::popId();
        drawdebug::popId();
    }
    return true;
}

// How far a point lies from a face across the ground plan: nothing when
// inside it.
static float groundDistanceToFace(const Uniwalk &uni, uint32_t face, const glm::vec2 &point) {
    glm::vec2 corners[3];
    for (int i = 0; i < 3; ++i) {
        corners[i] = glm::vec2(uni.vertices[uni.faces[face].vertices[i]]);
    }
    float nearest = FLT_MAX;
    int sides = 0;
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 &a = corners[i];
        const glm::vec2 &b = corners[(i + 1) % 3];
        const glm::vec2 along {b - a};
        const float cross = along.x * (point.y - a.y) - along.y * (point.x - a.x);
        sides += cross > 0.0f ? 1 : (cross < 0.0f ? -1 : 0);
        const float length2 = glm::dot(along, along);
        const float t = length2 > 0.0f ? std::clamp(glm::dot(point - a, along) / length2, 0.0f, 1.0f) : 0.0f;
        nearest = std::min(nearest, glm::distance(point, a + along * t));
    }
    return std::abs(sides) == 3 ? 0.0f : nearest;
}

LegBlocker nearestLegBlock(LegBlocker first, LegBlocker second) {
    if (!first || !second) {
        return first ? first : second;
    }
    return [first = std::move(first), second = std::move(second)](const glm::vec3 &from, const glm::vec3 &to) {
        auto a = first(from, to);
        auto b = second(from, to);
        if (!a || (b && glm::distance2(glm::vec2(from), glm::vec2(*b)) < glm::distance2(glm::vec2(from), glm::vec2(*a)))) {
            return b;
        }
        return a;
    };
}

// Where the path's legs first meet what \p legs finds.
static std::optional<glm::vec3> findLegBlock(const AStarPath &path, const LegBlocker &legs) {
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        if (auto meets = legs(path.points[i], path.points[i + 1])) {
            return meets;
        }
    }
    return std::nullopt;
}

// How far the walker walks along the path's points.
static float pathLength(const AStarPath &path) {
    float length = 0.0f;
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        length += glm::distance(path.points[i], path.points[i + 1]);
    }
    return length;
}

// A path is looked for as searchPath does. When \p legs finds one of its legs
// meeting something, the path's faces where the walker meets it are kept off
// and the path looked for again, a few times at most; met on the face the
// path starts or ends on, which is always crossed, the face it leaves or
// reaches that one by is kept off instead. A path so found is kept only when
// its legs meet nothing, it took no more searches than the first, and it is
// no more than a short way round: little longer than the first. Otherwise the
// first path stands, and the walk meets what is in its way as it would have.
static bool findPath(AStarPath &path, Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to,
                     const FacePassable &passable, float radius, const FacePassable &keepOff = {},
                     const LegBlocker &legs = {}) {
    static constexpr int kLegReplans = 3;
    // Faces within this far of where the walker meets something, or within
    // its radius if wider, are kept off.
    static constexpr float kLegKeepOff = 0.1f;
    // A way round may be longer than the first path by this share of it, or
    // by this many metres if more.
    static constexpr float kLegDetourShare = 0.25f;
    static constexpr float kLegDetourLength = 2.0f;
    int tier = 0;
    if (!searchPath(path, pf, from, to, passable, radius, keepOff, tier)) {
        return false;
    }
    if (!legs) {
        return true;
    }
    auto meets = findLegBlock(path, legs);
    const float firstLength = pathLength(path);
    const float longest = firstLength + std::max(firstLength * kLegDetourShare, kLegDetourLength);
    std::vector<uint32_t> kept;
    AStarPath trial;
    const AStarPath *blocked = &path;
    for (int attempt = 0; meets && attempt < kLegReplans; ++attempt) {
        const size_t before = kept.size();
        const float reach = std::max(radius, kLegKeepOff);
        const auto keep = [&kept](uint32_t face) {
            if (!std::binary_search(kept.begin(), kept.end(), face)) {
                kept.insert(std::upper_bound(kept.begin(), kept.end(), face), face);
            }
        };
        const std::vector<uint32_t> &faces = blocked->faces;
        const glm::vec2 at {*meets};
        for (size_t i = 1; i + 1 < faces.size(); ++i) {
            if (groundDistanceToFace(pf.uni, faces[i], at) <= reach) {
                keep(faces[i]);
            }
        }
        if (faces.size() > 2) {
            if (groundDistanceToFace(pf.uni, faces.front(), at) <= reach) {
                keep(faces[1]);
            }
            if (groundDistanceToFace(pf.uni, faces.back(), at) <= reach) {
                keep(faces[faces.size() - 2]);
            }
        }
        if (kept.size() == before) {
            break;
        }
        const FacePassable off = [&kept, &keepOff](uint32_t face) {
            return !std::binary_search(kept.begin(), kept.end(), face) && (!keepOff || keepOff(face));
        };
        trial = path;
        int trialTier = 0;
        if (!searchPath(trial, pf, from, to, passable, radius, off, trialTier) || trialTier > tier ||
            pathLength(trial) > longest) {
            break;
        }
        meets = findLegBlock(trial, legs);
        if (!meets) {
            path = std::move(trial);
            return true;
        }
        blocked = &trial;
    }
    return true;
}

bool pathExists(const Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to) {
    const uint32_t fromFace = findFaceAt(pf, from);
    const uint32_t toFace = findFaceAt(pf, to);
    if (fromFace == UINT32_MAX || toFace == UINT32_MAX) {
        return false;
    }
    return pf.uni.regions[fromFace] == pf.uni.regions[toFace];
}

std::optional<Path> createPath(Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to, const FacePassable &passable,
                               float radius, const LegBlocker &legs) {
    for (int32_t i = 0; i < pf.paths.size(); ++i) {
        AStarPath &path = pf.paths[i];
        if (path.active) {
            continue;
        }

        bool found = findPath(path, pf, from, to, passable, radius, {}, legs);
        if (!found) {
            return std::nullopt;
        }
        path.passable = passable;
        path.legs = legs;
        path.index = i;
        return Path {i};
    }

    return std::nullopt;
}

std::optional<Path> createStraightPath(Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to) {
    if (!pathExists(pf, from, to)) {
        return std::nullopt;
    }
    for (int32_t i = 0; i < pf.paths.size(); ++i) {
        AStarPath &path = pf.paths[i];
        if (path.active) {
            continue;
        }
        path.faces.clear();
        path.pointFaces.clear();
        path.from = from;
        path.to = to;
        path.radius = 0.0f;
        path.current = 0;
        path.straight = true;
        path.generation = pf.generation;
        path.replan = false;
        path.followsFaces = true;
        path.points = {from, to};
        path.pointNext = 1;
        path.passable = {};
        path.legs = {};
        path.active = true;
        path.index = i;
        return Path {i};
    }
    return std::nullopt;
}

bool replanPath(Pathfinder &pf, Path p, const glm::vec3 &from, const FacePassable &keepOff) {
    AStarPath &path = pf.paths[p.index];
    if (path.straight || !path.followsFaces) {
        return true;
    }
    // The search runs on a copy, so a failed one leaves the path as it was.
    AStarPath planned = path;
    if (!findPath(planned, pf, from, path.to, path.passable, path.radius, keepOff, path.legs)) {
        return false;
    }
    path = std::move(planned);
    return true;
}

// Whether the level segment between two points passes through a box across
// the ground plan.
static bool segmentCrossesBox(const glm::vec2 &a, const glm::vec2 &b, const glm::vec2 &min, const glm::vec2 &max) {
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 2; ++axis) {
        const float along = b[axis] - a[axis];
        if (std::fabs(along) < 1e-6f) {
            if (a[axis] < min[axis] || a[axis] > max[axis]) {
                return false;
            }
            continue;
        }
        float t0 = (min[axis] - a[axis]) / along;
        float t1 = (max[axis] - a[axis]) / along;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) {
            return false;
        }
    }
    return true;
}

void markPathsAcross(Pathfinder &pf, const glm::vec2 &min, const glm::vec2 &max) {
    for (AStarPath &path : pf.paths) {
        if (!path.active || path.straight || !path.followsFaces || path.replan || path.points.size() < 2) {
            continue;
        }
        const glm::vec2 reach {path.radius};
        const size_t first = path.pointNext > 0 ? path.pointNext - 1 : 0;
        for (size_t i = first; i + 1 < path.points.size(); ++i) {
            if (segmentCrossesBox(glm::vec2(path.points[i]), glm::vec2(path.points[i + 1]), min - reach, max + reach)) {
                path.replan = true;
                break;
            }
        }
    }
}

void releasePath(Pathfinder &pf, Path path) {
    pf.paths[path.index].active = false;
    pf.paths[path.index].passable = {};
    pf.paths[path.index].legs = {};

    if (isShowPathEnabled()) {
        drawdebug::pushId(path.index);

        drawdebug::pushId("findPath");
        drawdebug::clear();
        drawdebug::popId();

        drawdebug::pushId("funnel");
        drawdebug::clear();
        drawdebug::popId();

        drawdebug::pushId("updatePath");
        drawdebug::clear();
        drawdebug::popId();

        drawdebug::popId();
    }
}

// Explicit points are walked in order and never planned again. A point
// counts as reached from above, whatever its height, and points reached
// together are passed together.
static void updatePathPoints(AStarPath &path, const glm::vec3 &current) {
    const float eps2 = 0.01f;
    while (path.pointNext + 1 < path.points.size() &&
           glm::distance2(glm::vec2(path.points[path.pointNext]), glm::vec2(current)) < eps2) {
        ++path.pointNext;
    }
}

bool updatePath(Pathfinder &pf, Path p, const glm::vec3 &current) {
    AStarPath &path = pf.paths[p.index];

    // A path planned over a walkmesh since put out of place, or marked to be
    // planned again, is planned again from where the walker stands; one walked
    // straight keeps its points.
    if (path.generation != pf.generation || path.replan) {
        path.generation = pf.generation;
        path.replan = false;
        if (!path.straight && path.followsFaces && !findPath(path, pf, current, path.to, path.passable, path.radius, {}, path.legs)) {
            return false;
        }
    }
    if (path.straight) {
        return true;
    }
    if (!path.followsFaces) {
        updatePathPoints(path, current);
        return true;
    }

    // The walker is looked for on the path's faces from the one before where
    // it was last found to the one past the gap of the point it walks to:
    // the faces between are its way there.
    const uint32_t checkFrom = path.current > 0 ? path.current - 1 : 0;
    const uint32_t checkTo = std::min<uint32_t>(path.pointFaces[path.pointNext] + 1,
                                                static_cast<uint32_t>(path.faces.size()) - 1);

    bool isOnPath = false;
    for (uint32_t i = checkFrom; i <= checkTo; ++i) {
        if (standsOnFace(pf.uni, path.faces[i], current)) {
            path.current = i;
            isOnPath = true;
            break;
        }
    }
    if (!isOnPath) {
        // Wandered off the path, recalculate.
        bool foundNewPath = findPath(path, pf, current, path.to, path.passable, path.radius, {}, path.legs);
        if (!foundNewPath) {
            return false;
        }
    }

    // A corner is passed once the walker is within a tenth of a metre of it,
    // or stands on a face past its gap; the end of the path is not passed.
    const float eps2 = 0.01f;
    while (path.pointNext + 1 < path.points.size() &&
           (glm::distance2(path.points[path.pointNext], current) < eps2 ||
            path.current > path.pointFaces[path.pointNext])) {
        ++path.pointNext;
    }

    drawdebug::pushId(p.index);
    drawdebug::pushId("updatePath");
    drawdebug::clear();

    if (isShowPathEnabled()) {
        drawdebug::line(current, path.points[path.pointNext], 0xF0E68CFF, 0.05);
    }

    drawdebug::popId();
    drawdebug::popId();

    return true;
}

glm::vec3 getNextPathPoint(Pathfinder &pf, Path p) {
    AStarPath &path = pf.paths[p.index];
    return path.points[path.pointNext];
}

glm::vec3 getLastPathPoint(Pathfinder &pf, Path p) {
    const AStarPath &path = pf.paths[p.index];
    return path.followsFaces ? path.to : path.points.back();
}

bool isStraightPath(const Pathfinder &pf, Path p) {
    return pf.paths[p.index].straight;
}

const std::vector<glm::vec3> &getPathPoints(const Pathfinder &pf, Path p, uint32_t &next) {
    const AStarPath &path = pf.paths[p.index];
    next = path.pointNext;
    return path.points;
}

bool followsPathPoints(Pathfinder &pf, Path p) {
    return !pf.paths[p.index].followsFaces;
}

std::vector<glm::vec3> pathPoints(Pathfinder &pf, Path p, uint32_t &next) {
    const AStarPath &path = pf.paths[p.index];
    if (!path.followsFaces) {
        next = path.pointNext;
        return path.points;
    }
    // A path that follows its faces gives its points from the one last
    // passed, as the legs already walked are behind the walker.
    next = 1;
    return std::vector<glm::vec3>(path.points.begin() + (path.pointNext - 1), path.points.end());
}

void setPathPoints(Pathfinder &pf, Path p, std::vector<glm::vec3> points, uint32_t next) {
    AStarPath &path = pf.paths[p.index];
    assert(next < points.size());
    path.points = std::move(points);
    path.pointNext = next;
    path.straight = false;
    path.followsFaces = false;
}

// An edge's distance from the centre of a hexagon over a corner's.
static constexpr float kHexLineFactor = 0.866025f;
// A sixth of a turn, clockwise seen from above.
static constexpr float kHexStep = -1.0471976f;
// A path point this near a corner, squared, lies on it.
static constexpr float kHexCornerEps2 = 0.001f;

AvoidanceHex computeAvoidanceHex(const glm::vec3 &center, const glm::vec3 &moverDir, float inradius,
                                 const std::function<float(const glm::vec2 &)> &height) {
    const glm::quat step = glm::angleAxis(kHexStep, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::vec3 offset = glm::normalize(-moverDir) * (inradius / kHexLineFactor);
    AvoidanceHex hex;
    hex.v[0] = center + offset;
    for (int k = 1; k < 6; ++k) {
        offset = step * offset;
        hex.v[k] = center + offset;
        hex.v[k].z = height(glm::vec2(hex.v[k]));
    }
    return hex;
}

bool isPointInAvoidanceHex(const AvoidanceHex &hex, const glm::vec3 &point) {
    for (int e = 0; e < 6; ++e) {
        const glm::vec3 &corner = hex.v[e];
        const glm::vec3 edge = hex.v[(e + 1) % 6] - corner;
        // The right-hand normal of an edge of a clockwise hexagon points in.
        const glm::vec2 inward {edge.y, -edge.x};
        if (glm::dot(inward, glm::vec2(point - corner)) < 0.0f) {
            return false;
        }
    }
    return true;
}

int closestAvoidanceHexCorner(const AvoidanceHex &hex, const glm::vec3 &point) {
    int closest = 0;
    float closestDistance2 = FLT_MAX;
    for (int i = 0; i < 6; ++i) {
        const float distance2 = glm::distance2(hex.v[i], point);
        if (distance2 < closestDistance2) {
            closest = i;
            closestDistance2 = distance2;
        }
    }
    return closest;
}

bool findAvoidanceEntryAndExit(const AvoidanceHex &hex, const std::vector<glm::vec3> &points, uint32_t next,
                               AvoidanceCrossing &entry, AvoidanceCrossing &exit) {
    entry = AvoidanceCrossing();
    exit = AvoidanceCrossing();
    if (points.empty()) {
        return false;
    }

    // Path points are taken on the ground plane.
    const auto flat = [](const glm::vec3 &p) { return glm::vec3(p.x, p.y, 0.0f); };
    const auto nearCorner = [](const glm::vec3 &p, const glm::vec3 &corner) {
        return glm::distance2(p, corner) < kHexCornerEps2;
    };
    int found = 0;
    glm::vec3 a = flat(points[0]);
    for (int k = 1; k < static_cast<int>(points.size()); ++k) {
        const glm::vec3 b = flat(points[k]);
        for (int e = 0; e < 6; ++e) {
            const glm::vec3 &corner = hex.v[e];
            const glm::vec3 &nextCorner = hex.v[(e + 1) % 6];
            const bool a0 = nearCorner(a, corner);
            const bool a1 = nearCorner(a, nextCorner);
            const bool b0 = nearCorner(b, corner);
            const bool b1 = nearCorner(b, nextCorner);
            if (a0 + a1 + b0 + b1 >= 2) {
                // The segment runs along this edge.
                if (found == 0 && (a0 || a1)) {
                    exit = {e, k, a};
                    found = 1;
                }
                if (b0 || b1) {
                    exit = {e, k, b};
                    ++found;
                }
            }
            if (auto along = shape::meetSegments(glm::vec2(a), glm::vec2(b), glm::vec2(corner), glm::vec2(nextCorner))) {
                if (found > 0) {
                    entry = exit;
                }
                exit = {e, k, glm::vec3(glm::vec2(a) + glm::vec2(b - a) * *along, 0.0f)};
                ++found;
                break;
            }
        }
        a = b;
    }
    if (entry.edge == -1) {
        if (exit.edge == -1) {
            return false;
        }
        const int left = next > 0 ? static_cast<int>(next) - 1 : 0;
        entry.edge = closestAvoidanceHexCorner(hex, flat(points[left]));
        entry.index = left;
        entry.point = hex.v[entry.edge];
        ++found;
    }
    return found > 1;
}

std::vector<glm::vec3> findAvoidanceWay(const AvoidanceHex &hex, const AvoidanceCrossing &entry,
                                        const AvoidanceCrossing &exit, bool left) {
    std::vector<glm::vec3> way {entry.point};
    if (entry.edge == exit.edge) {
        way.push_back(exit.point);
        return way;
    }
    if (left) {
        const int last = (exit.edge + 1) % 6;
        int corner = entry.edge;
        way.push_back(hex.v[corner]);
        while (corner != last) {
            corner = (corner + 5) % 6;
            way.push_back(hex.v[corner]);
        }
    } else {
        int corner = (entry.edge + 1) % 6;
        way.push_back(hex.v[corner]);
        while (corner != exit.edge) {
            corner = (corner + 1) % 6;
            way.push_back(hex.v[corner]);
        }
    }
    way.push_back(exit.point);
    return way;
}

void insertAvoidanceWay(std::vector<glm::vec3> &points, uint32_t &next, int entryIndex, int exitIndex,
                        const std::vector<glm::vec3> &way) {
    const int rejoin = exitIndex + (entryIndex == exitIndex ? 1 : 0);
    std::vector<glm::vec3> spliced(points.begin(), points.begin() + entryIndex);
    spliced.insert(spliced.end(), way.begin(), way.end());
    spliced.insert(spliced.end(), points.begin() + rejoin, points.end());
    points = std::move(spliced);
    next = std::min(next, static_cast<uint32_t>(entryIndex));
}

std::optional<float> walkmeshElevationAt(const Uniwalk &uni, const glm::vec2 &xy, float nearZ) {
    std::optional<float> best;
    for (const Uniroom &room : uni.rooms) {
        if (xy.x > room.max.x || xy.y > room.max.y || xy.x < room.min.x || xy.y < room.min.y) {
            continue;
        }
        glm::vec3 top(xy, room.max.z + 1.0f);
        for (uint32_t i = room.begin; i < room.end; ++i) {
            CastResult res = castFace(uni, i, top);
            if (!res.intersects) {
                continue;
            }
            float z = res.intersection.z;
            if (!best || std::fabs(z - nearZ) < std::fabs(*best - nearZ)) {
                best = z;
            }
        }
    }
    return best;
}

// An edge of the walkable ground as a walker at a point meets it: the
// nearest point of the edge across the ground plan, the way from it to the
// walker and how far that is, and whether the walker stands on the walkable
// side of it and within kFloorHeadroom of its height there.
struct EdgeContact {
    glm::vec2 away {0.0f};
    float distance {0.0f};
    bool counts {false};
};

static EdgeContact meetEdge(const Uniwalk &uni, uint32_t edge, const glm::vec3 &position) {
    const Uniface &face = uni.faces[edge / 3];
    const uint32_t j = edge % 3;
    const glm::vec3 &a = uni.vertices[face.vertices[kTriangleEdges[j][0]]];
    const glm::vec3 &b = uni.vertices[face.vertices[kTriangleEdges[j][1]]];
    // The face's third corner lies on the walkable side.
    const glm::vec3 &c = uni.vertices[face.vertices[3 - kTriangleEdges[j][0] - kTriangleEdges[j][1]]];
    const glm::vec2 along {glm::vec2(b) - glm::vec2(a)};
    glm::vec2 inward {-along.y, along.x};
    if (glm::dot(inward, glm::vec2(c) - glm::vec2(a)) < 0.0f) {
        inward = -inward;
    }
    const float length2 = glm::dot(along, along);
    const glm::vec2 fromA {glm::vec2(position) - glm::vec2(a)};
    const float t = length2 > 0.0f ? std::clamp(glm::dot(fromA, along) / length2, 0.0f, 1.0f) : 0.0f;
    const glm::vec3 nearest {a + (b - a) * t};

    EdgeContact contact;
    const glm::vec2 offset {glm::vec2(position) - glm::vec2(nearest)};
    contact.distance = glm::length(offset);
    const float inwardLength = glm::length(inward);
    if (contact.distance > 1e-4f) {
        contact.away = offset / contact.distance;
    } else if (inwardLength > 0.0f) {
        contact.away = inward / inwardLength;
    }
    contact.counts = glm::dot(fromA, inward) >= 0.0f && std::fabs(nearest.z - position.z) <= kFloorHeadroom;
    return contact;
}

// Visits each edge filed in the cells within reach of a point once: in the
// first cell both its box and the reach cover.
template <typename Visit>
static void forEdgesNear(const Uniwalk &uni, const glm::vec3 &position, float reach, const Visit &visit) {
    const EdgeGrid &grid = uni.edgeGrid;
    if (grid.edges.empty()) {
        return;
    }
    const glm::vec2 centre {position};
    const GridSpan span = gridSpan(grid, centre - reach, centre + reach);
    for (int y = span.y0; y <= span.y1; ++y) {
        for (int x = span.x0; x <= span.x1; ++x) {
            const size_t cell = static_cast<size_t>(y) * grid.columns + x;
            for (uint32_t k = grid.cellStarts[cell]; k < grid.cellStarts[cell + 1]; ++k) {
                const uint32_t edge = grid.edges[k];
                glm::vec2 edgeMin, edgeMax;
                edgeBounds(uni, edge, edgeMin, edgeMax);
                const GridSpan own = gridSpan(grid, edgeMin, edgeMax);
                if (x != std::max(own.x0, span.x0) || y != std::max(own.y0, span.y0)) {
                    continue;
                }
                visit(edge);
            }
        }
    }
}

glm::vec2 computeKeepoutForce(const Uniwalk &uni, const glm::vec3 &position, float radius, const glm::vec2 &heading) {
    const float reach = radius + kKeepoutMargin;
    glm::vec2 push {0.0f};
    forEdgesNear(uni, position, reach, [&](uint32_t edge) {
        const EdgeContact contact = meetEdge(uni, edge, position);
        if (contact.counts && contact.distance < reach) {
            push += contact.away * ((reach - contact.distance) / reach);
        }
    });
    // An edge behind the walker does not push it on.
    const float forward = glm::dot(push, heading);
    if (forward > 0.0f) {
        push -= heading * forward;
    }
    const float length = glm::length(push);
    if (length > 1.0f) {
        push /= length;
    }
    return push;
}

glm::vec2 slideAlongEdges(const Uniwalk &uni, const glm::vec3 &position, float radius, glm::vec2 direction) {
    forEdgesNear(uni, position, radius, [&](uint32_t edge) {
        const EdgeContact contact = meetEdge(uni, edge, position);
        if (!contact.counts || contact.distance >= radius) {
            return;
        }
        const float off = glm::dot(direction, contact.away);
        if (off < kSlideOffEdge) {
            direction += contact.away * (kSlideOffEdge - off);
        }
    });
    return direction;
}

bool isClearGround(const Pathfinder &pf, const glm::vec3 &point, float radius) {
    const uint32_t face = findFaceAt(pf, point);
    if (face == UINT32_MAX || (!pf.uni.blocked.empty() && pf.uni.blocked[face])) {
        return false;
    }
    bool clear = true;
    forEdgesNear(pf.uni, point, radius, [&](uint32_t edge) {
        const EdgeContact contact = meetEdge(pf.uni, edge, point);
        if (contact.counts && contact.distance < radius) {
            clear = false;
        }
    });
    return clear;
}

} // namespace game

} // namespace reone
