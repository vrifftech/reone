/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "../effect.h"
#include "../poisondata.h"
namespace reone::game {
class PoisonEffect : public CopyableEffect<PoisonEffect> {
public:
    explicit PoisonEffect(Poison type) : CopyableEffect(EffectType::Poison) {
        setSaveFacingInteger(0, static_cast<int>(type));
    }
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
    void onUpdate(Object &, const EffectInstance &, float) override;
private:
    EffectApplicationResult queueRemoval(Object &, EffectId);
    void applyDamage(Creature &, const EffectInstance &, float factor);
};
} // namespace reone::game
