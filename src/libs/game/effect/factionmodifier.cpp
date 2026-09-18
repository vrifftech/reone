/*
 * Copyright (c) 2020-2023 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "reone/game/effect/factionmodifier.h"

#include <optional>

#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/object/trigger.h"
#include "reone/game/reputes.h"

namespace reone::game {
namespace {
std::optional<Faction> factionOf(Object &object) {
    if (auto *v = dyn_cast<Creature>(&object)) return v->faction();
    if (auto *v = dyn_cast<Door>(&object)) return v->faction();
    if (auto *v = dyn_cast<Placeable>(&object)) return v->faction();
    if (auto *v = dyn_cast<Trigger>(&object)) return v->faction();
    return std::nullopt;
}
void setFactionOf(Object &object, Faction faction) {
    if (auto *v = dyn_cast<Creature>(&object)) v->setFaction(faction);
    else if (auto *v = dyn_cast<Door>(&object)) v->setFaction(faction);
    else if (auto *v = dyn_cast<Placeable>(&object)) v->setFaction(faction);
    else if (auto *v = dyn_cast<Trigger>(&object)) v->setFaction(faction);
}
bool isNpcFaction(const Object &object, int faction) {
    if (faction <= static_cast<int>(Faction::Player)) return false;
    return static_cast<size_t>(faction) < object.services().game.reputes.state().factions.size();
}
}

EffectApplicationResult FactionModifierEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Retained;
    const auto oldFaction = factionOf(object);
    if (!oldFaction || !isNpcFaction(object, _newFaction)) return EffectApplicationResult::Rejected;
    if (auto *creature = dyn_cast<Creature>(&object); creature && creature->isPC()) {
        return EffectApplicationResult::Rejected;
    }

    instance.setIntegerParameter(1, static_cast<int>(*oldFaction));
    setFactionOf(object, static_cast<Faction>(_newFaction));
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->clearAllActions(true);
        if (!creature->isDead() && !creature->isTemporarilyDead()) {
            creature->refreshVisibilityPerception();
        }
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult FactionModifierEffect::onRemove(Object &object, const EffectInstance &instance) {
    switch (instance.spellId) {
    case 184: object.game().setGlobalNumber("000_Beast_Conf_Active", 0); break;
    case 200: object.game().setGlobalNumber("000_Human_Conf_Active", 0); break;
    case 269: object.game().setGlobalNumber("000_Droid_Conf_Active", 0); break;
    default: break;
    }
    setFactionOf(object, static_cast<Faction>(instance.integerParameter(1)));
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->refreshVisibilityPerception();
    }
    return EffectRemovalResult::Removed;
}

} // namespace reone::game
