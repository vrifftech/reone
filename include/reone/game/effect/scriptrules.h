/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "../effect.h"

namespace reone::game {
template<class Records>
std::vector<EffectId> selectScriptEffectRemovals(Records &records,
        ScriptEffectRemovalMatch match, const EffectInstance &value) {
    std::vector<EffectId> result;
    for (auto &record : records) {
        const bool selected = match == ScriptEffectRemovalMatch::PackageId
            ? record.id == value.id
            : match == ScriptEffectRemovalMatch::Integer0
                ? record.integerParameter(0) == value.integerParameter(0)
                : record.serializedType == value.serializedType &&
                  record.integerParameter(0) == value.integerParameter(0) &&
                  record.integerParameter(1) == value.integerParameter(1);
        if (!selected) continue;
        // Scripts hide the selected records immediately; removal itself
        // is queued. RemoveEffect queues once after hiding every package member.
        record.exposed = 0;
        if (match == ScriptEffectRemovalMatch::PackageId) result.assign(1, record.id);
        else result.push_back(record.id);
        if (match == ScriptEffectRemovalMatch::TypeAndFirstTwoIntegers) break;
    }
    return result;
}
} // namespace reone::game
