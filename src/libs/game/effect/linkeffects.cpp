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

#include "reone/game/effect/linkeffects.h"
#include "reone/game/object.h"

namespace reone {

namespace game {

void LinkEffectsEffect::setSubType(uint16_t category) {
    Effect::setSubType(category);
    if (_childEffect) _childEffect->setSubType(category);
    if (_parentEffect) _parentEffect->setSubType(category);
}

EffectApplicationResult LinkEffectsEffect::onApply(Object &object, EffectInstance &instance) {
    // These are the first and second VM arguments. Keep the call order;
    // admission is nontransactional and never rolls back an earlier member.
    std::vector<EffectInstance> members;
    if (_childEffect) members.push_back(instance.linkedChild(_childEffect));
    if (_parentEffect) members.push_back(instance.linkedChild(_parentEffect));
    object.applyEffectPackage(members);
    return EffectApplicationResult::Applied;
}

void LinkEffectsEffect::retireAreaRuntime(
    const std::set<const Object *> &retainedObjects) {
    if (_childEffect) _childEffect->retireAreaRuntime(retainedObjects);
    if (_parentEffect) _parentEffect->retireAreaRuntime(retainedObjects);
}

} // namespace game

} // namespace reone
