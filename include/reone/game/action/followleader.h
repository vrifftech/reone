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

#include <optional>

#include "../action.h"

namespace reone {

namespace game {

class Door;

class FollowLeaderAction : public Action {
public:
    FollowLeaderAction(Game &game, ServicesView &services) :
        Action(game, services, ActionType::FollowLeader) {
    }

    static bool classof(Action *from) {
        return from->type() == ActionType::FollowLeader;
    }

    void execute(std::shared_ptr<Action> self, Object &actor, float dt) override;
    std::optional<SavedActionRecord> saveFacingState() const override;
    /** A saved FollowLeader command. */
    static std::shared_ptr<Action> restore(const SavedActionRecord &record, Game &game);

private:
    /** Where the follower is in its walk along the leader's trail. */
    enum class State {
        /** It has just started: it stands where it may be. */
        New,
        /** It waits for a trail to follow, then heads for it. */
        WaitingForPath,
        /** It plans a walk to its place, or to the trail, the way it cannot walk straight. */
        FindPath,
        /** It walks the planned way there. */
        PlannedWalk,
        /** It walks straight to its point on the trail. */
        MovingToPath,
        /** It walks along the trail toward its follow point. */
        FollowingPath,
        /** It walks straight from the trail to its place in the formation. */
        MovingToOffset,
        /** It keeps its place in the formation alongside the trail. */
        Offset,
        /** Held up by someone outside the party, it jumps ahead along the trail. */
        TeleportForward
    };

    State _state {State::New};
    /** Where the follower may stand, which it springs toward. */
    glm::vec3 _safePosition {0.0f};
    /** Its point on the trail, and the trail step it heads for. */
    glm::vec3 _pathPoint {0.0f};
    int _pathIndex {0};
    /** Seconds it has followed the trail since it last walked onto it. */
    float _followingTime {0.0f};
    /** Where its planned walk goes. */
    glm::vec3 _plannedDestination {0.0f};
    /** Seconds before it may plan a walk again, after one that failed. */
    float _planRetryDelay {0.0f};
    /**
     * The party's turn-backs and trail resets it has seen (see
     * Party::followTurnBacks and Party::followPathResets).
     */
    int _seenTurnBacks {0};
    int _seenPathResets {0};
    /** The closed door it last ran its blocked script for, until one of its walks next gets through. */
    std::optional<uint32_t> _blockingDoorId;
    /** Seconds it has waited at that door. */
    float _doorWaitTime {0.0f};
    /** Where this action last left the follower, to tell when something else moved it. */
    std::optional<glm::vec2> _lastPosition;

    /** The follower looks for its way along the trail again from where it stands. */
    void restart(Creature &follower);

    /** One state's turn with \p budget metres of walking; whether the update ends with it. */
    bool runState(Creature &follower, int slot, float &budget);
    /**
     * Moves a point on the trail, heading for the step at \p index, up to
     * \p distance toward the slot's follow point, stopping at the newest step.
     * Gives the distance left over.
     */
    float advanceOnTrail(int slot, float distance, const glm::vec3 &from, int index, glm::vec3 &outPoint, int &outIndex) const;
    /**
     * A walk onto or along the trail stopped by a wall. At a closed door the
     * follower runs its blocked script, once while the door blocks it, and
     * waits, for a few seconds; any other wall, or a door that stays shut,
     * has it plan a walk. Whether the update ends.
     */
    bool onWallBlocked(Creature &follower, const Door *door);
    /** Whether a straight walk is clear, a party member in the way counting as \p memberClear. */
    bool lineClear(const Creature &follower, const glm::vec3 &from, const glm::vec3 &to, float shrink, bool memberClear) const;
    void glanceAtLeader(Creature &follower, const std::shared_ptr<Creature> &leader, bool moved);
};

} // namespace game

} // namespace reone
