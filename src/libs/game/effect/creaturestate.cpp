/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/effect/haste.h"
#include "reone/game/effect/beam.h"
#include "reone/game/effect/visual.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/knockdown.h"
#include <cmath>
#include "reone/game/combatfeedback.h"
#include "reone/game/effect/paralyze.h"
#include "reone/game/effect/sleep.h"
#include "reone/game/effect/stunned.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/movementspeeddecrease.h"
#include "reone/game/effect/savingthrowdecrease.h"
#include "reone/game/game.h"
#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/game/combat.h"
#include "reone/game/object/creature.h"
#include "reone/game/effect/blind.h"
#include "reone/game/effect/misschance.h"
#include "reone/game/effect/ultravision.h"
#include "reone/game/effect/entangle.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/invisibility.h"
#include "reone/system/cast.h"
#include "reone/game/effect/trueseeing.h"
#include "reone/game/effect/seeinvisible.h"
#include <algorithm>

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
    case CreatureState::Knockdown: return target.hasEffectImmunity(ImmunityType::Knockdown, creator);
    case CreatureState::MindTrick: return target.hasEffectImmunity(ImmunityType::MindSpells, creator);
    case CreatureState::DroidConfused: return target.hasEffectImmunity(ImmunityType::DroidConfused, creator);
    case CreatureState::DroidScramble: return false;
    case CreatureState::Horrified:
        return target.hasEffectiveFeat(FeatType::ForceImmunityFear) ||
               (target.game().isTSL() && target.hasEffectiveFeat(FeatType::MandalorianCourage));
    case CreatureState::DroidStun:
    case CreatureState::Choke:
    case CreatureState::Whirlwind:
    case CreatureState::Crush:
    case CreatureState::ForcePushed:
    case CreatureState::ForceJumpedOnto:
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
    case CreatureState::DroidConfused:
    case CreatureState::DroidScramble:
    case CreatureState::Paralysis:
    case CreatureState::Knockdown:
    case CreatureState::ForcePushed:
    case CreatureState::ForceJumpedOnto:
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
    case CreatureState::DroidConfused: _type = EffectType::DroidConfused; break;
    case CreatureState::DroidScramble: _type = EffectType::DroidScramble; break;
    case CreatureState::ForcePushed: _type = EffectType::ForcePushed; break;
    case CreatureState::Knockdown:
    case CreatureState::ForceJumpedOnto:
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
    // Sleep, Fear and both confusions retain their direct immunity checks even
    // when package inspection is bypassed. Restoration only changes child apply
    // mode.
    const bool checkStateImmunity = !bypass || _state == CreatureState::Sleep ||
        _state == CreatureState::Fear || _state == CreatureState::Confusion ||
        _state == CreatureState::DroidConfused;
    if (!bypass && creature->isEffectLinkImmune(candidate)) {
        _result = StateApplicationResult::Immune;
        return EffectApplicationResult::Rejected;
    }
    // A state-specific immunity is reported to the creator and the target,
    // whatever applied the state.
    if (checkStateImmunity &&
        hasStateSpecificImmunity(*creature, _state, creator ? dyn_cast<Creature>(creator.get()) : nullptr)) {
        addStateImmunityFeedback(object.game(), object.services(), creator, *creature, _state);
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
    if (auto *creature = dyn_cast<Creature>(&object)) {
        // Losing any state makes the creature commandable again; a state that
        // remains takes that back when it is laid again.
        creature->setCommandable(true);
        creature->rebuildStateEffects(nullptr, instance.applicationOrder);
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
    // Every other state clears the actions even of a creature that cannot be
    // commanded, and leaves it uncommandable; a restored creature keeps the
    // commandable state it was saved with.
    if (state != 9) {
        creature->clearAllActions(true, true);
        object.game().combat().cancelActions(*creature);
        if (!instance.restoring) creature->setCommandable(false);
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
        // Including during load.
        if (stateRemovalRunsEndRoundScript(state, object.game().isTSL())) creature->runEndRoundScript();
        creature->onInternalStateRemoved(instance.id, stateRemovalAppliesPose(state, object.game().isTSL()));
        // A creature other than a player character forgets what it perceived
        // and looks around anew.
        if (!creature->isPC()) creature->perceiveAfresh();
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
    // Only an icon whose row gives its picture and priority is shown; TSL
    // also needs its side and name.
    const int row = instance.integerParameter(0);
    const auto icons = getRequiredTwoDA(object.services().resource.twoDas, "effecticon");
    if (icons->getString(row, "iconresref").empty() || !icons->getIntOpt(row, "priority") ||
        (object.game().isTSL() && (!icons->getIntOpt(row, "good") || !icons->getIntOpt(row, "namestrref"))))
        return EffectApplicationResult::Rejected;
    creature->addEffectIcon(row);
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
EffectInstance WalkAnimationEffect::saveFacingInstance() const {
    auto record = Effect::saveFacingInstance();
    record.serializedType = 58;
    return record;
}
EffectApplicationResult WalkAnimationEffect::onApply(Object &object, EffectInstance &) {
    return isa<Creature>(&object) ? EffectApplicationResult::Retained : EffectApplicationResult::Rejected;
}
EffectInstance LightEffect::saveFacingInstance() const {
    auto record = Effect::saveFacingInstance();
    record.serializedType = 54;
    return record;
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
    // Apply on-hit visuals before the linked package. Attempt every child even
    // when an earlier child rejects its own application.
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

static constexpr float kKnockdownAIHoldSeconds = 1.5f;
static constexpr float kKnockdownFrontFacing = 0.707f;
static constexpr int kKnockdownBackAnimationId = 10005;
static constexpr int kKnockdownFrontAnimationId = 10007;

EffectApplicationResult KnockdownEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    // Anything but a creature keeps an inert record.
    if (!creature) return EffectApplicationResult::Retained;
    auto &game = object.game();
    const auto creator = instance.boundCreator();
    if (creature->hasEffectImmunity(ImmunityType::Knockdown, creator ? dyn_cast<Creature>(creator.get()) : nullptr)) {
        addStateImmunityFeedback(game, object.services(), creator, *creature, CreatureState::Knockdown);
        return EffectApplicationResult::Rejected;
    }
    // A plot creature is not knocked down, nor, in TSL, one already lying in
    // the knocked-down state.
    if (object.plotFlag() ||
        (game.isTSL() && creature->effectState() == static_cast<int>(CreatureState::Knockdown)))
        return EffectApplicationResult::Rejected;
    // The dead and the downed keep the record and nothing more.
    if (creature->isDead() || creature->isTemporarilyDead()) return EffectApplicationResult::Retained;
    game.combat().holdKnockedDown(*creature);
    // The records the knockdown adds belong to it: they end with it, and a
    // restored knockdown adds them again. Its AI is restricted for a second
    // and a half longer than it lasts, which it never outlives.
    auto ai = instance.linkedChild(std::make_shared<CreatureAIStateEffect>(-257));
    ai.markGeneratedForLoad();
    if (!instance.restoring) ai.setDuration(ai.durationType(), instance.duration + kKnockdownAIHoldSeconds);
    object.applyEffect(std::move(ai));
    // A restored creature keeps the command and the pose it was saved with.
    if (!instance.restoring) {
        creature->setCommandable(false);
        // It falls one way when its creator stands in front of it, the other
        // way when the creator is behind it or gone.
        bool front = true;
        if (creator) {
            const glm::vec3 toCreator = glm::normalize(creator->position() - object.position());
            const float facing = object.getFacing();
            const glm::vec3 forward(-std::sin(facing), std::cos(facing), 0.0f);
            front = glm::dot(toCreator, forward) >= kKnockdownFrontFacing;
        }
        creature->showKnockdownFall(front ? kKnockdownFrontAnimationId : kKnockdownBackAnimationId);
    }
    // TSL also lays it in the knocked-down state for as long as it lasts.
    if (game.isTSL()) {
        auto state = instance.linkedChild(std::make_shared<CreatureStateEffect>(CreatureState::Knockdown));
        state.markGeneratedForLoad();
        if (!instance.restoring) state.setDuration(DurationType::Temporary, instance.duration);
        object.applyEffect(std::move(state));
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult KnockdownEffect::onRemove(Object &object, const EffectInstance &) {
    // A creature that is still alive stands ready and takes commands again.
    if (auto *creature = dyn_cast<Creature>(&object);
        creature && !creature->isDead() && !creature->isTemporarilyDead()) {
        creature->setCommandable(true);
        creature->showPauseReadyAnimation(false);
    }
    return EffectRemovalResult::Removed;
}

EffectApplicationResult BlindEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    const int mask = instance.integerParameter(0);
    if (mask != 8 && mask != 16) return EffectApplicationResult::Rejected;
    const auto creator = instance.boundCreator();
    if (mask == 16 && creature->hasEffectImmunity(ImmunityType::Blindness, creator ? dyn_cast<Creature>(creator.get()) : nullptr)) {
        addBlindnessImmunityFeedback(object.game(), object.services(), creator, *creature);
        return EffectApplicationResult::Rejected;
    }
    if (object.plotFlag()) return EffectApplicationResult::Rejected;
    if (mask == 8 && (creature->visibilityCounterBits() & 6) != 0)
        return EffectApplicationResult::Retained;
    // The restored root rebuilds its children, so they are never saved.
    const auto child = [&instance](const std::shared_ptr<Effect> &effect) {
        auto result = instance.linkedChild(effect);
        result.markGeneratedForLoad();
        return result;
    };
    auto miss = child(std::make_shared<MissChanceEffect>(50));
    miss.setIntegerParameter(1, mask == 8 ? 1 : 0);
    object.applyEffect(std::move(miss));
    object.applyEffect(child(std::make_shared<VisionEffect>(4)));
    object.applyEffect(child(std::make_shared<VisualEffectMarkerEffect>(5002)));
    creature->setVisibilityCounter(static_cast<uint8_t>(mask));
    return EffectApplicationResult::Retained;
}

EffectRemovalResult BlindEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object))
        creature->restoreBlindnessCounter(instance.integerParameter(0), instance.applicationOrder);
    return EffectRemovalResult::Removed;
}

EffectApplicationResult EntangleEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;

    auto creator = instance.boundCreator();
    auto *creatorCreature = creator ? dyn_cast<Creature>(creator.get()) : nullptr;
    if (creature->hasEffectImmunity(ImmunityType::Entangle, creatorCreature)) {
        addEntangleImmunityFeedback(object.game(), object.services(), creator, *creature);
        return EffectApplicationResult::Rejected;
    }
    if (object.plotFlag()) return EffectApplicationResult::Rejected;

    creature->clearAllActions(true);

    std::vector<EffectInstance> children;
    auto attack = instance.linkedChild(
        std::make_shared<AttackDecreaseEffect>(2, AttackBonus::Misc));
    // Copy the rules-owned racial selector into the third slot.
    // The stock rules value is 28 for this package.
    attack.setIntegerParameter(2, 28);
    children.push_back(std::move(attack));
    children.push_back(instance.linkedChild(
        std::make_shared<AbilityDecreaseEffect>(Ability::Dexterity, 4)));
    children.push_back(instance.linkedChild(
        std::make_shared<CreatureAIStateEffect>(-3)));
    // The restored root rebuilds its children, so they are never saved.
    for (auto &child : children) child.markGeneratedForLoad();
    object.applyEffectPackage(children);
    return EffectApplicationResult::Retained;
}

EffectRemovalResult EntangleEffect::onRemove(Object &object, const EffectInstance &) {
    if (auto *creature = dyn_cast<Creature>(&object); creature && !creature->isPC()) {
        creature->perceiveAfresh();
    }
    return EffectRemovalResult::Removed;
}

EffectApplicationResult InvisibilityEffect::onApply(
    Object &object,
    EffectInstance &) {

    return EffectApplicationResult::Retained;
}

EffectRemovalResult InvisibilityEffect::onRemove(Object &, const EffectInstance &) {
    return EffectRemovalResult::Removed;
}

ParalyzeEffect::ParalyzeEffect(bool bypassPackageInspection) :
    CreatureStateEffect(CreatureState::Paralysis, bypassPackageInspection) {
}

std::shared_ptr<script::EngineType> ParalyzeEffect::cloneForScript() const {
    return std::make_shared<ParalyzeEffect>(*this);
}

SleepEffect::SleepEffect() :
    CreatureStateEffect(CreatureState::Sleep) {
}

std::shared_ptr<script::EngineType> SleepEffect::cloneForScript() const {
    return std::make_shared<SleepEffect>(*this);
}

StunnedEffect::StunnedEffect(bool bypassPackageInspection) :
    CreatureStateEffect(CreatureState::Stun, bypassPackageInspection) {
}

std::shared_ptr<script::EngineType> StunnedEffect::cloneForScript() const {
    return std::make_shared<StunnedEffect>(*this);
}

namespace {

Creature *effectCreature(Object &object) {
    return dyn_cast<Creature>(&object);
}

size_t firstBlindness(const Object &object) {
    const auto &effects = object.effects();
    return static_cast<size_t>(std::find_if(effects.begin(), effects.end(),
        [](const EffectInstance &effect) { return effect.serializedType >= 73; }) - effects.begin());
}

void removeLinkedBlindness(Object &object, bool removeRoot) {
    // These are live, type-sorted walks, not whole-package removals. True
    // Seeing removes the blindness root; Ultravision removes its siblings.
    for (size_t outer = firstBlindness(object); outer < object.effects().size(); ++outer) {
        const auto root = object.effects()[outer];
        if (root.serializedType > 73) break;
        if (root.serializedType != 73 || root.integerParameter(0) != 8) continue;
        for (size_t inner = 0; inner < object.effects().size();) {
            const auto candidate = object.effects()[inner];
            if (candidate.serializedType > 75) break;
            if (candidate.id != root.id || candidate.applicationOrder == root.applicationOrder) {
                ++inner;
                continue;
            }
            if (removeRoot && outer >= object.effects().size()) break;
            const auto order = removeRoot ? object.effects()[outer].applicationOrder
                                          : candidate.applicationOrder;
            if (!object.removeEffectApplication(order)) break;
            outer = firstBlindness(object);
            // The True Seeing walk advances its inner cursor after
            // removal; Ultravision restarts it. Keep the root identity by
            // value rather than reading a freed effect pointer.
            inner = removeRoot ? inner + 1 : 0;
        }
    }
}

void reapplyBlindness(Object &object) {
    // Keep live index progression: removing and reinserting a
    // record can change which blindness record occupies the next index.
    for (size_t index = firstBlindness(object); index < object.effects().size(); ++index) {
        auto effect = object.effects()[index];
        if (effect.serializedType > 73) break;
        if (effect.serializedType != 73) continue;
        if (!object.removeEffectApplication(effect.applicationOrder)) continue;
        effect.restoring = false;
        object.applyEffect(std::move(effect));
    }
}

void removeVisibilityCounter(
    Object &object,
    const EffectInstance &instance,
    EffectType type,
    uint8_t bit,
    bool trueSeeingRemovalQuirk = false) {

    auto *creature = effectCreature(object);
    if (!creature) {
        return;
    }
    creature->restoreVisibilityCounter(
        type,
        bit,
        instance.applicationOrder,
        trueSeeingRemovalQuirk);
    if (type == EffectType::Ultravision || type == EffectType::TrueSeeing) {
        reapplyBlindness(object);
    }
    creature->refreshVisibilityPerception();
}

} // namespace

EffectApplicationResult TrueSeeingEffect::onApply(Object &object, EffectInstance &) {
    if (auto *creature = effectCreature(object)) {
        creature->setVisibilityCounter(Creature::kTrueSeeingCounter);
        removeLinkedBlindness(object, true);
    }
    // The non-creature path retains an inert effect record too.
    return EffectApplicationResult::Retained;
}

EffectRemovalResult TrueSeeingEffect::onRemove(
    Object &object,
    const EffectInstance &instance) {

    // In TSL, removing True Seeing while another instance remains clears
    // True Seeing and enables Ultravision.
    removeVisibilityCounter(
        object,
        instance,
        EffectType::TrueSeeing,
        Creature::kTrueSeeingCounter,
        true);

    return EffectRemovalResult::Removed;
}

EffectApplicationResult SeeInvisibleEffect::onApply(Object &object, EffectInstance &) {
    if (auto *creature = effectCreature(object)) {
        creature->setVisibilityCounter(Creature::kSeeInvisibleCounter);
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult SeeInvisibleEffect::onRemove(
    Object &object,
    const EffectInstance &instance) {

    removeVisibilityCounter(
        object,
        instance,
        EffectType::SeeInvisible,
        Creature::kSeeInvisibleCounter);

    return EffectRemovalResult::Removed;
}

EffectApplicationResult UltravisionEffect::onApply(Object &object, EffectInstance &instance) {
    if (auto *creature = effectCreature(object)) {
        object.applyEffect(instance.linkedChild(std::make_shared<VisionEffect>(3)));
        creature->setVisibilityCounter(Creature::kUltravisionCounter);
        removeLinkedBlindness(object, false);
    }
    // Perception is refreshed after admission, when the counter's retained
    // record is visible to hasVisibilityCounter().
    return EffectApplicationResult::Retained;
}

EffectInstance VisionEffect::saveFacingInstance() const {
    auto instance = Effect::saveFacingInstance();
    instance.serializedType = 69;
    return instance;
}

EffectApplicationResult VisionEffect::onApply(Object &, EffectInstance &) {
    // Vision is consumed immediately without retaining a visibility effect, including on
    // non-creature targets.
    return EffectApplicationResult::Applied;
}

EffectRemovalResult UltravisionEffect::onRemove(
    Object &object,
    const EffectInstance &instance) {

    removeVisibilityCounter(
        object,
        instance,
        EffectType::Ultravision,
        Creature::kUltravisionCounter);

    return EffectRemovalResult::Removed;
}

} // namespace reone::game
