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

#include "../effect.h"

namespace reone {

namespace game {

struct HasteSlowTransition {
    int before;
    int after;
};

template<class Records>
HasteSlowTransition getHasteSlowTransition(const Records &records, uint16_t type, bool removing) {
    int balance = 0;
    for (const auto &record : records) {
        if (record.serializedType > 3) break;
        if (record.serializedType == 1) ++balance;
        else if (record.serializedType == 3) --balance;
    }
    const int contribution = type == 1 ? 1 : -1;
    return {std::clamp(balance, -1, 1),
            std::clamp(balance + (removing ? -contribution : contribution), -1, 1)};
}

inline EffectInstance makeHasteSlowInternal(int selection, const EffectInstance *applyingRoot) {
    EffectInstance result;
    result.serializedType = selection > 0 ? 41 : 42;
    result.subType = kMagicalEffectCategory;
    result.setDuration(DurationType::Innate, 0.0f);
    result.creatorId = kSavedEffectInvalidObjectId;
    result.markGeneratedForLoad();
    if (applyingRoot) {
        result.spellId = applyingRoot->spellId;
        result.restoring = applyingRoot->restoring;
    }
    return result;
}

/** Shared external Haste/Slow roots (1/3), not the modifier package. */
class HasteSlowEffect : public CopyableEffect<HasteSlowEffect> {
public:
    explicit HasteSlowEffect(bool haste) :
        CopyableEffect(haste ? EffectType::Haste : EffectType::Slow) {
    }

    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onRemove(Object &, const EffectInstance &) override;
};

/** One selected 41/42 internal, with its own package identity. */
class HasteSlowInternalEffect : public CopyableEffect<HasteSlowInternalEffect> {
public:
    explicit HasteSlowInternalEffect(bool haste) :
        CopyableEffect(EffectType::Invalid),
        _haste(haste) {
    }

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
private:
    bool _haste;
};

} // namespace game

} // namespace reone
