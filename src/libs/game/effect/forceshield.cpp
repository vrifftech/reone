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

#include "reone/game/effect/forceshield.h"
#include "reone/game/forceshieldrules.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/damageresistance.h"
#include "reone/game/object/creature.h"
#include "reone/game/game.h"
#include "reone/game/twodautil.h"
#include "reone/game/di/services.h"
#include "reone/resource/2da.h"
#include "reone/resource/provider/2das.h"

namespace reone::game {
EffectApplicationResult ForceShieldEffect::onApply(Object &object, EffectInstance &record) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Retained;
    if (creature->sceneNode()) {
        if (auto previous = replacedForceShield(object.effects())) object.removeEffectsById(*previous);
    }
    auto table = getRequiredTwoDA(object.services().resource.twoDas, "forceshields");
    const auto shield = readForceShieldDefinition(*table, record.integerParameter(0), creature->appearance());
    auto visual = record.linkedChild(std::make_shared<VisualEffectMarkerEffect>(shield.visual));
    visual.markGeneratedForLoad();
    auto target = object.game().getObjectById(object.id());
    visual.creator = target;
    visual.creatorId = object.id();
    visual.spellId = object.effectSpellId();
    visual.restoring = false;
    auto protection = record.linkedChild(std::make_shared<DamageResistanceEffect>(
        static_cast<DamageType>(shield.damageFlags), shield.resistance, shield.amount, shield.vulnerabilities));
    if (object.game().isTSL()) protection.setIntegerParameter(4, 1);
    protection.markGeneratedForLoad();
    protection.creator = target;
    protection.creatorId = object.id();
    protection.spellId = object.effectSpellId();
    protection.restoring = false;
    protection.subType = (protection.subType & ~uint16_t(0x18)) | 0x08;
    object.applyEffect(visual);
    object.applyEffect(protection);
    return EffectApplicationResult::Retained;
}
} // namespace reone::game
