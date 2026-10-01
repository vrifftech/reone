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

#include "reone/game/action/movetoobject.h"

#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/module.h"
#include "reone/game/savedruntime.h"

#include <algorithm>
#include <cmath>
#include "reone/game/action/movetolocation.h"
#include "reone/game/location.h"
#include "reone/game/action/movetopoint.h"
#include "reone/game/action/moveawayfromobject.h"
#include "reone/game/action/moveawayfromlocation.h"
#include "reone/game/object/area.h"
#include "reone/game/action/follow.h"
#include "reone/game/action/followleader.h"
#include "reone/game/party.h"
#include "reone/game/action/followowner.h"
#include "reone/game/action/jumptolocation.h"
#include "commonactions.h"
#include "reone/game/action/changefacing.h"
#include "reone/game/action/jumptoobject.h"
#include "reone/game/object/door.h"
#include "reone/game/action/randomwalk.h"
#include "reone/game/action/wait.h"

namespace reone {

namespace game {

// A jump lands on the first safe spot within this distance of its destination.
static constexpr float kJumpSearchRadius = 20.0f;
// A forced move that runs out of time puts the mover down on its destination
// only when that spot itself is safe.
static constexpr float kForcedPointJumpRadius = 1.0f;

// Facing along a direction; a direction too short to have one faces along +x.
static float facingAlong(const glm::vec3 &direction) {
    static constexpr float kShortestDirection = 1e-9f;
    if (glm::length(direction) < kShortestDirection) return facingAlong(glm::vec3(1.0f, 0.0f, 0.0f));
    return -std::atan2(direction.x, direction.y);
}

// A jump onto a waypoint takes its facing. A jump to a door lands behind the
// door's closed-state use point nearest its back, twice the jumper's creature
// personal space further back, facing away from the door. A jump onto anything
// else lands on it facing along +x. The trail keeps the facing the jumper had
// before it jumped.
static void jumpToObject(Creature &jumper, Object &target, bool clearLine, Game &game) {
    glm::vec3 destination(target.position());
    float facing;
    if (target.type() == ObjectType::Waypoint) {
        facing = target.getFacing();
    } else if (auto *door = dyn_cast<Door>(&target)) {
        const float doorFacing = door->getFacing();
        const glm::vec3 doorForward(-std::sin(doorFacing), std::cos(doorFacing), 0.0f);
        const float spacing = 2.0f * jumper.creaturePersonalSpace();
        destination = door->nearestActionPoint(door->position() - doorForward, true) - spacing * doorForward;
        facing = facingAlong(destination - door->position());
    } else {
        facing = facingAlong(glm::vec3(1.0f, 0.0f, 0.0f));
    }
    jumpToPositionFacing(jumper, destination, facing, jumper.getFacing(), kJumpSearchRadius, clearLine, game);
}

MoveToObjectAction::MoveToObjectAction(
    Game &game, ServicesView &services, std::shared_ptr<Object> moveTo,
    bool run, float range, bool force, float timeout, bool pointPath, std::optional<float> checkRange,
    bool closeToUseRange) :
    Action(game, services, ActionType::MoveToObject),
    _moveTo(std::move(moveTo)),
    _run(run),
    _range(range),
    _force(force),
    _pointPath(pointPath),
    _timeout(timeout),
    _checkRange(checkRange),
    _closeToUseRange(closeToUseRange) {
    requireRuntimeObject(_moveTo);
    if (_moveTo) {
        _forcedState.destination = _moveTo->position();
    }
}

MoveToObjectAction::MoveToObjectAction(
    Game &game, ServicesView &services, std::shared_ptr<Object> moveTo,
    bool run, float range, float timeout, ForcedState forcedState, bool force) :
    Action(game, services, ActionType::MoveToObject),
    _moveTo(std::move(moveTo)),
    _run(run),
    _range(range),
    _force(force),
    _pointPath(true),
    _timeout(timeout),
    _forcedState(std::move(forcedState)) {
    requireRuntimeObject(_moveTo);
}

// A move order ends a push or leap carrying the creature.
void MoveToObjectAction::onQueued(Object &actor) {
    if (auto *creature = dyn_cast<Creature>(&actor)) creature->endForcedMove();
}

void MoveToObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    // Conversations owned by doors/placeables can queue creature actions on
    // those objects. Such actors have nothing to move, so discard safely.
    auto creatureActor = _game.getObjectById<Creature>(actor.id());
    if (!creatureActor) {
        complete();
        return;
    }
    // Moving to an object releases the mover's orientation lock.
    creatureActor->setOrientationLock(script::kObjectInvalid);

    // A door or placeable is walked to at the mover's use point for it. The
    // move sets out for the object with its range, takes the use point and
    // range the first time either differs, and keeps them from then on.
    const bool toUsePoint = _moveTo &&
                            (_moveTo->type() == ObjectType::Door || _moveTo->type() == ObjectType::Placeable);
    if (toUsePoint) {
        if (!_approach) _approach = Creature::UseApproach {_moveTo->id(), {_moveTo->position(), _range}};
        _approach->follow(creatureActor->useRange(*_moveTo));
    }
    auto destination = [&]() {
        return toUsePoint ? _approach->use.point : _moveTo->position();
    };
    const float closeTo = closingRange();

    if (_force && !_forcedState.active) {
        _forcedState.active = true;
        if (_moveTo) {
            _forcedState.destination = destination();
        }
        if (auto module = _game.module(); module && module->area()) {
            _forcedState.areaId = module->area()->id();
        }
        _forcedState.expiryMilliseconds = _game.worldTimeMilliseconds() +
            static_cast<uint64_t>(
                std::llround(std::max(0.0f, _timeout) * 1000.0f));
    }

    auto dest = _moveTo ? destination() : _forcedState.destination;

    if (_force && _forcedState.active) {
        bool expired =
            _game.worldTimeMilliseconds() >= _forcedState.expiryMilliseconds;
        if (expired) {
            // Out of time, the mover jumps to the object as a jump to it does.
            jumpToObject(*creatureActor, *_moveTo, true, _game);
            complete();
            return;
        }
    }

    bool reached = creatureActor->navigateTo(dest, _run, closeTo, dt, _moveTo.get());
    if (!reached) {
        return;
    }
    if (_checkRange && _moveTo) {
        const float useRange = creatureActor->useRange(*_moveTo).range;
        if (!creatureActor->isInUseRange(*_moveTo, std::max(0.0f, *_checkRange - useRange))) {
            if (!_moveTo->spatialArea() && !isa<Creature>(*_moveTo)) {
                complete();
                return;
            }
            // Short of the check, the mover sets out afresh for the current
            // use point with the move's range.
            if (toUsePoint) {
                _approach = Creature::UseApproach {_moveTo->id(), creatureActor->useRange(*_moveTo)};
                _approach->use.range = _range;
            }
            return;
        }
    }
    complete();
}

float MoveToObjectAction::closingRange() const {
    return _closeToUseRange && _approach ? _approach->use.range : _range;
}

std::optional<SavedActionRecord> MoveToObjectAction::saveFacingState() const {
    // ActionId 17 is the ranged move-to-object check. Forced movement
    // carries additional path/timeout semantics which are not this record.
    if (!_moveTo || !std::isfinite(_range)) {
        return std::nullopt;
    }

    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    if (usesPointPath()) {
        if ((_force && (!std::isfinite(_timeout) || _timeout < 0.0f)) ||
            (!_force && !_pointPath && _timeout >= 0.0f)) {
            return std::nullopt;
        }
        auto destination = _forcedState.active ? _forcedState.destination : _moveTo->position();
        uint32_t areaId = _forcedState.areaId;
        if (areaId == kSavedRuntimeInvalidObjectId) {
            if (auto module = _game.module(); module && module->area()) {
                areaId = module->area()->id();
            }
        }
        if (areaId == kSavedRuntimeInvalidObjectId) {
            return std::nullopt;
        }
        // Split the absolute deadline into the day/time pair at the
        // serialization boundary.
        const uint64_t millisecondsPerDay = _game.millisecondsPerWorldDay();
        const uint32_t expiryDay = _force && _forcedState.active
            ? static_cast<uint32_t>(_forcedState.expiryMilliseconds / millisecondsPerDay)
            : 0;
        const uint32_t expiryTime = _force && _forcedState.active
            ? static_cast<uint32_t>(_forcedState.expiryMilliseconds % millisecondsPerDay)
            : 0;
        int32_t flags = (_run ? 1 : 0) | (_force && !_forcedState.active ? 4 : 0);
        result.actionId = 1;
        result.declaredParameterCount = 13;
        result.parameters = {
            {2, destination.x}, {2, destination.y}, {2, destination.z},
            {3, SavedObjectReference::fromRuntimeId(areaId)}, {3, SavedObjectReference::fromRuntimeId(_moveTo->id())},
            {1, flags}, {2, closingRange()}, {1, int32_t {0}},
            {2, _force && !_forcedState.active ? _timeout : 0.0f},
            {2, _forcedState.offset.x}, {2, _forcedState.offset.y},
            {1, static_cast<int32_t>(expiryDay)},
            {1, static_cast<int32_t>(expiryTime)},
        };
        return result;
    }

    result.actionId = 17;
    result.declaredParameterCount = 5;
    result.parameters = {
        SavedActionParameter {
            static_cast<uint32_t>(SavedActionParameterType::Object),
            SavedObjectReference::fromRuntimeId(_moveTo->id())},
        SavedActionParameter {
            static_cast<uint32_t>(SavedActionParameterType::Integer),
            static_cast<int32_t>(_run ? 1 : 0)},
        SavedActionParameter {
            static_cast<uint32_t>(SavedActionParameterType::Float), _range},
        SavedActionParameter {
            static_cast<uint32_t>(SavedActionParameterType::Float), _checkRange.value_or(_range)},
        SavedActionParameter {
            static_cast<uint32_t>(SavedActionParameterType::Integer), int32_t {1}},
    };
    return result;
}

// A move order lets go of the orientation lock and ends a push or leap
// carrying the creature.
void MoveToLocationAction::onQueued(Object &actor) {
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        creature->setOrientationLock(script::kObjectInvalid);
        creature->endForcedMove();
    }
}

void MoveToLocationAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (!_destination) {
        complete();
        return;
    }
    auto creatureActor = _game.getObjectById<Creature>(actor.id());
    if (!creatureActor) {
        complete();
        return;
    }
    glm::vec3 destination(_destination->position());

    if (_force && !_forcedState.active) {
        _forcedState.active = true;
        if (auto module = _game.module(); module && module->area()) {
            _forcedState.areaId = module->area()->id();
        }
        _forcedState.expiryMilliseconds = _game.worldTimeMilliseconds() +
            static_cast<uint64_t>(
                std::llround(std::max(0.0f, _timeout) * 1000.0f));
    }

    if (_force && _forcedState.active) {
        bool expired =
            _game.worldTimeMilliseconds() >= _forcedState.expiryMilliseconds;
        if (expired) {
            // Out of time, the mover is put down on its destination, facing
            // along +x, if that spot is safe and still in the area the move
            // began in; otherwise it stays where it is.
            if (auto module = _game.module(); module && module->area() &&
                module->area()->id() == _forcedState.areaId) {
                const float facing = facingAlong(glm::vec3(1.0f, 0.0f, 0.0f));
                jumpToPositionFacing(*creatureActor, destination, facing, facing, kForcedPointJumpRadius, true, _game);
            }
            complete();
            return;
        }
    }

    bool reached = creatureActor->navigateTo(destination, _run, _closeRange, dt, nullptr, _straight);
    if (reached) {
        complete();
    }
}

std::optional<SavedActionRecord> MoveToLocationAction::saveFacingState() const {
    if (!_destination || !std::isfinite(_destination->position().x) ||
        !std::isfinite(_destination->position().y) ||
        !std::isfinite(_destination->position().z) ||
        (_force && (!std::isfinite(_timeout) ||
                    (!_forcedState.active && _timeout <= 0.0f))) ||
        (_force && _forcedState.active &&
         _forcedState.expiryMilliseconds == 0)) {
        return std::nullopt;
    }

    uint32_t areaId = _forcedState.areaId;
    if (areaId == kSavedRuntimeInvalidObjectId) {
        if (auto module = _game.module(); module && module->area()) {
            areaId = module->area()->id();
        }
    }
    if (areaId == kSavedRuntimeInvalidObjectId) {
        return std::nullopt;
    }

    // Split the absolute deadline into the day/time pair at the serialization
    // boundary. A zero absolute deadline is rejected above, so an armed forced
    // move never serializes as the unarmed (0, 0) encoding.
    const bool forcedActive = _force && _forcedState.active;
    const uint64_t millisecondsPerDay = _game.millisecondsPerWorldDay();
    const uint32_t expiryDay = forcedActive
        ? static_cast<uint32_t>(_forcedState.expiryMilliseconds / millisecondsPerDay)
        : 0;
    const uint32_t expiryTime = forcedActive
        ? static_cast<uint32_t>(_forcedState.expiryMilliseconds % millisecondsPerDay)
        : 0;

    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 1;
    result.declaredParameterCount = 13;
    int32_t flags = (_run ? 1 : 0) |
                    (_force && !_forcedState.active ? 4 : 0) |
                    (_straight ? 8 : 0);
    result.parameters = {
        {2, _destination->position().x},
        {2, _destination->position().y},
        {2, _destination->position().z},
        {3, SavedObjectReference::fromRuntimeId(areaId)},
        {3, SavedObjectReference::fromRuntimeId(kSavedRuntimeInvalidObjectId)},
        {1, flags},
        {2, _closeRange},
        {1, int32_t {0}},
        {2, _force && !_forcedState.active ? _timeout : 0.0f},
        {2, 0.0f},
        {2, 0.0f},
        {1, static_cast<int32_t>(expiryDay)},
        {1, static_cast<int32_t>(expiryTime)},
    };
    return result;
}

void MoveToPointAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto creatureActor = _game.getObjectById<Creature>(actor.id());
    if (!creatureActor) {
        complete();
        return;
    }

    bool reached = creatureActor->navigateTo(_point, _run, _range, dt);
    if (reached) {
        complete();
    }
}

// The walk is kept as an ordinary walk to its point in the current area,
// ending as near the point as it does.
std::optional<SavedActionRecord> MoveToPointAction::saveFacingState() const {
    auto module = _game.module();
    if (!module || !module->area()) {
        return std::nullopt;
    }
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 1;
    result.declaredParameterCount = 13;
    result.parameters = {
        {2, _point.x}, {2, _point.y}, {2, _point.z},
        {3, SavedObjectReference::fromRuntimeId(module->area()->id())},
        {3, SavedObjectReference::fromRuntimeId(kSavedRuntimeInvalidObjectId)},
        {1, int32_t {_run ? 1 : 0}}, {2, _range}, {1, int32_t {0}}, {2, 0.0f},
        {2, 0.0f}, {2, 0.0f}, {1, int32_t {0}}, {1, int32_t {0}},
    };
    return result;
}

// A creature moving away pauses this long before each leg, less when the
// player controls it.
static constexpr float kMoveAwayLegPause = 0.3f;
static constexpr float kControlledMoveAwayLegPause = 0.1f;

// Sends the creature on its way to the point that takes it the range away from
// a threat, ahead of the action that sent it: a short pause, then a walk in a
// straight line onto the point. The action runs again once the creature gets
// there.
static void moveAwayFrom(Game &game, const Action &action, Creature &creature, const glm::vec3 &threat, float range, bool run) {
    const glm::vec3 point = game.module()->area()->computeAwayPoint(creature, threat, range);
    auto leg = game.newAction<MoveToLocationAction>(std::make_shared<Location>(point, 0.0f), run, false, -1.0f, 0.0f, true);
    creature.addActionBefore(action, leg);
    const bool controlled = game.party().getLeader().get() == &creature;
    creature.addActionBefore(*leg, game.newAction<WaitAction>(controlled ? kControlledMoveAwayLegPause : kMoveAwayLegPause));
}

void MoveAwayFromObject::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto &creature = static_cast<Creature &>(actor);
    // A creature that is dead, or down at no vitality, gives up; one is done
    // when the object has left the area, when it is out of range of it, or
    // when it has set off as often as it may.
    if (creature.isDead() || creature.isTemporarilyDead() ||
        !_game.module()->area()->isObjectResident(*_fleeFrom) ||
        creature.getSquareDistanceTo(*_fleeFrom) > _moveAwayRange * _moveAwayRange ||
        _attemptsLeft <= 0) {
        complete();
        return;
    }
    --_attemptsLeft;
    moveAwayFrom(_game, *this, creature, _fleeFrom->position(), _moveAwayRange, _run);
}

void MoveAwayFromLocation::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto &creature = static_cast<Creature &>(actor);
    // A creature that is dead, or down at no vitality, gives up; one out of
    // range of the location is done.
    if (creature.isDead() || creature.isTemporarilyDead() ||
        creature.getSquareDistanceTo(_moveAwayFrom->position()) > _moveAwayRange * _moveAwayRange) {
        complete();
        return;
    }
    moveAwayFrom(_game, *this, creature, _moveAwayFrom->position(), _moveAwayRange, _run);
}

std::optional<SavedActionRecord> MoveAwayFromObject::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 3;
    result.declaredParameterCount = 4;
    result.parameters = {
        {3, SavedObjectReference::fromRuntimeId(_fleeFrom->id())},
        {1, int32_t {_run ? 1 : 0}},
        {2, _moveAwayRange},
        {1, int32_t {_attemptsLeft}},
    };
    return result;
}

std::optional<SavedActionRecord> MoveAwayFromLocation::saveFacingState() const {
    // The record also carries a count of legs, which this action never uses.
    static constexpr int32_t kUnusedAttempts = 10;
    const glm::vec3 &position = _moveAwayFrom->position();
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 44;
    result.declaredParameterCount = 6;
    result.parameters = {
        {2, position.x},
        {2, position.y},
        {2, position.z},
        {1, int32_t {_run ? 1 : 0}},
        {2, _moveAwayRange},
        {1, kUnusedAttempts},
    };
    return result;
}

void FollowAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto creatureActor = _game.getObjectById<Creature>(actor.id());
    if (!creatureActor) {
        complete();
        return;
    }

    auto dest = _follow->position();
    float distance2 = creatureActor->getSquareDistanceTo(glm::vec2(dest));
    bool run = distance2 > kDistanceWalk * kDistanceWalk;

    if (creatureActor->navigateTo(dest, run, _followDistance, dt, _follow.get())) {
        complete();
    }
}

// A follow is kept as the one followed and the point the follow distance in
// front of it, which is also where the follower last headed.
std::optional<SavedActionRecord> FollowAction::saveFacingState() const {
    const float facing = _follow->getFacing();
    const glm::vec3 point = _follow->position() + _followDistance * glm::vec3(-std::sin(facing), std::cos(facing), 0.0f);
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 55;
    result.declaredParameterCount = 7;
    result.parameters = {
        {3, SavedObjectReference::fromRuntimeId(_follow->id())}, {1, int32_t {1}},
        {2, point.x}, {2, point.y}, {1, int32_t {0}}, {2, point.x}, {2, point.y},
    };
    return result;
}

static constexpr float kGlanceDistance = 8.0f;

void FollowLeaderAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    // The party has no leader while it is empty: before it is first populated,
    // after a module transition resets it, and once the last member is removed
    // by RemovePartyMember. A FollowLeader action queued on a creature can still
    // be executed in those windows, so there is nothing to follow and the action
    // is dropped instead of dereferencing a null leader.
    auto leader = _game.party().getLeader();
    if (!leader) {
        complete();
        return;
    }

    // Only a party member other than the leader follows; for it the action
    // keeps running once it has caught up.
    auto creatureActor = _game.getObjectById<Creature>(actor.id());
    if (!creatureActor || creatureActor == leader || !_game.party().isMember(*creatureActor)) {
        complete();
        return;
    }

    glm::vec3 destination(leader->position());
    float distance2 = creatureActor->getSquareDistanceTo(glm::vec2(destination));
    bool run = distance2 > kDistanceWalk;

    const glm::vec2 before(creatureActor->position());
    creatureActor->navigateTo(destination, run, kDefaultFollowDistance, dt);
    glanceAtLeader(*creatureActor, leader, before != glm::vec2(creatureActor->position()));
}

// A follower on the move faces where it went and stops looking. One standing
// still looks at the leader, and turns toward a leader beside it, outside the
// head arc but not behind; its facing stays, only its model turns.
void FollowLeaderAction::glanceAtLeader(Creature &follower, const std::shared_ptr<Creature> &leader, bool moved) {
    const float facing = follower.getFacing();
    const glm::vec3 forward(-std::sin(facing), std::cos(facing), 0.0f);
    if (moved) {
        _game.party().noteFollowerFacing(follower, forward);
        follower.lookAt(nullptr, kGlanceDistance);
        return;
    }
    const glm::vec3 offset(leader->position() - follower.position());
    if (glm::length(offset) > 0.0f) {
        const glm::vec3 toLeader(glm::normalize(offset));
        if (std::abs(glm::dot(forward, toLeader)) < std::cos(glm::radians(follower.headTurnHorizontal()))) {
            _game.party().noteFollowerFacing(follower, toLeader);
        }
    }
    follower.lookAt(leader, kGlanceDistance);
}

std::optional<SavedActionRecord> FollowLeaderAction::saveFacingState() const {
    // Action 61 is the abstract party-follow command. It deliberately
    // carries no target or formation parameters: execution resolves the current
    // party leader, while Party/FollowInfo owns formation state.
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 61;
    result.declaredParameterCount = 0;
    result.parameters.clear();
    return result;
}

// A puppet keeps near its owner. Within the range, measured across the ground,
// it waits; farther away it runs to the owner, behind whatever else it was
// told to do, and then follows again. The order never ends by itself; a
// creature that is no longer a puppet drops it.
void FollowOwnerAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto &puppet = cast<Creature>(actor);
    if (!puppet.isPuppet()) {
        complete();
        return;
    }
    auto owner = _game.party().puppetOwner(puppet);
    if (!owner || puppet.getSquareDistanceTo(glm::vec2(owner->position())) < _range * _range) return;
    puppet.addAction(_game.newAction<MoveToObjectAction>(owner, true, _range, false, -1.0f, true));
    puppet.addAction(_game.newAction<FollowOwnerAction>(_range));
    complete();
}

std::optional<SavedActionRecord> FollowOwnerAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 70;
    result.declaredParameterCount = 1;
    result.parameters = {{2, _range}};
    return result;
}

// A jump to a location always lands where the jumper could walk straight to it from.
void JumpToLocationAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    const float facing = objectFacingFromScript(_location->facing());
    jumpToPositionFacing(cast<Creature>(actor), _location->position(), facing, facing, kJumpSearchRadius, true, _game);
    complete();
}

// A jump is kept as the point in the current area, a straight line to it, the
// search radius and the facing.
std::optional<SavedActionRecord> JumpToLocationAction::saveFacingState() const {
    auto module = _game.module();
    if (!module || !module->area()) {
        return std::nullopt;
    }
    const glm::vec3 &position = _location->position();
    const glm::vec3 orientation = _location->saveOrientation();
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 5;
    result.declaredParameterCount = 8;
    result.parameters = {
        {2, position.x}, {2, position.y}, {2, position.z},
        {3, SavedObjectReference::fromRuntimeId(module->area()->id())},
        {1, int32_t {1}}, {2, kJumpSearchRadius}, {2, orientation.x}, {2, orientation.y},
    };
    return result;
}

void JumpToObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    jumpToObject(cast<Creature>(actor), *_toJumpTo, _walkStraightLine, _game);
    complete();
}

std::optional<SavedActionRecord> JumpToObjectAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 48;
    result.declaredParameterCount = 2;
    result.parameters = {
        {3, SavedObjectReference::fromRuntimeId(_toJumpTo->id())},
        {1, int32_t {_walkStraightLine ? 1 : 0}},
    };
    return result;
}

void ChangeFacingAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto &creature = cast<Creature>(actor);
    if (_target) {
        creature.turnToward(*_target);
    } else {
        creature.turnToward(_point);
    }
    complete();
}

// A turn is kept as the object faced, or as the point faced.
std::optional<SavedActionRecord> ChangeFacingAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    if (_target) {
        result.actionId = 19;
        result.declaredParameterCount = 1;
        result.parameters = {{3, SavedObjectReference::fromRuntimeId(_target->id())}};
    } else {
        result.actionId = 49;
        result.declaredParameterCount = 3;
        result.parameters = {{2, _point.x}, {2, _point.y}, {2, _point.z}};
    }
    return result;
}

// A walker stands this long after each try at a walk, and this long, without
// walking, while its AI runs at the lowest level.
static constexpr float kRandomWalkPause = 3.0f;
static constexpr float kRandomWalkIdlePause = 15.0f;

// Each time the walk comes up it throws away whatever was queued behind it,
// then goes back behind a pause, and, when it finds a point near its home that
// the walker can walk straight to, behind a walk to that point too. It never
// ends by itself.
void RandomWalkAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto *walker = dyn_cast<Creature>(&actor);
    if (!walker) {
        complete();
        return;
    }
    walker->removeActionsBehind(*this);
    if (walker->aiLevel() <= 0) {
        walker->addActionBefore(*this, _game.newAction<WaitAction>(kRandomWalkIdlePause));
        return;
    }
    auto pause = _game.newAction<WaitAction>(kRandomWalkPause);
    walker->addActionBefore(*this, pause);
    if (auto point = _game.module()->area()->randomWalkPoint(*walker, _home)) {
        walker->addActionBefore(*pause, _game.newAction<MoveToLocationAction>(
                                            std::make_shared<Location>(*point, 0.0f), false, false, -1.0f, 0.0f, true));
    }
}

std::optional<SavedActionRecord> RandomWalkAction::saveFacingState() const {
    auto module = _game.module();
    if (!module || !module->area()) {
        return std::nullopt;
    }
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 45;
    result.declaredParameterCount = 4;
    result.parameters = {
        {2, _home.x},
        {2, _home.y},
        {2, _home.z},
        {3, SavedObjectReference::fromRuntimeId(module->area()->id())},
    };
    return result;
}

} // namespace game

} // namespace reone
