/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "../effect.h"

namespace reone::game {
class Creature;
enum class CreatureState : int { None=0, Confusion=1, Fear=2, DroidStun=3, Stun=4, Paralysis=5, Sleep=6, Choke=7, Horrified=8, Whirlwind=10, Crush=15, MindTrick=18, DroidScramble=19 };
enum class StateApplicationResult { Rejected, Immune, Dormant, Applied };

class CreatureStateEffect : public CopyableEffect<CreatureStateEffect> {
public:
    explicit CreatureStateEffect(CreatureState state, bool bypassPackageInspection = false);
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
    CreatureState state() const { return _state; }
    StateApplicationResult result() const { return _result; }
private:
    CreatureState _state;
    StateApplicationResult _result {StateApplicationResult::Rejected};
};
class CreatureStateInternalEffect : public CopyableEffect<CreatureStateInternalEffect> {
public:
    explicit CreatureStateInternalEffect(CreatureState state);
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};
class CreatureAIStateEffect : public CopyableEffect<CreatureAIStateEffect> {
public:
    explicit CreatureAIStateEffect(int mask);
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};
class EffectIconMarkerEffect : public CopyableEffect<EffectIconMarkerEffect> {
public:
    explicit EffectIconMarkerEffect(int iconId);
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};
class VisualEffectMarkerEffect : public CopyableEffect<VisualEffectMarkerEffect> {
public:
    explicit VisualEffectMarkerEffect(int visualEffectId);
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
};
class LimitMovementSpeedEffect : public CopyableEffect<LimitMovementSpeedEffect> {
public:
    LimitMovementSpeedEffect() : CopyableEffect(EffectType::Invalid) { setSaveFacingInteger(0, 0); }
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};


bool hasStateSpecificImmunity(const Creature &, CreatureState, const Creature *creator);
bool hasStateImmunity(const Creature &, CreatureState, const Creature *creator);
bool hasSlowImmunity(const Creature &, const Creature *creator);
StateApplicationResult applyStatePackage(Object &, CreatureState, float duration,
                                        const std::shared_ptr<Object> &creator);
bool applySlowPackage(Object &, float duration, const std::shared_ptr<Object> &creator);
} // namespace reone::game
