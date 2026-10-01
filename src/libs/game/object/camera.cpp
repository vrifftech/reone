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

#include <algorithm>

#include "reone/game/game.h"
#include "reone/graphics/types.h"
#include "reone/scene/node/camera.h"

namespace reone {

namespace game {

static constexpr float kViewAngleAnimationReset = 45.0f;

// Combat camera keyboard turning: rate in degrees per second and the
// acceleration and braking that approach it.
static constexpr float kCombatTurnRate = 200.0f;
static constexpr float kCombatTurnAcceleration = 500.0f;
static constexpr float kCombatTurnDeceleration = 2000.0f;
// The longest step an accelerated rate integrates at once.
static constexpr float kAcceleratedRateMaxStep = 0.2f;
// Mouse travel per full-strength frame.
static constexpr float kMouseFullTravel = 100.0f;

// Acceleration for a velocity and an input: the acceleration gain with input,
// the braking gain without, both pulling toward the rate.
static float rateAcceleration(float velocity, float input, float rate, float acceleration, float deceleration) {
    const float gain = input == 0.0f ? deceleration : acceleration;
    return gain * input - (gain / rate) * velocity;
}

// One Runge-Kutta step from rest position: the first stage takes the previous
// frame's input, the middle stages their average and the last this frame's.
// The half-step states are halved sums, (v + a*h)/2.
float stepAcceleratedRate(
    float &velocity, float &previousInput, float input, float dt,
    float rate, float acceleration, float deceleration) {
    const float h = dt < kAcceleratedRateMaxStep ? dt : kAcceleratedRateMaxStep;
    const float mid = (previousInput + input) * 0.5f;
    const float v0 = velocity;
    const float x0 = 0.0f;
    auto accelerate = [&](float v, float u) { return rateAcceleration(v, u, rate, acceleration, deceleration); };

    const float dv1 = accelerate(v0, previousInput) * h;
    const float dx1 = h * v0;
    const float v1 = (v0 + dv1) * 0.5f;
    const float dv2 = accelerate(v1, mid) * h;
    const float dx2 = v1 * h;
    const float v2 = (v0 + dv2) * 0.5f;
    const float dv3 = accelerate(v2, mid) * h;
    const float dx3 = v2 * h;
    const float v3 = v0 + dv3;
    const float dv4 = accelerate(v3, input) * h;
    const float dx4 = h * v3;

    velocity = v0 + (((dv2 * 0.33333334f + dv1 * 0.16666667f) + dv3 * 0.33333334f) + dv4 * 0.16666667f);
    previousInput = input;
    return x0 + (((dx2 * 0.33333334f + dx1 * 0.16666667f) + dx3 * 0.33333334f) + dx4 * 0.16666667f);
}

float stepKeyboardTurn(float &velocity, float &previousInput, float input, float dt) {
    return stepAcceleratedRate(velocity, previousInput, input, dt,
        kCombatTurnRate, kCombatTurnAcceleration, kCombatTurnDeceleration);
}

float mouseFrameStrength(float travel) {
    return std::clamp(-travel / kMouseFullTravel, -1.0f, 1.0f);
}

float mouseSensitivity(uint8_t setting) {
    return static_cast<float>(setting) / 100.0f * 45.0f + 10.0f;
}

bool Camera::isMouseLookMode() const {
    return _mouseLookHeld != _game.mouseLook();
}

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
