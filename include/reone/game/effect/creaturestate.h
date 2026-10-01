/*
 * Copyright (c) 2026 The reone project contributors
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

#include "../effect.h"

namespace reone {

namespace game {

class Creature;

enum class CreatureState : int {
    None = 0,
    Confusion = 1,
    Fear = 2,
    DroidStun = 3,
    Stun = 4,
    Paralysis = 5,
    Sleep = 6,
    Choke = 7,
    Horrified = 8,
    ForcePushed = 9,
    Whirlwind = 10,
    ForceJumpedOnto = 13,
    Knockdown = 14,
    Crush = 15,
    DroidConfused = 16,
    MindTrick = 18,
    DroidScramble = 19
};

enum class StateApplicationResult {
    Rejected,
    Immune,
    Dormant,
    Applied
};

class CreatureStateEffect : public CopyableEffect<CreatureStateEffect> {
public:
    explicit CreatureStateEffect(CreatureState state, bool bypassPackageInspection = false);

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onRemove(Object &, const EffectInstance &) override;
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
    void onRemove(Object &, const EffectInstance &) override;
};

class CreatureAIStateEffect : public CopyableEffect<CreatureAIStateEffect> {
public:
    explicit CreatureAIStateEffect(int mask);

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onRemove(Object &, const EffectInstance &) override;
};

class EffectIconMarkerEffect : public CopyableEffect<EffectIconMarkerEffect> {
public:
    explicit EffectIconMarkerEffect(int iconId);

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onRemove(Object &, const EffectInstance &) override;
};

class VisualEffectMarkerEffect : public CopyableEffect<VisualEffectMarkerEffect> {
public:
    explicit VisualEffectMarkerEffect(int visualEffectId);

    EffectApplicationResult onApply(Object &, EffectInstance &) override;
};

class LimitMovementSpeedEffect : public CopyableEffect<LimitMovementSpeedEffect> {
public:
    LimitMovementSpeedEffect() :
        CopyableEffect(EffectType::Invalid) {
        setSaveFacingInteger(0, 0);
    }

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onRemove(Object &, const EffectInstance &) override;
};

// Records a creature's special walk animation (an iprp_walk row).
class WalkAnimationEffect : public CopyableEffect<WalkAnimationEffect> {
public:
    explicit WalkAnimationEffect(int walkAnimation) :
        CopyableEffect(EffectType::Invalid) {
        setSaveFacingInteger(0, walkAnimation);
    }

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
};

// Records the visual effect that lights its holder; the visual itself is a
// separate application from the same source.
class LightEffect : public CopyableEffect<LightEffect> {
public:
    explicit LightEffect(int visualEffectId) :
        CopyableEffect(EffectType::Invalid) {
        setSaveFacingInteger(0, visualEffectId);
    }

    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override { return EffectApplicationResult::Retained; }
};

bool hasStateSpecificImmunity(const Creature &, CreatureState, const Creature *creator);
bool hasStateImmunity(const Creature &, CreatureState, const Creature *creator);
bool hasSlowImmunity(const Creature &, const Creature *creator);
StateApplicationResult applyStatePackage(Object &, CreatureState, float duration,
                                        const std::shared_ptr<Object> &creator);
bool applySlowPackage(Object &, float duration, const std::shared_ptr<Object> &creator);

template<class Records>
inline bool hasMovementLimitSurvivor(const Records &records, uint64_t removingOrder) {
    for (const auto &record : records) {
        if (record.serializedType > 59) break;
        if (record.serializedType == 59 && record.applicationOrder != removingOrder) return true;
    }
    return false;
}

inline bool stateBlocksActions(CreatureState state) {
    return state == CreatureState::Stun || state == CreatureState::Paralysis || state == CreatureState::Sleep ||
           state == CreatureState::DroidStun || state == CreatureState::Choke ||
           state == CreatureState::Horrified || state == CreatureState::Whirlwind ||
           state == CreatureState::MindTrick || state == CreatureState::DroidScramble ||
           state == CreatureState::Crush || state == CreatureState::ForceJumpedOnto;
}

inline bool stateHasConsumer(CreatureState state) {
    return stateBlocksActions(state) || state == CreatureState::Fear ||
           state == CreatureState::Confusion || state == CreatureState::DroidConfused ||
           state == CreatureState::Knockdown || state == CreatureState::ForcePushed;
}

/**
 * The ambient animation state a creature held by the state takes, which
 * chooses the pose it holds. KotOR knows none of the TSL-only states.
 */
inline int getStateAmbientCode(CreatureState state, bool tsl) {
    switch (state) {
    case CreatureState::DroidStun: return 8;
    case CreatureState::Stun: return 2;
    case CreatureState::Paralysis: return 12;
    case CreatureState::Sleep: return 11;
    case CreatureState::Choke: return 5;
    case CreatureState::Horrified: return 7;
    case CreatureState::Whirlwind: return 6;
    case CreatureState::ForcePushed: return 9;
    // A knocked-down creature lies prone.
    case CreatureState::Knockdown: return tsl ? 15 : 0;
    case CreatureState::Crush: return tsl ? 16 : 0;
    case CreatureState::MindTrick:
    case CreatureState::DroidScramble: return tsl ? 2 : 0;
    default: return 0;
    }
}

/**
 * Whether laying the state sets the creature's pose afresh. The states that
 * hold its AI do; the others leave it the animation it is playing.
 */
inline bool stateAppliesPose(int state, bool tsl) {
    if (state >= 3 && state <= 10) return true;
    return tsl && (state == 14 || state == 15 || state == 17 || state == 18 || state == 19);
}

/**
 * Whether lifting the state runs the creature's end-of-round script. Fear (2)
 * does not, and KotOR knows none of the states from 12 on.
 */
inline bool stateRemovalRunsEndRoundScript(int state, bool tsl) {
    if (state < 0 || state >= 20) return false;
    const uint32_t states = tsl ? 0xfc7fau : 0x7fau;
    return (states & (1u << state)) != 0;
}

/**
 * Whether lifting the state sets the creature's pose afresh: the states whose
 * end runs the end-of-round script, and states 12 and 13.
 */
inline bool stateRemovalAppliesPose(int state, bool tsl) {
    return stateRemovalRunsEndRoundScript(state, tsl) || state == 12 || state == 13;
}

/**
 * The internal record that lays the root's state. A root being applied lays it
 * for as long as it lasts, with its own spell; when a root is removed, the
 * strongest remaining one lays it for good, with no spell.
 */
inline EffectInstance makeInternalStateInstance(const EffectInstance &root, bool afterRemoval) {
    auto effect = std::make_shared<CreatureStateInternalEffect>(static_cast<CreatureState>(root.integerParameter(0)));
    const auto descriptor = effect->saveFacingInstance();
    auto result = root.linkedChild(effect);
    result.id = kUnassignedEffectId;
    result.subType = descriptor.subType;
    result.markGeneratedForLoad();
    result.restoring = root.restoring;
    if (afterRemoval) result.spellId = UINT32_MAX;
    result.setDuration(afterRemoval ? DurationType::Permanent : DurationType::Innate, 0.0f);
    return result;
}

} // namespace game

} // namespace reone
