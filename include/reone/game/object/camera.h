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

#include <optional>

#include "reone/input/event.h"
#include "reone/resource/parser/gff/git.h"

#include "../object.h"

namespace reone {

namespace game {

/**
 * One Runge-Kutta step of a rate that approaches \p rate times the input: it
 * accelerates with \p acceleration under an input and brakes with
 * \p deceleration without one. \p velocity and \p previousInput carry across
 * frames; the step is at most 0.2 s. Returns the distance moved this step.
 */
float stepAcceleratedRate(
    float &velocity, float &previousInput, float input, float dt,
    float rate, float acceleration, float deceleration);

/**
 * One step of a keyboard turn that approaches 200 degrees per second under a
 * held input and brakes without one. \p velocity and \p previousInput carry
 * across frames. Returns the degrees turned.
 */
float stepKeyboardTurn(float &velocity, float &previousInput, float input, float dt);

/**
 * Mouse travel gathered over a frame as a strength from -1 to 1: a hundred
 * counts is full strength, and rightward or downward travel reads negative.
 */
float mouseFrameStrength(float travel);

/** The degrees a full-strength frame of mouse travel is worth at a sensitivity setting. */
float mouseSensitivity(uint8_t setting);

class Camera : public Object {
public:
    Camera(
        uint32_t id,
        std::string sceneName,
        Game &game,
        ServicesView &services) :
        Object(
            id,
            ObjectType::Camera,
            std::move(sceneName),
            game,
            services) {
    }

    static bool classof(const Object *from) {
        return from->type() == ObjectType::Camera;
    }

    virtual bool handle(const input::Event &event) {
        return false;
    }

    void update(float dt) override;

    virtual void stopMovement() {
    }

    inline std::shared_ptr<scene::CameraSceneNode> cameraSceneNode() const {
        return std::static_pointer_cast<scene::CameraSceneNode>(_sceneNode);
    }

    int cameraId() const { return _cameraId; }
    float fieldOfView() const { return _fieldOfView; }

    /** The vertical view angle in degrees. */
    float viewAngle() const;
    void setViewAngle(float degrees);
    /**
     * Moves the view angle linearly to \p target over \p seconds. A running
     * animation is dropped and the new one starts from 45 degrees.
     */
    void beginViewAngleAnimation(float target, float seconds);
    void endViewAngleAnimation();
    /** Keeps the subject the same size on screen while the view angle moves. */
    virtual bool setDollyZoom(bool enabled) { return false; }

    /**
     * A visual program that animates the view angle holds the angle and the
     * dolly zoom to return to. Releasing the hold returns them; a pause
     * releases it at once. Returns the hold's id.
     */
    uint32_t beginViewAngleHold();
    bool holdsViewAngle(uint32_t hold) const { return _viewAngleHold && _viewAngleHold->id == hold; }
    float heldViewAngle() const { return _viewAngleHold ? _viewAngleHold->restoreAngle : viewAngle(); }
    void releaseViewAngleHold(uint32_t hold);

    float facing() const { return _facing; }
    /**
     * Whether the mouse turns the camera: while the right button or a Ctrl key
     * is held, or, with the Mouse Look option, while neither is.
     */
    bool isMouseLookMode() const;

protected:
    int _cameraId {0};
    float _fieldOfView {0.0f};

    float _facing {0.0f};
    bool _mouseLookHeld {false};

    int _projectionWidth {-1};
    int _projectionHeight {-1};

    struct ViewAngleAnimation {
        float duration {0.0f};
        float elapsed {0.0f};
        float start {0.0f};
        float target {0.0f};
    };
    struct ViewAngleHold {
        uint32_t id {0};
        float restoreAngle {0.0f};
        bool dolly {false};
    };
    std::optional<float> _viewAngleOverride;
    std::optional<ViewAngleAnimation> _viewAngleAnimation;
    std::optional<ViewAngleHold> _viewAngleHold;
    uint32_t _nextViewAngleHold {1};

    void rebuildProjection();
    void updateViewAngle(float dt);
    virtual float projectionFovy() const = 0;
};

} // namespace game

} // namespace reone
