/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/game.h"

#include <algorithm>
#include <cmath>
#include "reone/system/clock.h"
#include <vector>

#include "reone/game/di/services.h"
#include "reone/game/effect/resurrection.h"
#include "reone/game/gui/saveload.h"
#include "reone/game/object/camera/thirdperson.h"
#include "reone/resource/strings.h"

namespace reone::game {

// Recovered party members are placed within this radius of where they lie,
// or of their follow point when they are further than kRegroupDistance from
// the leader and the party is not solo.
static constexpr float kRecoveryPlacementRadius = 5.0f;
static constexpr float kRegroupDistance2 = 1600.0f;
// The death fade holds the view for twelve seconds before a one second fade to
// black; hurrying it fades at once over half a second.
static constexpr float kDeathFadeWait = 12.0f;
static constexpr float kDeathFadeLength = 1.0f;
static constexpr float kHurriedFadeLength = 0.5f;
// The first title's slow motion: it reaches kDeathSlowestSpeed after
// kDeathSlowSeconds of real time, easing logarithmically from full speed.
static constexpr float kDeathSlowSeconds = 4.0f;
static constexpr float kDeathSlowestSpeed = 0.2f;
static constexpr double kDeathSlowSpan = 0.8;
// The sequel's gameover panel runs the world at a quarter of its speed.
static constexpr float kDeathPanelTimeScale = 0.25f;
// The first title's death message.
static constexpr int kStrRefDeathMessage = 42351;

void Game::updateTemporaryDeath() {
    const auto area = _module ? _module->area() : nullptr;
    // Readiness and game-over are common; the second title additionally waits
    // while the GUI manager has a modal panel. Pausing simulation is NOT a gate.
    if (_gameOver || !area || _screen == Screen::Loading || (isTSL() && hasModalPanel())) return;

    std::vector<std::shared_ptr<Creature>> temporary;
    std::shared_ptr<Creature> reference;
    bool allDown = true;
    for (const Party::Member &member : _party.members()) {
        // The last member's creature anchors the hostile scan; a member without
        // one keeps the party from counting as fallen.
        reference = member.creature;
        if (!reference) {
            allDown = false;
            continue;
        }
        if (reference->isTemporarilyDead()) {
            temporary.push_back(reference);
        } else {
            allDown = false;
        }
    }
    if (allDown && !_party.members().empty()) runDeathSequence();

    const auto milliseconds = _services.system.clock.millis();
    const bool recover = _temporaryDeathRecovery.sample(
        static_cast<std::uint32_t>(milliseconds), !temporary.empty(), [&]() {
            if (!reference) return false;
            for (const auto &object : area->getObjectsByType(ObjectType::Creature)) {
                const auto creature = dyn_cast<Creature>(object);
                // Dead nonparty candidates are not excluded by the scan.
                if (!creature || _party.isMember(*creature)) continue;
                if (creature->getReputationToward(*reference) > 10) continue;
                if (std::any_of(_party.members().begin(), _party.members().end(),
                        [&](const Party::Member &member) {
                            return member.creature &&
                                   creature->perception().seen.count(member.creature->id()) != 0;
                        })) return true;
            }
            return false;
        });
    if (!recover) return;

    // The leader's position is taken before anyone moves. Every member, fallen
    // or not, that ends up too far from it regroups at its follow point.
    const auto leader = _party.getLeader();
    const glm::vec3 leaderPosition(leader->position());
    const auto &members = _party.members();
    for (size_t slot = 0; slot < members.size(); ++slot) {
        const auto &creature = members[slot].creature;
        if (!creature) continue;
        if (creature->isTemporarilyDead()) {
            const glm::vec3 lying(creature->position());
            creature->setPosition(area->computeSafeLocation(lying, kRecoveryPlacementRadius, *creature, false).value_or(lying));
            area->determineObjectRoom(*creature);
            creature->applyEffect(newEffect<ResurrectionEffect>(0), DurationType::Instant);
            // ordering intentionally allows a first refused attempt: this
            // flag is set AFTER ApplyEffect, and the timer permits another pass.
            creature->setRaiseable(true);
        }
        // Only the follow slots have follow points.
        if (slot >= static_cast<size_t>(kPartyFollowSlots) || _party.isSoloMode()) continue;
        const glm::vec3 offset(creature->position() - leaderPosition);
        if (glm::dot(offset, offset) <= kRegroupDistance2) continue;
        const glm::vec3 followPoint(_party.followPoint(static_cast<int>(slot)));
        creature->setPosition(area->computeSafeLocation(followPoint, kRecoveryPlacementRadius, *creature, false).value_or(followPoint));
        area->determineObjectRoom(*creature);
    }
}

bool Game::hasModalPanel() const {
    if (_screen == Screen::InGame) return _confirmPopup && _confirmPopup->isVisible();
    // These exclusive screens own input instead of the world. The complete
    // modal stack of the GUI manager is not exposed to scripts.
    switch (_screen) {
    case Screen::InGameMenu: case Screen::Container: case Screen::PartySelection:
    case Screen::SaveLoad: case Screen::PazaakWager: case Screen::PazaakSetup:
    case Screen::PazaakBoard: case Screen::Death: case Screen::MainMenu:
        return true;
    default: return false;
    }
}

void Game::setLastPartyMemberTempKilled(const Creature &creature) {
    if (_party.isMember(creature)) _lastPartyMemberTempKilled = creature.id();
}

// The death sequence ends play: panels close, the title's death message comes
// up, the camera starts orbiting the last party member to fall, and the view
// holds before fading to black.
void Game::runDeathSequence() {
    _gameOver = true;
    _deathSequenceStarting = true;
    // Every panel closes, a tutorial window about to open included.
    _tutorialPending = TutorialRequest();
    stopMovement();
    const Camera *view = getActiveCamera();
    const glm::mat4 viewTransform(view ? view->sceneNode()->absoluteTransform() : glm::mat4(1.0f));
    exitFreeLook();
    setRelativeMouseMode(false);
    changeScreen(Screen::Death);
    if (isTSL()) {
        if (!_deathDisplay) {
            _deathDisplay = std::make_unique<DeathDisplay>(*this, _services);
            _deathDisplay->init();
        }
        _deathDisplay->present();
    } else {
        if (!_deathMessage) {
            _deathMessage = std::make_unique<ConfirmPopup>(*this, _services);
            _deathMessage->init();
        }
        // Queue end-game so the GUI isn't destroyed in its own callback.
        _deathMessage->show(_services.resource.strings.getText(kStrRefDeathMessage), nullptr, [this]() { requestEndGame(); });
    }
    const auto area = _module ? _module->area() : nullptr;
    auto *camera = area ? area->getCamera<ThirdPersonCamera>(CameraType::ThirdPerson) : nullptr;
    const auto fallen = getObjectById(_lastPartyMemberTempKilled);
    if (camera && fallen && fallen->type() == ObjectType::Creature) {
        camera->startDeathOrbit(fallen, viewTransform);
        _cameraType = CameraType::ThirdPerson;
    }
    _globalFade.request(GlobalFade::Direction::Out, kDeathFadeWait, kDeathFadeLength, glm::vec3(0.0f));
}

void Game::hurryDeathSequence() {
    _globalFade.request(GlobalFade::Direction::Out, 0.0f, kHurriedFadeLength, glm::vec3(0.0f));
}

// The first title slows the world while its game is over, and ends the game
// once the fade to black has finished or an end was asked for. The sequel's
// gameover panel slows the world for as long as it is up.
void Game::updateDeathSequence() {
    if (isTSL()) {
        _deathTimeScale = _deathDisplay && _deathDisplay->isPresented() ? kDeathPanelTimeScale : 1.0f;
        return;
    }
    const auto now = static_cast<uint32_t>(_services.system.clock.millis());
    const uint32_t elapsed = now - _deathSequenceSample;
    _deathSequenceSample = now;
    if (!_gameOver) return;
    _deathSequenceSeconds += static_cast<float>(elapsed) / 1000.0f;
    if (_deathSequenceStarting) {
        _deathSequenceStarting = false;
        _deathSlowRate = static_cast<float>(
            -std::log(static_cast<double>(_deathSequenceSeconds * (1.0f / kDeathSlowSeconds))) / kDeathSlowSpan);
    }
    _deathTimeScale = _deathSequenceSeconds > kDeathSlowSeconds
                          ? kDeathSlowestSpeed
                          : -std::log(_deathSequenceSeconds * (1.0f / kDeathSlowSeconds)) / _deathSlowRate + kDeathSlowestSpeed;
    if (_globalFade.fading() && !_endGamePending) return;
    _deathTimeScale = 1.0f;
    _gameOver = false;
    _deathSequenceStarting = true;
    _globalFade.stop();
    _endGamePending = true;
    if (_deathMessage) _deathMessage->hide();
}

Game::LastSaveLaunch Game::launchMostRecentSave() {
    _saveLoad->setMode(SaveLoadMode::LoadAfterDeath);
    _saveLoad->refresh();
    if (_saveLoad->savedGameCount() == 0) return LastSaveLaunch::NoSaves;
    return _saveLoad->launchMostRecentSave() ? LastSaveLaunch::Launched : LastSaveLaunch::Unavailable;
}

} // namespace reone::game
