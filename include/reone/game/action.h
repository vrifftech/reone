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

#include "savedruntime.h"
#include "runtimeref.h"
#include "types.h"

namespace reone {

namespace game {

struct ServicesView;

class Creature;
class Game;
class Object;

class Action : boost::noncopyable {
public:
    virtual ~Action() = default;

    static bool classof(Action *from) {
        return true;
    }

    /**
     * Execute the action. This function is called every update until it is
     * either complete(), or cancelled, or the actor it belongs to is dead.
     */
    virtual void execute(std::shared_ptr<Action> self, Object &actor, float dt);

    /**
     * Perform cancellation cleanup while the node is still linked. Command
     * clearing erases the node only if this returns true; forced teardown uses
     * its own policy.
     */
    virtual bool cancel(std::shared_ptr<Action> self, Object &actor) { return true; }

    /** Ordinary queue insertion, distinct from combat-round registration. */
    virtual void onQueued(Object &actor) {}
    /** Retire attack-record state without forcing a surviving ordinary action away. */
    virtual void retireCombatRound() {}
    // Wrappers keep their queue identity while exposing the executing command.
    virtual Action &combatAction() { return *this; }
    virtual const Action &combatAction() const { return *this; }
    virtual bool holdsCombatRound() const { return false; }
    virtual bool suppressesEndRoundScript() const { return _cutsceneAttack; }
    bool isCutsceneAttack() const { return _cutsceneAttack; }
    void setCutsceneAttack(bool value) { _cutsceneAttack = value; }

    bool isClearable() const { return _clearable; }
    void setClearable(bool value) { _clearable = value; }

    /**
     * Actions must call complete() once they are done. Completed actions are
     * removed from the action queue, allowing subsequent actions to execute.
     */
    void complete() { _completed = true; }

    /**
     * Locked action cannot be cancelled, unless the creature it belongs to is dead.
     */
    bool locked() const { return _locked; }

    /**
     * Lock the action to prevent it from being cancelled. AI scripts may
     * request to cancel all actions. Actions that are already in progress
     * (attack, open lock, etc.) should be locked.
     */
    void lock() { _locked = true; }

    ActionType type() const { return _type; }
    /** Ordinary action ID, distinct from the engine/UI action enum. */
    virtual uint32_t serializedActionId() const;

    bool isUserAction() const { return _userAction; }
    bool isCompleted() const { return _completed; }
    bool isCancelled() const { return _cancelled; }

    void setUserAction(bool val) { _userAction = val; }
    void markCancelled() { _cancelled = true; }

    bool isScheduledCommand() const { return _scheduledCommand; }
    void setScheduledCommand(bool value) { _scheduledCommand = value; }
    void attachSavedAction(SavedActionRecord record) {
        _scheduledCommand = record.scheduled;
        _savedAction = std::move(record);
    }
    const std::optional<SavedActionRecord> &originalSavedAction() const {
        return _savedAction;
    }
    /** Semantic snapshot only; GFF encoding belongs to later E3. */
    virtual std::optional<SavedActionRecord> saveFacingState() const {
        return _savedAction;
    }

    /**
     * True while every non-owning gameplay object used by this action still
     * denotes the exact live incarnation captured by the action.
     */
    bool runtimeDependenciesLive() const;

protected:
    const float kDefaultMaxObjectDistance = 2.0f;
    const float kDistanceWalk = 4.0f;

    Game &_game;
    ServicesView &_services;
    ActionType _type;

    bool _userAction {false};
    bool _completed {false};
    bool _cancelled {false};
    bool _locked {false};
    bool _clearable {true};
    bool _cutsceneAttack {false};
    bool _scheduledCommand {false};
    std::optional<SavedActionRecord> _savedAction;
    std::vector<RuntimeObjectRef<Object>> _runtimeDependencies;

    void requireRuntimeObject(const std::shared_ptr<Object> &object);

    Action(
        Game &game,
        ServicesView &services,
        ActionType type) :
        _game(game),
        _services(services),
        _type(type) {
    }
};

/**
 * Returns true for attacks, feats, some spells (force powers, grenades).
 */
bool isHostileAction(Action &action);

} // namespace game

} // namespace reone
