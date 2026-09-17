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

#include <cmath>

#include "reone/game/effect/death.h"

#include "reone/game/di/services.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/hitpointchangewhendying.h"
#include "reone/game/object.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/resource/provider/2das.h"

namespace reone {

namespace game {

EffectApplicationResult DeathEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    if (auto *creature = dyn_cast<Creature>(&object))
        return creature->applyDeathEffect(instance.boundCreator(), instance.integerParameter(2) != 0, &instance)
            ? EffectApplicationResult::Applied : EffectApplicationResult::Rejected;
    return EffectApplicationResult::Rejected;
}

EffectApplicationResult HitPointChangeWhenDyingEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || !creature->isPC()) return EffectApplicationResult::Rejected;
    const float rate = instance.floatParameters[0];
    if (instance.durationType() == DurationType::Instant && rate >= 0.1f) {
        auto effect = std::make_shared<HitPointChangeWhenDyingEffect>(rate);
        auto child = effect->saveFacingInstance();
        child.effect = effect;
        child.subType = 0;
        child.setDuration(DurationType::Temporary, std::fabs(6.0f / rate));
        child.creatorId = instance.creatorId;
        child.creator = instance.creator;
        if (auto creator = instance.boundCreator()) child.spellId = creator->effectSpellId();
        child.restoring = instance.restoring;
        object.applyEffect(std::move(child));
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult HitPointChangeWhenDyingEffect::onRemove(Object &object, const EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    // Remove malformed loaded non-creature records safely after a failed cast.
    if (!creature) return EffectRemovalResult::Removed;
    if (creature->isPC()) {
        const float rate = instance.floatParameters[0];
        const int amount = creature->currentHitPointsWithoutTemporary() + (rate > 0.0f ? 1 : -1);
        creature->Object::setCurrentHitPoints(amount);
        if (!(rate > 0.0f)) {
            auto appearance = object.services().resource.twoDas.get("appearance");
            const auto blood = appearance ? appearance->getString(creature->appearance(), "bloodcolr") : "";
            const int visualId = blood == "R" ? 158 : blood == "G" ? 159 : blood == "Y" ? 160 : 0;
            auto effect = std::make_shared<VisualEffectMarkerEffect>(visualId);
            auto child = effect->saveFacingInstance();
            child.effect = effect;
            child.subType = 0;
            child.setDuration(DurationType::Instant, 0.0f);
            object.applyEffect(std::move(child));
        }
    }
    if (creature->isDead() ||
        (object.game().party().isMember(object) && creature->currentHitPoints() <= 0)) {
        auto effect = std::make_shared<DeathEffect>(false, true, false);
        auto child = effect->saveFacingInstance();
        child.effect = effect;
        child.subType = 0;
        child.setDuration(DurationType::Instant, 0.0f);
        child.creatorId = instance.creatorId;
        child.creator = instance.creator;
        if (auto creator = instance.boundCreator()) child.spellId = creator->effectSpellId();
        object.applyEffect(std::move(child));
    }
    return EffectRemovalResult::Removed;
}

} // namespace game

} // namespace reone
