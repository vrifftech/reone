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

#include "reone/game/effect/regenerate.h"
#include "reone/game/effect/heal.h"
#include "reone/game/forcerules.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"

namespace reone::game {

EffectApplicationResult RegenerateEffect::onApply(Object &object, EffectInstance &instance) {
    if (dyn_cast<Creature>(&object)) {
        const auto now = object.game().worldTimeMilliseconds();
        const auto day = static_cast<uint32_t>(object.game().millisecondsPerWorldDay());
        // Reset the stamp on every application, including restoration.
        instance.setIntegerParameter(2, static_cast<int32_t>(now / day));
        instance.setIntegerParameter(3, static_cast<int32_t>(now % day));
    }
    return EffectApplicationResult::Retained;
}

void RegenerateEffect::onUpdate(Object &object, const EffectInstance &instance, float) {
    auto *creature = dyn_cast<Creature>(&object);
    auto &game = object.game();
    if (object.isDead()) return;
    if (creature && (game.isTSL() ? creature->isPC() && creature->currentHitPoints() <= 0
                                   : creature->isTemporarilyDead())) return;
    const int selector = instance.integerParameter(4);
    if (creature && selector == 54) {
        if (creature->currentForceWithoutTemporary() >= narrowSignedResource(creature->maxForcePoints())) return;
    } else if (object.currentHitPointsWithoutTemporary() >= narrowSignedResource(object.maxHitPoints())) {
        return;
    }

    const auto now = game.worldTimeMilliseconds();
    const auto day = static_cast<uint32_t>(game.millisecondsPerWorldDay());
    const auto nowDay = static_cast<uint32_t>(now / day);
    const auto nowTime = static_cast<uint32_t>(now % day);
    const auto previousDay = static_cast<uint32_t>(instance.integerParameter(2));
    const auto previousTime = static_cast<uint32_t>(instance.integerParameter(3));
    // SubtractWorldTimes returns a day count and a time remainder; the
    // regeneration branch compares only the remainder, strictly greater than
    // integer parameter 1. A backwards valid stamp has no elapsed interval.
    if (previousTime < day && (nowDay < previousDay ||
        (nowDay == previousDay && nowTime < previousTime))) return;
    uint32_t elapsed = nowTime - previousTime;
    if (elapsed >= day) elapsed += day;
    if (elapsed <= static_cast<uint32_t>(instance.integerParameter(1))) return;

    auto heal = std::make_shared<HealEffect>(instance.integerParameter(0));
    auto child = heal->saveFacingInstance();
    child.effect = heal;
    child.subType = 0;
    child.setDuration(DurationType::Instant, 0.0f);
    child.setIntegerParameter(1, selector);
    if (game.isTSL()) child.setIntegerParameter(2, 1);
    child.creatorId = instance.creatorId;
    child.creator = instance.creator;
    // Derive the spell context from the resolved creator,
    // not from the parent regeneration record.
    if (auto creator = instance.boundCreator()) child.spellId = creator->effectSpellId();
    object.applyEffect(std::move(child));
    // The operation can mutate the collection. Reacquire the exact root and
    // commit the stamp after submission; never catch up with multiple heals.
    if (auto *record = object.findEffectApplication(instance.applicationOrder)) {
        record->setIntegerParameter(2, static_cast<int32_t>(nowDay));
        record->setIntegerParameter(3, static_cast<int32_t>(nowTime));
    }
}

} // namespace reone::game
