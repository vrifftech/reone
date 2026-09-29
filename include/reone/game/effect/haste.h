/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "../effect.h"
#include <algorithm>

namespace reone::game {
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
    result.subType = 0x8;
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
    explicit HasteSlowEffect(bool haste) : CopyableEffect(haste ? EffectType::Haste : EffectType::Slow) {}
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};

/** One selected 41/42 internal, with its own package identity. */
class HasteSlowInternalEffect : public CopyableEffect<HasteSlowInternalEffect> {
public:
    explicit HasteSlowInternalEffect(bool haste) : CopyableEffect(EffectType::Invalid), _haste(haste) {}
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
private:
    bool _haste;
};
} // namespace reone::game
