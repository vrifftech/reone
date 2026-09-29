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

#include "reone/game/object/camera.h"

#include "reone/game/game.h"
#include "reone/graphics/types.h"
#include "reone/scene/node/camera.h"

namespace reone {

namespace game {

static constexpr float kViewAngleAnimationReset = 45.0f;

void Camera::update(float dt) {
    updateViewAngle(dt);
    auto &options = _game.options().graphics;
    if (_projectionWidth != options.width || _projectionHeight != options.height) {
        rebuildProjection();
    }
}

void Camera::updateViewAngle(float dt) {
    // A pause returns a held view angle at once.
    if (_viewAngleHold && _game.isPaused()) {
        releaseViewAngleHold(_viewAngleHold->id);
        return;
    }
    if (!_viewAngleAnimation) return;
    auto &animation = *_viewAngleAnimation;
    animation.elapsed += dt;
    // Past its end the animation is dropped without a last write.
    if (animation.elapsed > animation.duration) {
        _viewAngleAnimation.reset();
        return;
    }
    const float t = animation.elapsed / animation.duration;
    setViewAngle((1.0f - t) * animation.start + t * animation.target);
}

float Camera::viewAngle() const {
    return _viewAngleOverride.value_or(glm::degrees(projectionFovy()));
}

void Camera::setViewAngle(float degrees) {
    _viewAngleOverride = degrees;
    rebuildProjection();
}

void Camera::beginViewAngleAnimation(float target, float seconds) {
    ViewAngleAnimation animation;
    if (_viewAngleAnimation) {
        setViewAngle(kViewAngleAnimationReset);
        animation.start = kViewAngleAnimationReset;
    } else {
        animation.start = viewAngle();
    }
    animation.duration = seconds;
    animation.target = target;
    _viewAngleAnimation = animation;
}

void Camera::endViewAngleAnimation() {
    _viewAngleAnimation.reset();
}

uint32_t Camera::beginViewAngleHold() {
    ViewAngleHold hold;
    hold.id = _nextViewAngleHold++;
    hold.restoreAngle = viewAngle();
    hold.dolly = setDollyZoom(true);
    _viewAngleHold = hold;
    return hold.id;
}

void Camera::releaseViewAngleHold(uint32_t id) {
    if (!holdsViewAngle(id)) return;
    const auto hold = *_viewAngleHold;
    _viewAngleHold.reset();
    endViewAngleAnimation();
    // The camera's own angle needs no override.
    if (hold.restoreAngle == glm::degrees(projectionFovy())) {
        _viewAngleOverride.reset();
        rebuildProjection();
    } else {
        setViewAngle(hold.restoreAngle);
    }
    if (hold.dolly) setDollyZoom(false);
}

void Camera::rebuildProjection() {
    auto &options = _game.options().graphics;
    float aspect = options.width / static_cast<float>(options.height);
    const float fovy = _viewAngleOverride ? glm::radians(*_viewAngleOverride) : projectionFovy();
    cameraSceneNode()->setPerspectiveProjection(fovy, aspect, graphics::kDefaultClipPlaneNear, graphics::kDefaultClipPlaneFar);
    _projectionWidth = options.width;
    _projectionHeight = options.height;
}

} // namespace game

} // namespace reone
