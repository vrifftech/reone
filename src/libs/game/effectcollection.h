/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <algorithm>
#include <vector>
#include "reone/game/effect.h"

namespace reone::game::detail {
template<class Members, class Apply>
EffectPackageApplicationResult applyEffectPackageMembers(const Members &members, Apply apply) {
    EffectPackageApplicationResult result;
    for (const auto &member : members) {
        if (apply(member)) ++result.accepted; else ++result.rejected;
    }
    return result;
}

struct EffectCollectionUnchanged { void operator()() const {} };

// Application order, not package ID, identifies the record across callbacks.
// The current record and subsequent siblings remain queryable during removal.
template<class Collection, class Remove, class AfterRemove = EffectCollectionUnchanged>
bool removeEffectApplication(Collection &effects, uint64_t order,
                             std::vector<uint64_t> &inFlight, Remove remove, AfterRemove afterRemove = {}) {
    const auto find = [&]() { return std::find_if(effects.begin(), effects.end(),
        [order](const auto &record) { return record.applicationOrder == order; }); };
    auto at = find();
    if (at == effects.end() || std::find(inFlight.begin(), inFlight.end(), order) != inFlight.end())
        return false;
    const auto record = *at;
    struct Invocation {
        std::vector<uint64_t> &stack;
        Invocation(std::vector<uint64_t> &stack, uint64_t order) : stack(stack) { stack.push_back(order); }
        ~Invocation() { stack.pop_back(); }
    } invocation(inFlight, order);
    if (remove(record) != EffectRemovalResult::Removed) return false;
    at = find();
    if (at == effects.end()) return false;
    effects.erase(at);
    afterRemove();
    return true;
}

template<class Collection, class Remove, class AfterRemove = EffectCollectionUnchanged>
size_t removeEffectPackage(Collection &effects, EffectId id, std::vector<uint64_t> &inFlight,
                           Remove remove, AfterRemove afterRemove = {}) {
    size_t count = 0;
    std::vector<uint64_t> visited;
    for (;;) {
        auto at = std::find_if(effects.begin(), effects.end(), [&](const auto &record) {
            return record.id == id &&
                std::find(visited.begin(), visited.end(), record.applicationOrder) == visited.end() &&
                std::find(inFlight.begin(), inFlight.end(), record.applicationOrder) == inFlight.end();
        });
        if (at == effects.end()) return count;
        const auto order = at->applicationOrder;
        visited.push_back(order);
        if (removeEffectApplication(effects, order, inFlight, remove, afterRemove)) ++count;
    }
}
} // namespace reone::game::detail
