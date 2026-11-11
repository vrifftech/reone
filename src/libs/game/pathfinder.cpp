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
void uniwalkLoadRoom(struct Uniwalk &uni, graphics::Walkmesh &data, std::set<uint32_t> &walkableMaterial) {
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
    for (uint32_t i = 0; i < data.faces.size(); ++i) {
        uint32_t material = data.materials[i];
        if (!graphics::Walkmesh::hasSurfaceMaterial(walkableMaterial, material)) {
            continue;
        }
        unifiedFaces[i] = static_cast<uint32_t>(uni.faces.size());

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

/// Mark edges that are shared between two faces.
static void markAdjecent(Uniwalk &uni, uint32_t faceA, uint32_t faceB) {
    Uniface &a = uni.faces[faceA];
    Uniface &b = uni.faces[faceB];

    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 3; ++j) {

            uint32_t i0 = a.vertices[kTriangleEdges[i][0]];
            uint32_t i1 = a.vertices[kTriangleEdges[i][1]];

            uint32_t j0 = b.vertices[kTriangleEdges[j][0]];
            uint32_t j1 = b.vertices[kTriangleEdges[j][1]];

            if (i0 != j1 || i1 != j0) {
                continue;
            }

            // i-th edge of face A is the same as j-th edge of face B.
            a.adjecent[i] = faceB;
            b.adjecent[j] = faceA;
        }
    }
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

// Files every edge no other walkable face shares in each cell its box across
// the ground plan covers.
static void buildEdgeGrid(Uniwalk &uni) {
    EdgeGrid &grid = uni.edgeGrid;
    grid = EdgeGrid();
    std::vector<uint32_t> edges;
    glm::vec2 min {FLT_MAX};
    glm::vec2 max {-FLT_MAX};
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            if (uni.faces[i].adjecent[j] != UINT32_MAX) {
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

void uniwalkFinalize(struct Uniwalk &uni) {
    // Deduplicate vertices, so that the same vertex gets the same index in all
    // faces. This way we can compare indices of two vertices instead of
    // calculating distance between them every time.
    const float eps2 = 0.1f * 0.1f;
    std::vector<uint32_t> dedup(uni.vertices.size(), UINT32_MAX);
    for (uint32_t i = 0; i < uni.vertices.size(); ++i) {
        if (dedup[i] != UINT32_MAX) {
            // This vertex, and all following vertices that are close to it are
            // already deduplicated.
            continue;
        }
        for (uint32_t j = i + 1; j < uni.vertices.size(); ++j) {
            if (glm::distance2(uni.vertices[i], uni.vertices[j]) < eps2) {
                dedup[j] = i;
            }
        }
    }
    for (Uniface &face : uni.faces) {
        for (uint32_t &v : face.vertices) {
            if (dedup[v] != UINT32_MAX) {
                v = dedup[v];
            }
        }
    }

    // For each faces, find faces that are adjecent to it.
    for (uint32_t i = 0; i < uni.faces.size(); ++i) {
        for (uint32_t j = i + 1; j < uni.faces.size(); ++j) {
            markAdjecent(uni, i, j);
        }
    }

    // A vertex of an edge no other walkable face shares lies on the edge of
    // the walkable ground.
    uni.boundary.assign(uni.vertices.size(), 0);
    for (const Uniface &face : uni.faces) {
        for (uint32_t j = 0; j < 3; ++j) {
            if (face.adjecent[j] == UINT32_MAX) {
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

    drawDebugUniwalk(uni);
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
            if (room.walkmesh == ref->walkmesh) {
                return ref->face < room.unifiedFaces.size() ? room.unifiedFaces[ref->face] : UINT32_MAX;
            }
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

/// Find a face with the minimum cost and remove it from the open list.
static AStarOpenFace popOpenFace(std::vector<AStarOpenFace> &open) {
    assert(open.size() >= 1);
    if (open.size() == 1) {
        AStarOpenFace face = open[0];
        open.resize(0);
        return face;
    }

    float minCost = FLT_MAX;
    uint32_t minIndex = UINT32_MAX;
    for (uint32_t i = 0; i < open.size(); ++i) {
        if (open[i].cost < minCost) {
            minCost = open[i].cost;
            minIndex = i;
        }
    }

    AStarOpenFace face = open[minIndex];

    // Remove the face from the list.
    uint32_t lastIndex = open.size() - 1;
    if (lastIndex != minIndex) {
        std::swap(open[minIndex], open[lastIndex]);
    }
    open.resize(open.size() - 1);

    return face;
}

static bool findPathAStar(AStarPath &path, AStarContext &ctx, const Uniwalk &uni,
                          uint32_t fromFace, uint32_t toFace, const FacePassable &passable, float radius) {
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
    ctx.open.push_back({toFace, glm::distance(uni.faces[toFace].centroid, goal)});

    // Each face is asked once a search whether the walker may pass over it.
    // The search starts from the face the walk ends on and ends on the face
    // the walk starts from, so neither is asked.
    if (passable) {
        ctx.passes.assign(uni.faces.size(), -1);
    }
    const auto mayPass = [&](uint32_t face) {
        if (!passable || face == fromFace) {
            return true;
        }
        int8_t &passes = ctx.passes[face];
        if (passes < 0) {
            passes = passable(face) ? 1 : 0;
        }
        return passes != 0;
    };

    while (!ctx.open.empty()) {
        // Extract face with least total cost from open list and close it.
        AStarOpenFace current = popOpenFace(ctx.open);
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
                // current face.
                bool foundOpen = false;
                for (AStarOpenFace &open : ctx.open) {
                    if (open.index != adj) {
                        continue;
                    }
                    foundOpen = true;
                    open.cost = cost;
                    break;
                }
                assert(foundOpen);
                ctx.state[adj].parent = current.index;
                ctx.state[adj].travelled = travelled;
            } else {
                // Open the adjecent face.
                ctx.open.push_back({adj, cost});
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

// A path keeps clear of the faces the walker may not pass over, and of gaps
// too narrow for it; when no path does, it is found over every face and gap,
// and the funnel closes the gaps too narrow to a point. Faces \p keepOff turns
// down are kept off either way.
static bool findPath(AStarPath &path, Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to,
                     const FacePassable &passable, float radius, const FacePassable &keepOff = {}) {
    const Uniwalk &uni = pf.uni;
    AStarContext &astar = pf.astar;
    uint32_t fromFace = findFaceAt(pf, from);
    uint32_t toFace = findFaceAt(pf, to);

    if (fromFace == UINT32_MAX || toFace == UINT32_MAX) {
        return false;
    }

    FacePassable first = passable;
    if (keepOff) {
        first = [&passable, &keepOff](uint32_t face) {
            return keepOff(face) && (!passable || passable(face));
        };
    }
    bool found = findPathAStar(path, astar, uni, fromFace, toFace, first, radius);
    if (!found && (passable || radius > 0.0f)) {
        found = findPathAStar(path, astar, uni, fromFace, toFace, keepOff, 0.0f);
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

bool pathExists(const Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to) {
    const uint32_t fromFace = findFaceAt(pf, from);
    const uint32_t toFace = findFaceAt(pf, to);
    if (fromFace == UINT32_MAX || toFace == UINT32_MAX) {
        return false;
    }
    return pf.uni.regions[fromFace] == pf.uni.regions[toFace];
}

std::optional<Path> createPath(Pathfinder &pf, const glm::vec3 &from, const glm::vec3 &to, const FacePassable &passable,
                               float radius) {
    for (int32_t i = 0; i < pf.paths.size(); ++i) {
        AStarPath &path = pf.paths[i];
        if (path.active) {
            continue;
        }

        bool found = findPath(path, pf, from, to, passable, radius);
        if (!found) {
            return std::nullopt;
        }
        path.passable = passable;
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
        path.followsFaces = true;
        path.points = {from, to};
        path.pointNext = 1;
        path.passable = {};
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
    if (!findPath(planned, pf, from, path.to, path.passable, path.radius, keepOff)) {
        return false;
    }
    path = std::move(planned);
    return true;
}

void releasePath(Pathfinder &pf, Path path) {
    pf.paths[path.index].active = false;
    pf.paths[path.index].passable = {};

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
        bool foundNewPath = findPath(path, pf, current, path.to, path.passable, path.radius);
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
    if (findFaceAt(pf, point) == UINT32_MAX) {
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
