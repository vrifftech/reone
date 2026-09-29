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

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "savedruntime.h"
#include "runtimeref.h"
#include "types.h"

namespace reone {

namespace game {

struct ServicesView;

class AttackBuffer;
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
    /** Strip the special-attack type from every attack record of the current round. */
    virtual void clearSpecialAttacks() {}
    /** Current physical record, distinct from the last-used feat. */
    virtual uint16_t currentCombatAttackType() const { return 0; }
    /** Result held by the current physical record. */
    virtual AttackResultType currentCombatAttackResult() const { return AttackResultType::Invalid; }
    /** The attack records of this action's round, when it runs one. */
    virtual AttackBuffer *combatAttacks() { return nullptr; }
    // Wrappers keep their queue identity while exposing the executing command.
    virtual Action &combatAction() { return *this; }
    virtual const Action &combatAction() const { return *this; }
    virtual bool holdsCombatRound() const { return false; }
    /** The action has taken its round's one attack, cast or item use. */
    virtual bool tookRoundAction() const { return false; }
    /**
     * The command runs inside a round its creature is still in, rather than
     * starting the round afresh.
     */
    virtual bool joinsRunningRound() const { return false; }
    virtual bool suppressesEndRoundScript() const { return _cutsceneAttack; }
    bool isCutsceneAttack() const { return _cutsceneAttack; }
    void setCutsceneAttack(bool value) { _cutsceneAttack = value; }

    /** Clearing leaves an action that is not clearable running; only teardown removes it. */
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
    /** Ordinary action ID as saved, distinct from ActionType. */
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

/**
 * The number scripts know a queued action by (the ACTION_* constants), for an
 * ordinary action ID; nothing for an action scripts cannot name.
 */
std::optional<int> scriptActionNumber(uint32_t actionId, bool tsl);

/** One ordinary node; identity is not the identity of its Action payload. */
struct ActionQueueNode {
    std::shared_ptr<Action> action;
    std::optional<SavedActionRecord> opaque;
    uint32_t actionId {0xffff};
    uint16_t groupId {0};
    bool refusalReported {false};
};

/**
 * The node list is authoritative, including non-executable saved records.
 * Iteration exposes only executable Action payloads to existing UI/callers;
 * execution, serialization and pending/group queries use nodes directly.
 */
class OrdinaryActionQueue {
public:
    static constexpr uint16_t kNewGroup = 0xffff;
    static constexpr uint16_t kLastGroup = 0xfffe;
    using Node = std::shared_ptr<ActionQueueNode>;
    using Nodes = std::deque<Node>;

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::shared_ptr<Action>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type *;
        using reference = const value_type &;

        const_iterator() = default;
        const_iterator(Nodes::const_iterator position, Nodes::const_iterator end) :
            _position(position), _end(end) { skipOpaque(); }
        reference operator*() const { return (*_position)->action; }
        pointer operator->() const { return &(*_position)->action; }
        const_iterator &operator++() { ++_position; skipOpaque(); return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const const_iterator &other) const { return _position == other._position; }
        bool operator!=(const const_iterator &other) const { return !(*this == other); }

    private:
        Nodes::const_iterator _position;
        Nodes::const_iterator _end;
        void skipOpaque() {
            while (_position != _end && !(*_position)->action) ++_position;
        }
    };

    const_iterator begin() const { return {nodes.begin(), nodes.end()}; }
    const_iterator end() const { return {nodes.end(), nodes.end()}; }
    bool empty() const { return begin() == end(); }
    size_t size() const { return static_cast<size_t>(std::distance(begin(), end())); }
    const std::shared_ptr<Action> &front() const { assert(!empty()); return *begin(); }
    const std::shared_ptr<Action> &back() const {
        auto position = std::find_if(nodes.rbegin(), nodes.rend(),
            [](const auto &node) { return node->action != nullptr; });
        assert(position != nodes.rend());
        return (*position)->action;
    }
    const std::shared_ptr<Action> &operator[](size_t index) const {
        auto position = begin();
        while (index != 0 && position != end()) { --index; ++position; }
        assert(position != end());
        return *position;
    }
    bool operator==(const OrdinaryActionQueue &other) const {
        return std::equal(begin(), end(), other.begin(), other.end());
    }
    bool operator!=(const OrdinaryActionQueue &other) const { return !(*this == other); }
    void clear() { nodes.clear(); }

    /** Explicit IDs do not change either allocation counter. */
    uint16_t allocateGroup(uint16_t requested) {
        if (requested == kNewGroup) {
            const uint16_t group = _nextGroup;
            _nextGroup = static_cast<uint16_t>(_nextGroup + 1);
            if (_nextGroup == kNewGroup) _nextGroup = 0;
            _lastGroup = group;
            return group;
        }
        return requested == kLastGroup ? _lastGroup : requested;
    }

    Nodes nodes;

private:
    uint16_t _nextGroup {0};
    uint16_t _lastGroup {0};
};

namespace detail {

/** Shared action-group classifier; the extra action IDs are TSL additions. */
inline bool isAcceptableGroupAction(uint32_t action, bool tsl) {
    switch (action) {
    case 1: case 7: case 9: case 12: case 15: case 20: case 21:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30:
    case 38: case 39: case 40: case 41: case 42: case 43: case 46:
    case 50: case 54: case 55: case 56: case 61: case 63:
        return true;
    case 67: case 68: case 69: case 70: case 71:
        return tsl;
    default:
        return false;
    }
}

/**
 * Classify the first contiguous run of each queried group using its last acceptable node.
 * \p accept decides from that action ID and node whether the group goes. Group removal
 * deletes all nodes with that identity; iteration resumes at group index one.
 */
template <class Node, class Group, class ActionId, class Accept, class Remove>
void discardCombatActionGroups(std::vector<Node> &nodes, bool tsl,
                               Group group, ActionId actionId, Accept accept, Remove remove) {
    auto countGroups = [&]() {
        size_t count = 0;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (i == 0 || group(nodes[i]) != group(nodes[i - 1])) ++count;
        return count;
    };
    const size_t originalCount = countGroups();
    for (size_t index = 0; index < originalCount; ++index) {
        size_t start = 0, current = 0;
        while (start < nodes.size() && current < index) {
            const auto key = group(nodes[start]);
            do { ++start; } while (start < nodes.size() && group(nodes[start]) == key);
            ++current;
        }
        if (start == nodes.size()) continue;
        const auto key = group(nodes[start]);
        auto first = std::find_if(nodes.begin(), nodes.end(),
            [&](const auto &node) { return group(node) == key; });
        uint32_t selected = 0xffff;
        auto representative = first;
        for (auto it = first; it != nodes.end() && group(*it) == key; ++it) {
            const auto id = actionId(*it);
            if (isAcceptableGroupAction(id, tsl)) {
                selected = id;
                representative = it;
            }
        }
        if (selected == 0xffff || !accept(selected, *representative)) continue;
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const auto &node) {
            if (group(node) != key) return false;
            remove(node);
            return true;
        }), nodes.end());
        index = 0;
    }
}

} // namespace detail

} // namespace game

} // namespace reone
