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

#include "reone/game/object/areapathing.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <future>
#include <set>

#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/surfaces.h"
#include "reone/graphics/walkmesh.h"
#include "reone/scene/node/walkmesh.h"
#include "reone/system/di/services.h"
#include "reone/system/threadpool.h"

#include "staticwalls.h"

namespace reone {

namespace game {

// Seconds of game time a walkmesh put together away from the area takes to
// be put in place, whatever the frame rate.
static constexpr float kBuildDelay = 0.25f;

// A walkmesh put together away from the area: its rooms' walkmeshes, the
// surfaces walked on and the placeables' blocking faces are taken when it is
// asked for, and the placeables' outlines are taken from those as it is put
// together. Either a worker of the thread pool or the area, whichever comes first,
// puts it together.
struct AreaPathing::Build {
    std::vector<std::shared_ptr<graphics::Walkmesh>> rooms;
    std::set<uint32_t> walkable;
    std::vector<ObstacleSource> sources;
    bool stacked {false};
    /** The placeables as they stood when their blocking faces were taken. */
    std::vector<Pose> poses;
    std::atomic_bool claimed {false};
    std::promise<Uniwalk> promise;
    std::future<Uniwalk> result;
    /** Seconds of game time until the walkmesh is put in place. */
    float wait {0.0f};

    Uniwalk build() const {
        Uniwalk uni;
        for (const auto &walkmesh : rooms) {
            uniwalkLoadRoom(uni, *walkmesh, walkable);
        }
        uniwalkFinalize(uni, obstacles(sources, stacked));
        return uni;
    }
};

AreaPathing::AreaPathing(Area &area, Pathfinder &pathfinder) :
    _area(area),
    _pathfinder(pathfinder) {
}

// A walkmesh still being put together away from the area is dropped with it.
AreaPathing::~AreaPathing() {
    if (_build) {
        _build->claimed = true;
    }
}

namespace {

// A door and the box round its walkmeshes, open and closed, across the ground
// plan.
struct DoorBox {
    uint32_t id;
    glm::vec2 min;
    glm::vec2 max;
};

} // namespace

static std::vector<DoorBox> doorBoxes(const ObjectList &objects) {
    std::vector<DoorBox> doors;
    for (const auto &object : objects) {
        const auto &door = static_cast<const Door &>(*object);
        DoorBox box {door.id(), glm::vec2(FLT_MAX), glm::vec2(-FLT_MAX)};
        for (const auto &node : {door.walkmeshOpen1(), door.walkmeshOpen2(), door.walkmeshClosed()}) {
            if (!node) {
                continue;
            }
            for (const glm::vec3 &vertex : node->walkmesh().vertices) {
                const glm::vec2 point {node->absoluteTransform() * glm::vec4(vertex, 1.0f)};
                box.min = glm::min(box.min, point);
                box.max = glm::max(box.max, point);
            }
        }
        if (box.min.x <= box.max.x) {
            doors.push_back(box);
        }
    }
    return doors;
}

// Each door, and each placeable not yet cut out of the walkmesh, is tested
// against a face only near the box round its walkmeshes, and as it stands
// when the face is asked about. The blocking surfaces, the walker's reach and
// the heights it meets walls across are taken once.
FacePassable AreaPathing::facesClearOfStaticWalls(const Creature &walker) const {
    auto doors = doorBoxes(_area.getObjectsByType(ObjectType::Door));
    const auto blocking = std::make_shared<const std::set<uint32_t>>(blockingSurfaces(_area.services().game.surfaces));
    const float radius = walker.personalSpace();
    // The band is the same distance under and over any floor height.
    const auto band = _area.staticWallBand(walker, 0.0f, 0.0f);
    return [this, doors = std::move(doors), blocking, radius, band](uint32_t face) {
        if (face >= _pathfinder.uni.faces.size()) {
            return true;
        }
        const glm::vec3 &centre = _pathfinder.uni.faces[face].centroid;
        const auto near = [&centre, radius](const glm::vec2 &min, const glm::vec2 &max) {
            return centre.x >= min.x - radius && centre.x <= max.x + radius &&
                   centre.y >= min.y - radius && centre.y <= max.y + radius;
        };
        for (const DoorBox &box : doors) {
            if (!near(box.min, box.max)) {
                continue;
            }
            auto door = _area.game().getObjectById<Door>(box.id);
            if (door && hasDoorWallWithin(*door, *blocking, centre, centre, radius, centre.z + band.low, centre.z + band.high)) {
                return false;
            }
        }
        for (const Pose &pose : uncutPlaceables()) {
            if (!near(pose.min, pose.max)) {
                continue;
            }
            auto placeable = _area.game().getObjectById<Placeable>(pose.id);
            if (placeable && hasBlockingFaceWithin(placeable->walkmesh(), *blocking, centre, centre, radius, centre.z + band.low, centre.z + band.high)) {
                return false;
            }
        }
        return true;
    };
}

// How far a point lies from a triangle across the ground plan: nothing when
// inside it.
static float distanceToTriangle(const glm::vec2 &point, const std::vector<glm::vec2> &triangle) {
    bool inside = true;
    float nearest = FLT_MAX;
    for (size_t i = 0; i < 3; ++i) {
        const glm::vec2 &a = triangle[i];
        const glm::vec2 &b = triangle[(i + 1) % 3];
        const glm::vec2 along {b - a};
        if (along.x * (point.y - a.y) - along.y * (point.x - a.x) < 0.0f) {
            inside = false;
        }
        const float length2 = glm::dot(along, along);
        const float t = length2 > 0.0f ? std::clamp(glm::dot(point - a, along) / length2, 0.0f, 1.0f) : 0.0f;
        nearest = std::min(nearest, glm::distance(point, a + along * t));
    }
    return inside ? 0.0f : nearest;
}

// The blocking faces of each placeable's walkmesh, in the walkmesh's own
// coordinates, and where it stands: what the placeables' outlines are taken
// from, away from the area.
std::vector<AreaPathing::ObstacleSource> AreaPathing::obstacleSources() const {
    const auto blocking = blockingSurfaces(_area.services().game.surfaces);
    std::vector<ObstacleSource> sources;
    for (const auto &object : _area.getObjectsByType(ObjectType::Placeable)) {
        const auto node = static_cast<const Placeable &>(*object).walkmesh();
        if (!node || !node->isEnabled()) {
            continue;
        }
        const graphics::Walkmesh &walkmesh = node->walkmesh();
        ObstacleSource source;
        source.transform = node->absoluteTransform();
        for (size_t i = 0; i < walkmesh.faces.size(); ++i) {
            if (!graphics::Walkmesh::hasSurfaceMaterial(blocking, walkmesh.materials[i])) {
                continue;
            }
            for (uint32_t index : walkmesh.faces[i].indices) {
                source.corners.push_back(walkmesh.vertices[index]);
            }
        }
        if (!source.corners.empty()) {
            sources.push_back(std::move(source));
        }
    }
    return sources;
}

std::vector<Obstacle> AreaPathing::obstacles(const std::vector<ObstacleSource> &sources, bool stacked) {
    // A blocking face with less ground than this under it, in square metres,
    // stands upright: a strip this far either side of its longest edge across
    // the ground plan is cut instead, unless it stands at the side of a flat
    // face of the same placeable, within this far of one.
    static constexpr float kUprightArea = 1e-4f;
    static constexpr float kUprightStripHalfWidth = 0.05f;
    static constexpr float kUprightBesideFlat = 0.01f;
    // Where floors lie over others, a walker standing on a floor meets a
    // placeable's face from kFloorHeadroom under the floor to this far over
    // it, as staticWallBand gives for a walker 2 metres tall.
    static constexpr float kObstacleReachOver = 2.1f;
    // Corners this near, in metres, are the same: a face whose corners are
    // the same as another's across the ground plan cuts nothing more.
    static constexpr float kSameCorner = 1e-4f;
    std::vector<Obstacle> obstacles;
    for (const ObstacleSource &source : sources) {
        // Each blocking face of the placeable's walkmesh cuts its own
        // outline: a flat one its triangle across the ground plan, an upright
        // one a strip along it.
        const glm::mat4 &transform = source.transform;
        struct Upright {
            glm::vec2 p, q;
            float low, high;
        };
        std::vector<Upright> uprights;
        std::vector<std::vector<glm::vec2>> flats;
        const auto band = [&](Obstacle &obstacle, float low, float high) {
            if (stacked) {
                obstacle.floorMin = low - kObstacleReachOver;
                obstacle.floorMax = high + graphics::Walkmesh::kFloorHeadroom;
            }
        };
        const auto sameCorners = [](const std::vector<glm::vec2> &a, const std::vector<glm::vec2> &b) {
            return std::all_of(a.begin(), a.end(), [&b](const glm::vec2 &p) {
                return std::any_of(b.begin(), b.end(), [&p](const glm::vec2 &q) { return glm::distance(p, q) < kSameCorner; });
            });
        };
        for (size_t i = 0; i + 2 < source.corners.size(); i += 3) {
            glm::vec3 corners[3];
            for (int k = 0; k < 3; ++k) {
                corners[k] = glm::vec3(transform * glm::vec4(source.corners[i + k], 1.0f));
            }
            const float low = std::min({corners[0].z, corners[1].z, corners[2].z});
            const float high = std::max({corners[0].z, corners[1].z, corners[2].z});
            const glm::vec2 a {corners[0]};
            const glm::vec2 b {corners[1]};
            const glm::vec2 c {corners[2]};
            const float doubleArea = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(doubleArea) <= 2.0f * kUprightArea) {
                glm::vec2 p {a};
                glm::vec2 q {b};
                for (const auto &[from, to] : {std::pair {b, c}, std::pair {c, a}}) {
                    if (glm::distance2(from, to) > glm::distance2(p, q)) {
                        p = from;
                        q = to;
                    }
                }
                if (glm::distance(p, q) > kUprightStripHalfWidth) {
                    uprights.push_back({p, q, low, high});
                }
                continue;
            }
            std::vector<glm::vec2> triangle = doubleArea > 0.0f ? std::vector<glm::vec2> {a, b, c} : std::vector<glm::vec2> {a, c, b};
            if (std::any_of(flats.begin(), flats.end(), [&](const auto &flat) { return sameCorners(flat, triangle); })) {
                continue;
            }
            flats.push_back(triangle);
            Obstacle obstacle;
            obstacle.outline = std::move(triangle);
            band(obstacle, low, high);
            obstacles.push_back(std::move(obstacle));
        }
        for (const Upright &upright : uprights) {
            const glm::vec2 middle {(upright.p + upright.q) * 0.5f};
            bool besideFlat = false;
            for (const auto &flat : flats) {
                besideFlat = besideFlat || distanceToTriangle(middle, flat) <= kUprightBesideFlat;
            }
            if (besideFlat) {
                continue;
            }
            const float length = glm::distance(upright.p, upright.q);
            const glm::vec2 dir {(upright.q - upright.p) / length};
            const glm::vec2 left {glm::vec2(-dir.y, dir.x) * kUprightStripHalfWidth};
            Obstacle obstacle;
            obstacle.outline = {upright.p - left, upright.q - left, upright.q + left, upright.p + left};
            band(obstacle, upright.low, upright.high);
            obstacles.push_back(std::move(obstacle));
        }
    }
    return obstacles;
}

// The box round a walkmesh, as it stands, across the ground plan.
static void groundBox(const scene::WalkmeshSceneNode &node, glm::vec2 &min, glm::vec2 &max) {
    min = glm::vec2(FLT_MAX);
    max = glm::vec2(-FLT_MAX);
    const glm::mat4 &transform = node.absoluteTransform();
    for (const glm::vec3 &vertex : node.walkmesh().vertices) {
        const glm::vec2 point {transform * glm::vec4(vertex, 1.0f)};
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
}

std::vector<AreaPathing::Pose> AreaPathing::poses() const {
    std::vector<Pose> poses;
    for (const auto &object : _area.getObjectsByType(ObjectType::Placeable)) {
        const auto node = static_cast<const Placeable &>(*object).walkmesh();
        if (!node || !node->isEnabled()) {
            continue;
        }
        Pose pose;
        pose.id = object->id();
        pose.transform = node->absoluteTransform();
        groundBox(*node, pose.min, pose.max);
        poses.push_back(pose);
    }
    std::sort(poses.begin(), poses.end(), [](const Pose &a, const Pose &b) { return a.id < b.id; });
    return poses;
}

bool AreaPathing::hasPose(const std::vector<Pose> &poses, const Pose &pose) {
    auto it = std::lower_bound(poses.begin(), poses.end(), pose.id, [](const Pose &p, uint32_t id) { return p.id < id; });
    return it != poses.end() && *it == pose;
}

const std::vector<AreaPathing::Pose> &AreaPathing::uncutPlaceables() const {
    if (!_uncutStale) {
        return _uncut;
    }
    _uncutStale = false;
    _uncut.clear();
    // A pathfinder not put together from rooms' walkmeshes has no placeables
    // cut out of it, nor tests them.
    if (_roomWalkmeshes.empty()) {
        return _uncut;
    }
    for (const Pose &pose : poses()) {
        if (!hasPose(_cut, pose) && pose.min.x <= pose.max.x) {
            _uncut.push_back(pose);
        }
    }
    return _uncut;
}

void AreaPathing::markObstaclesStale() {
    _stale = true;
    _uncutStale = true;
}

void AreaPathing::replanAcross(const Pose &pose) {
    if (pose.min.x <= pose.max.x) {
        markPathsAcross(_pathfinder, pose.min, pose.max);
    }
}

// Paths keep their faces where those are left whole (see replaceUniwalk);
// those near a placeable that came, went or moved between the two walkmeshes
// are planned again, round it or over the ground it left.
void AreaPathing::putInPlace(Uniwalk uni, std::vector<Pose> cut) {
    replaceUniwalk(_pathfinder, std::move(uni));
    for (const Pose &pose : _cut) {
        if (!hasPose(cut, pose)) {
            replanAcross(pose);
        }
    }
    for (const Pose &pose : cut) {
        if (!hasPose(_cut, pose)) {
            replanAcross(pose);
        }
    }
    _cut = std::move(cut);
    // A placeable still not cut out as it stands, having moved since the
    // paths across it were marked, has them marked again where it stands.
    // Each is marked once more at most until the next walkmesh is in place.
    auto marked = std::move(_marked);
    _marked.clear();
    for (const Pose &pose : _seen) {
        if (hasPose(_cut, pose)) {
            continue;
        }
        const auto it = marked.find(pose.id);
        if (it == marked.end() || it->second != pose.transform) {
            replanAcross(pose);
        }
        _marked[pose.id] = pose.transform;
    }
    _uncutStale = true;
}

void AreaPathing::rebuild() {
    _stale = false;
    if (_roomWalkmeshes.empty()) {
        return;
    }
    // A walkmesh being put together away from the area is dropped.
    if (_build) {
        _build->claimed = true;
        _build.reset();
    }
    Build build;
    build.rooms = _roomWalkmeshes;
    build.walkable = _area.services().game.surfaces.getWalkableSurfaces();
    build.sources = obstacleSources();
    build.stacked = _area.hasStackedFloors();
    _seen = poses();
    putInPlace(build.build(), _seen);
}

// When a placeable comes, goes or moves, the walkmesh is put together again by
// a worker of the thread pool, and put in place kBuildDelay of game time
// later. Should the worker not be done by then, the area waits for it; should
// it not have started, the area puts the walkmesh together itself.
// Placeables that come, go or move meanwhile are taken into the next
// walkmesh, begun once this one is in place, so a run of changes holds none
// of them back for longer than two such waits. Until then, paths are planned
// round a placeable not yet cut out as it stands (see
// facesClearOfStaticWalls), and the paths whose way ahead it came or moved
// across are planned again at once: once until the next walkmesh is in place,
// however often it moves, and then where it stands, if it is still not cut
// out there. A placeable that went and came back where it stood,
// as a placeable does when the area is shown again, changes nothing.
void AreaPathing::update(float dt) {
    if (_stale) {
        _stale = false;
        auto now = poses();
        if (!_roomWalkmeshes.empty() && now != _seen) {
            for (const Pose &pose : now) {
                if (!hasPose(_seen, pose) && _marked.emplace(pose.id, pose.transform).second) {
                    replanAcross(pose);
                }
            }
            _seen = std::move(now);
        }
    }
    if (_build) {
        _build->wait -= dt;
        if (_build->wait > 0.0f) {
            return;
        }
        auto build = std::move(_build);
        Uniwalk uni = build->claimed.exchange(true) ? build->result.get() : build->build();
        putInPlace(std::move(uni), std::move(build->poses));
    }
    if (_roomWalkmeshes.empty() || _seen == _cut) {
        return;
    }
    auto build = std::make_shared<Build>();
    build->rooms = _roomWalkmeshes;
    build->walkable = _area.services().game.surfaces.getWalkableSurfaces();
    build->sources = obstacleSources();
    build->stacked = _area.hasStackedFloors();
    build->poses = _seen;
    build->result = build->promise.get_future();
    build->wait = kBuildDelay;
    _area.services().system.threadPool.enqueue([build](const std::atomic_bool &canceled) {
        if (canceled || build->claimed.exchange(true)) {
            return;
        }
        try {
            build->promise.set_value(build->build());
        } catch (...) {
            build->promise.set_exception(std::current_exception());
        }
    });
    _build = std::move(build);
}

} // namespace game

} // namespace reone
