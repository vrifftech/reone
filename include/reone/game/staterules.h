/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "effect/creaturestate.h"
namespace reone::game {
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
           state == CreatureState::Crush;
}
inline bool stateHasConsumer(CreatureState state) {
    return stateBlocksActions(state) || state == CreatureState::Fear ||
           state == CreatureState::Confusion;
}
inline bool stateControlsActions(CreatureState state) {
    return state == CreatureState::Fear || state == CreatureState::Confusion;
}
inline int getStateAmbientCode(CreatureState state) {
    switch (state) {
    case CreatureState::Stun: return 2;
    case CreatureState::Paralysis: return 12;
    case CreatureState::Sleep: return 11;
    default: return 0;
    }
}
inline EffectInstance makeInternalStateInstance(const EffectInstance &root, bool restoring) {
    auto effect = std::make_shared<CreatureStateInternalEffect>(static_cast<CreatureState>(root.integerParameter(0)));
    const auto descriptor = effect->saveFacingInstance();
    auto result = root.linkedChild(effect);
    result.id = kUnassignedEffectId;
    result.subType = descriptor.subType;
    result.markGeneratedForLoad();
    result.restoring = root.restoring;
    if (restoring) result.spellId = UINT32_MAX;
    result.setDuration(restoring ? DurationType::Permanent : DurationType::Innate, 0.0f);
    return result;
}
} // namespace reone::game
