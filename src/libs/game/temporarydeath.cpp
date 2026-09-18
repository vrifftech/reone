/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/game.h"

#include <algorithm>
#include "reone/system/clock.h"
#include <vector>

#include "reone/game/di/services.h"
#include "reone/game/effect/resurrection.h"

namespace reone::game {

void Game::updateTemporaryDeath() {
    const auto area = _module ? _module->area() : nullptr;
    // Readiness and game-over are common; the second title additionally waits
    // while the GUI manager has a modal panel. Pausing simulation is NOT a gate.
    if (_gameOver || !area || _screen == Screen::Loading || (isTSL() && hasModalPanel())) return;

    std::vector<std::shared_ptr<Creature>> temporary;
    std::shared_ptr<Creature> reference;
    size_t resolved = 0;
    for (const Party::Member &member : _party.members()) {
        if (!member.creature) continue;
        ++resolved;
        reference = member.creature; // last resolved server creature
        if (member.creature->isTemporarilyDead()) temporary.push_back(member.creature);
    }
    if (resolved != 0 && temporary.size() == resolved) runDeathSequence();

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

    for (const auto &creature : temporary) {
        creature->setPosition(area->findPartyPosition(*creature, creature->position(), 5.0f));
        (void)area->landObject(*creature);
        creature->applyEffect(newEffect<ResurrectionEffect>(0), DurationType::Instant);
        // ordering intentionally allows a first refused attempt: this
        // flag is set AFTER ApplyEffect, and the timer permits another pass.
        creature->setRaiseable(true);
    }
    // The non-solo >40-unit relocation pass additionally consumes each
    // member's retained follow-path point. That provider does not exist in this
    // lineage; do not substitute the leader's current position here.
}

bool Game::hasModalPanel() const {
    if (_screen == Screen::InGame) return _confirmPopup && _confirmPopup->isVisible();
    // These exclusive screens own input instead of the world. reone does not
    // yet expose the GUI manager's complete modal stack to scripts.
    switch (_screen) {
    case Screen::InGameMenu: case Screen::Container: case Screen::PartySelection:
    case Screen::SaveLoad: case Screen::PazaakWager: case Screen::PazaakSetup:
    case Screen::PazaakBoard: case Screen::Death: case Screen::MainMenu:
        return true;
    default: return false;
    }
}

void Game::runDeathSequence() {
    if (_gameOver) return;
    _gameOver = true;
    stopMovement();
    setRelativeMouseMode(false);
    if (isTSL()) {
        if (!_deathDisplay) {
            _deathDisplay = std::make_unique<DeathDisplay>(*this, _services);
            _deathDisplay->init();
        }
    } else {
        if (!_deathMessage) {
            _deathMessage = std::make_unique<ConfirmPopup>(*this, _services);
            _deathMessage->init();
            // Queue end-game so the GUI isn't destroyed in its own callback.
        }
        _deathMessage->show(_services.resource.strings.getText(42351), nullptr, [this]() { requestEndGame(); });
    }
    // Replaces the world HUD/input owner. camera/fade and the sequel's
    // retained last-save selector are separate incomplete presentation providers.
    changeScreen(Screen::Death);
}

void Game::dismissDeathSequence(const Creature &creature) {
    // GetClientObjectByObjectId addresses the controlled creature, not every
    // NPC for which a Resurrection operation succeeds.
    const auto controlled = _party.getLeader();
    if (!controlled || controlled.get() != &creature) return;
    if (_deathMessage) _deathMessage->hide();
    _gameOver = false;
    _endGamePending = false;
    if (_screen == Screen::Death) changeScreen(Screen::InGame);
}

} // namespace reone::game
