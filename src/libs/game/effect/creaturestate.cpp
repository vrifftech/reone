/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effect/haste.h"
#include "reone/game/effect/beam.h"
#include "reone/game/effect/visual.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/paralyze.h"
#include "reone/game/effect/sleep.h"
#include "reone/game/effect/stunned.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/movementspeeddecrease.h"
#include "reone/game/effect/savingthrowdecrease.h"
#include "reone/game/game.h"
#include "reone/game/staterules.h"
#include "reone/game/combat.h"
#include "reone/game/object/creature.h"

namespace reone::game {
static bool hasImplementedStateConsumer(CreatureState state) {
    return stateHasConsumer(state);
}

bool hasStateSpecificImmunity(const Creature &target, CreatureState state,
                             const Creature *creator) {
    switch (state) {
    case CreatureState::Sleep: return target.hasEffectImmunity(ImmunityType::Sleep, creator);
    case CreatureState::Paralysis:
        return target.hasEffectImmunity(ImmunityType::Paralysis, creator) ||
               target.hasEffectiveFeat(FeatType::ForceImmunityParalysis);
    case CreatureState::Stun:
        return target.hasEffectImmunity(ImmunityType::Stun, creator) ||
               target.hasEffectiveFeat(FeatType::ForceImmunityStun) ||
               target.hasEffectiveFeat(FeatType::ForceImmunityParalysis);
    case CreatureState::Fear:
        return target.hasEffectImmunity(ImmunityType::Fear, creator) ||
               (target.game().isTSL() && target.hasEffectiveFeat(FeatType::MandalorianCourage));
    case CreatureState::Confusion: return target.hasEffectImmunity(ImmunityType::Confused, creator);
    case CreatureState::MindTrick: return target.hasEffectImmunity(ImmunityType::MindSpells, creator);
    case CreatureState::DroidScramble: return false;
    case CreatureState::Horrified:
        return target.hasEffectiveFeat(FeatType::ForceImmunityFear) ||
               (target.game().isTSL() && target.hasEffectiveFeat(FeatType::MandalorianCourage));
    case CreatureState::DroidStun:
    case CreatureState::Choke:
    case CreatureState::Whirlwind:
    case CreatureState::Crush:
    case CreatureState::None: return false;
    }
    return false;
}
bool hasStateImmunity(const Creature &target, CreatureState state, const Creature *creator) {
    if (hasStateSpecificImmunity(target, state, creator)) return true;
    switch (state) {
    case CreatureState::DroidStun:
        return target.hasEffectImmunity(ImmunityType::Stun, creator) || target.hasEffectImmunity(ImmunityType::Dazed, creator);
    case CreatureState::Choke: return target.hasEffectImmunity(ImmunityType::All, creator);
    case CreatureState::Horrified:
        return target.hasEffectImmunity(ImmunityType::MindSpells, creator) || target.hasEffectImmunity(ImmunityType::Fear, creator);
    case CreatureState::Whirlwind:
    case CreatureState::Crush:
    case CreatureState::DroidScramble:
    case CreatureState::Paralysis:
    case CreatureState::None: return false;
    default: return target.hasEffectImmunity(ImmunityType::MindSpells, creator);
    }
}
bool hasSlowImmunity(const Creature &target, const Creature *creator) {
    return target.hasEffectImmunity(ImmunityType::Slow, creator);
}

CreatureStateEffect::CreatureStateEffect(CreatureState state, bool bypassPackageInspection) :
    CopyableEffect(EffectType::Invalid), _state(state) {
    switch (state) {
    case CreatureState::Stun: _type = EffectType::Stunned; break;
    case CreatureState::Paralysis: _type = EffectType::Paralyze; break;
    case CreatureState::Sleep: _type = EffectType::Sleep; break;
    case CreatureState::Fear: _type = EffectType::Frightened; break;
    case CreatureState::Confusion: _type = EffectType::Confused; break;
    case CreatureState::DroidStun: _type = EffectType::DroidStun; break;
    case CreatureState::Choke: _type = EffectType::Choke; break;
    case CreatureState::Horrified: _type = EffectType::Horrified; break;
    case CreatureState::Whirlwind: _type = EffectType::WhirlWind; break;
    case CreatureState::Crush: _type = EffectType::Crush; break;
    case CreatureState::MindTrick: _type = EffectType::MindTrick; break;
    case CreatureState::DroidScramble: _type = EffectType::DroidScramble; break;
    case CreatureState::None: break;
    }
    setSaveFacingInteger(0, static_cast<int>(state));
    setSaveFacingInteger(1, bypassPackageInspection ? 1 : 0);
}
EffectInstance CreatureStateEffect::saveFacingInstance() const {
    auto result = Effect::saveFacingInstance();
    result.serializedType = 8;
    return result;
}
EffectApplicationResult CreatureStateEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    // The root retains an inert record on non-creatures, in both modes.
    if (!creature) return EffectApplicationResult::Retained;
    if ((_state == CreatureState::MindTrick || _state == CreatureState::DroidScramble) && !object.game().isTSL())
        return EffectApplicationResult::Rejected;
    if (!hasImplementedStateConsumer(_state))
        return instance.restoring ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
    auto creator = instance.boundCreator();
    const bool bypass = instance.integerParameter(1) != 0;
    const SavedEffectValue candidate(instance);
    // Sleep, Fear and Confusion retain their direct immunity checks even when
    // package inspection is bypassed. Restoration only changes child apply mode.
    const bool checkStateImmunity = !bypass || _state == CreatureState::Sleep ||
        _state == CreatureState::Fear || _state == CreatureState::Confusion;
    const bool immune = (!bypass && creature->isEffectLinkImmune(candidate)) ||
        (checkStateImmunity &&
         hasStateSpecificImmunity(*creature, _state, dyn_cast<Creature>(creator.get())));
    if (immune) {
        _result = StateApplicationResult::Immune;
        return EffectApplicationResult::Rejected;
    }
    auto commonAI = instance.linkedChild(std::make_shared<CreatureAIStateEffect>(-257));
    commonAI.markGeneratedForLoad();
    object.applyEffect(std::move(commonAI));
    creature->onStateRootApplied(instance);
    _result = creature->activeStateRootId() == instance.id
        ? StateApplicationResult::Applied : StateApplicationResult::Dormant;
    return EffectApplicationResult::Retained;
}
EffectRemovalResult CreatureStateEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (!object.isClearingEffects()) {
        if (auto *creature = dyn_cast<Creature>(&object)) creature->rebuildStateEffects(nullptr, instance.applicationOrder);
    }

    return EffectRemovalResult::Removed;
}

CreatureStateInternalEffect::CreatureStateInternalEffect(CreatureState state) :
    CopyableEffect(EffectType::Invalid) { setSaveFacingInteger(0, static_cast<int>(state)); }
EffectInstance CreatureStateInternalEffect::saveFacingInstance() const {
    auto result = Effect::saveFacingInstance(); result.serializedType = 9; return result;
}
EffectApplicationResult CreatureStateInternalEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    const int state = instance.integerParameter(0);
    if (state == 4 || state == 5 || state == 6)
        creature->beginStateImmobilization();
    // State 9 alone leaves action queues intact. Load mode changes child
    // admission, not cancellation; saved continuations are published afterward.
    if (state != 9) {
        creature->clearAllActions(true);
        object.game().combat().cancelActions(*creature);
    }
    std::vector<EffectInstance> members;
    if (state == 3) {
        members.push_back(instance.linkedChild(std::make_shared<VisualEffectMarkerEffect>(1007)));
        members.push_back(instance.linkedChild(std::make_shared<SavingThrowDecreaseEffect>(
            0, 2, SavingThrowType::All)));
    }
    if (state == 2)
        members.push_back(instance.linkedChild(std::make_shared<SavingThrowDecreaseEffect>(
            0, 2, SavingThrowType::All)));
    const bool stunnedVisual = state == 4 ||
        (object.game().isTSL() && (state == 18 || state == 19));
    const bool auxiliaryAI = object.game().isTSL() && (state == 14 || state == 15 || state == 17);
    if ((state >= 3 && state <= 10) || stunnedVisual || auxiliaryAI) {
        const int mask = state == 5 ? -15 : state == 6 ? -351 : -3;
        members.push_back(instance.linkedChild(std::make_shared<CreatureAIStateEffect>(mask)));
    }
    if (stunnedVisual)
        members.push_back(instance.linkedChild(std::make_shared<VisualEffectMarkerEffect>(2002)));
    for (auto &member : members) member.markGeneratedForLoad();
    object.applyEffectPackage(members);
    return EffectApplicationResult::Retained;
}
EffectRemovalResult CreatureStateInternalEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        const int state = instance.integerParameter(0);
        // Removal tables exclude Fear (2), including during load.
        const uint32_t scriptStates = object.game().isTSL() ? 0xfc7fa : 0x7fa;
        if (state >= 0 && state < 20 && (scriptStates & (1u << state)) != 0)
            creature->runEndRoundScript();
        creature->onInternalStateRemoved(instance.id);
    }

    return EffectRemovalResult::Removed;
}
CreatureAIStateEffect::CreatureAIStateEffect(int mask) : CopyableEffect(EffectType::Invalid) {
    setSaveFacingInteger(0, mask);
}
EffectInstance CreatureAIStateEffect::saveFacingInstance() const {
    auto result = Effect::saveFacingInstance(); result.serializedType = 23; return result;
}
EffectApplicationResult CreatureAIStateEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    creature->recomputeAIStateEffects(creature->effectAIStateMask() & instance.integerParameter(0));
    return EffectApplicationResult::Retained;
}
EffectRemovalResult CreatureAIStateEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        // Removal does not revive an already-zero cached AI word.
        if (creature->effectAIStateMask() != 0)
            creature->recomputeAIStateEffects(0xffff, instance.applicationOrder);
    }

    return EffectRemovalResult::Removed;
}
EffectIconMarkerEffect::EffectIconMarkerEffect(int iconId) : CopyableEffect(EffectType::Invalid) {
    setSaveFacingInteger(0, iconId);
}
EffectInstance EffectIconMarkerEffect::saveFacingInstance() const {
    auto result = Effect::saveFacingInstance(); result.serializedType = 67; return result;
}
EffectApplicationResult EffectIconMarkerEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || instance.durationType() == DurationType::Instant)
        return EffectApplicationResult::Rejected;
    creature->addEffectIcon(instance.integerParameter(0));
    return EffectApplicationResult::Retained;
}
EffectRemovalResult EffectIconMarkerEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) creature->removeEffectIcon(instance.integerParameter(0));

    return EffectRemovalResult::Removed;
}
VisualEffectMarkerEffect::VisualEffectMarkerEffect(int visualEffectId) : CopyableEffect(EffectType::Visual) {
    setSaveFacingInteger(0, visualEffectId);
}
EffectApplicationResult VisualEffectMarkerEffect::onApply(Object &object, EffectInstance &instance) {
    auto visual = std::make_shared<VisualEffect>(instance.integerParameter(0), instance.integerParameter(2) != 0, object.services());
    return visual->onApply(object, instance);
}
EffectInstance LimitMovementSpeedEffect::saveFacingInstance() const {
    auto record = Effect::saveFacingInstance();
    record.serializedType = 59;
    return record;
}
EffectApplicationResult LimitMovementSpeedEffect::onApply(Object &object, EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        if (object.plotFlag()) return EffectApplicationResult::Rejected;
        creature->setRunLimited(true);
    }
    return EffectApplicationResult::Retained;
}
EffectRemovalResult LimitMovementSpeedEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->setRunLimited(hasMovementLimitSurvivor(object.effects(), instance.applicationOrder));
    }
    return EffectRemovalResult::Removed;
}

static EffectInstance makeStatePackage(Object &target, const std::shared_ptr<Effect> &effect,
                                      float duration, const std::shared_ptr<Object> &creator) {
    effect->setSaveFacingCreator(creator);
    auto record = effect->saveFacingInstance();
    record.effect = effect;
    record.id = target.game().allocateEffectId();
    record.setDuration(DurationType::Temporary, duration);
    record.subType = static_cast<uint16_t>((record.subType & ~uint16_t(0x18)) | 0x08);
    record.exposed = 1;
    return record;
}
static void applyStateVisual(Object &target, int visual,
                             const std::shared_ptr<Object> &creator) {
    auto effect = std::make_shared<VisualEffectMarkerEffect>(visual);
    effect->setSaveFacingCreator(creator);
    effect->setSubType(0);
    target.applyEffect(effect, DurationType::Instant, 0.0f);
}
StateApplicationResult applyStatePackage(Object &target, CreatureState state, float duration,
                                        const std::shared_ptr<Object> &creator) {
    if (!hasImplementedStateConsumer(state)) return StateApplicationResult::Rejected;
    std::shared_ptr<CreatureStateEffect> root;
    switch (state) {
    case CreatureState::Stun: root = std::make_shared<StunnedEffect>(); break;
    case CreatureState::Paralysis: root = std::make_shared<ParalyzeEffect>(); break;
    case CreatureState::Sleep: root = std::make_shared<SleepEffect>(); break;
    default: root = std::make_shared<CreatureStateEffect>(state); break;
    }
    auto package = makeStatePackage(target, root, duration, creator);
    // On-hit visuals precede the link. OnApplyLink attempts every child,
    // even when a preceding child is rejected by its own admission callback.
    if (state == CreatureState::Sleep) applyStateVisual(target, 94, creator);
    target.applyEffect(package);
    const int first = state == CreatureState::Fear ? 218
        : state == CreatureState::Stun || state == CreatureState::Confusion ? 208
        : state == CreatureState::Paralysis ? 232 : 7;
    target.applyEffect(package.linkedChild(std::make_shared<VisualEffectMarkerEffect>(first)));
    if (state == CreatureState::Paralysis)
        target.applyEffect(package.linkedChild(std::make_shared<VisualEffectMarkerEffect>(82)));
    target.applyEffect(package.linkedChild(std::make_shared<VisualEffectMarkerEffect>(207)));
    const int icon = state == CreatureState::Fear ? 6
        : state == CreatureState::Confusion ? 17 : state == CreatureState::Stun ? 29
        : state == CreatureState::Paralysis ? 15 : 27;
    target.applyEffect(package.linkedChild(std::make_shared<EffectIconMarkerEffect>(icon)));
    return root->result();
}
bool applySlowPackage(Object &target, float duration, const std::shared_ptr<Object> &creator) {
    auto root = std::make_shared<HasteSlowEffect>(false);
    auto package = makeStatePackage(target, root, duration, creator);
    applyStateVisual(target, 95, creator);
    const bool applied = target.applyEffect(package);
    target.applyEffect(package.linkedChild(std::make_shared<VisualEffectMarkerEffect>(207)));
    target.applyEffect(package.linkedChild(std::make_shared<EffectIconMarkerEffect>(28)));
    return applied;
}
} // namespace reone::game
