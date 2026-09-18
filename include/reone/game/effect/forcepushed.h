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

#pragma once

#include "../effect.h"

namespace reone {

namespace game {

class ForcePushStateEffect : public CopyableEffect<ForcePushStateEffect> {
public:
    ForcePushStateEffect() : CopyableEffect(EffectType::ForcePushed) { setSaveFacingInteger(0, 9); }
    EffectInstance saveFacingInstance() const override;
    EffectApplicationResult onApply(Object &, EffectInstance &) override;
    void onUpdate(Object &, const EffectInstance &, float) override;
    EffectRemovalResult onRemove(Object &, const EffectInstance &) override;
};

bool applyForcePushMovement(Object &, const glm::vec3 &centre, bool ignoreDirectLine,
                            EffectInstance &owner);

class ForcePushedEffect : public CopyableEffect<ForcePushedEffect> {
public:
    ForcePushedEffect() :
        CopyableEffect(EffectType::ForcePushed) {
    }

    EffectApplicationResult onApply(Object &object, EffectInstance &) override;
};

} // namespace game

} // namespace reone
