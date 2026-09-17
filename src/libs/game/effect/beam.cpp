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

#include "reone/game/effect/visual.h"
#include "reone/game/effect/beam.h"

namespace reone {

namespace game {

EffectApplicationResult BeamEffect::onApply(Object &object, EffectInstance &instance) {
    auto visual = std::make_shared<VisualEffect>(instance.integerParameter(0), instance.integerParameter(2) != 0, object.services());
    auto child = instance.linkedChild(visual);
    child.setIntegerParameter(1, instance.integerParameter(1));
    child.setIntegerParameter(2, instance.integerParameter(2));
    child.markGeneratedForLoad();
    child.objectParameters = instance.objectParameters;
    child.objectParameterObjects = instance.objectParameterObjects;
    child.subType = static_cast<uint16_t>((child.subType & ~0x18) | 0x08);
    object.applyEffect(std::move(child));
    return EffectApplicationResult::Retained;
}

} // namespace game

} // namespace reone
