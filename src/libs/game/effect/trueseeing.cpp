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

#include "reone/game/effect/trueseeing.h"

#include "reone/game/effect/seeinvisible.h"
#include "reone/game/effect/ultravision.h"
#include "reone/game/object/creature.h"
#include "reone/system/cast.h"

#include <algorithm>

namespace reone {

namespace game {

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

    // K2's handler clears True Seeing and sets Ultravision while
    // another True Seeing effect remains. Preserve that observable quirk.
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
    // K1/K2 OnApplyVision constructs and discards a type-30 value without
    // applying it, then returns DELETE_EFFECT, including for non-creatures.
    // This is a consumed operation, not a retained visibility capability.
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

} // namespace game

} // namespace reone
