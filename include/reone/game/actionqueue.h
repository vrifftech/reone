/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "action.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>

namespace reone::game {

/** One ordinary node; identity is not the identity of its Action payload. */
struct ActionQueueNode {
    std::shared_ptr<Action> action;
    std::optional<SavedActionRecord> opaque;
    uint32_t actionId {0xffff};
    uint16_t groupId {0};
    bool clearable {true};
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

} // namespace reone::game
