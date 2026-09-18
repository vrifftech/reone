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

#include "reone/game/effect/forcepushed.h"
#include "reone/game/effect/forcepushtargeted.h"

namespace reone {

namespace game {

EffectApplicationResult ForcePushTargetedEffect::onApply(Object &object, EffectInstance &record) {
    if (record.boundCreator()) {
        const glm::vec3 centre(record.floatParameters[0], record.floatParameters[1], record.floatParameters[2]);
        applyForcePushMovement(object, centre, record.integerParameter(1) != 0, record);
    }
    return EffectApplicationResult::Retained;
}

} // namespace game

} // namespace reone
