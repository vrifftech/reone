/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <algorithm>
#include <iterator>
#include <cstdint>
#include <vector>
#include <utility>

namespace reone::game::detail {

/** Capture the next node before the callback; accepted removal happens afterwards. */
template <class Queue, class Remove>
void removeCommandActions(Queue &queue, Remove remove) {
    auto current = queue.empty() ? typename Queue::value_type {} : queue.front();
    while (current) {
        auto position = std::find(queue.begin(), queue.end(), current);
        if (position == queue.end()) break;
        auto following = std::next(position);
        auto next = following == queue.end() ? typename Queue::value_type {} : *following;
        if (remove(current)) {
            position = std::find(queue.begin(), queue.end(), current);
            if (position != queue.end()) queue.erase(position);
        }
        current = std::move(next);
    }
}

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
 * Classify the first contiguous run of each queried group using its last
 * acceptable node. RemoveGroup deletes all nodes with that group identity.
 * The caller resumes at group index one after each removal.
 */
template <class Node, class Group, class ActionId, class Remove>
void discardCombatActionGroups(std::vector<Node> &nodes, bool tsl,
                               Group group, ActionId actionId, Remove remove) {
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
        for (auto it = first; it != nodes.end() && group(*it) == key; ++it) {
            const auto id = actionId(*it);
            if (isAcceptableGroupAction(id, tsl)) selected = id;
        }
        if (selected != 12 && selected != 15 && selected != 46) continue;
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const auto &node) {
            if (group(node) != key) return false;
            remove(node);
            return true;
        }), nodes.end());
        index = 0;
    }
}

} // namespace reone::game::detail
