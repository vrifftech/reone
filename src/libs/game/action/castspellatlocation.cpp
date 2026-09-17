/*
 * Copyright (c) 2026 The reone project contributors
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

#include "reone/game/action/castspellatlocation.h"
#include "reone/audio/mixer.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/scene/node/model.h"
#include "reone/game/projectiles.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

CastSpellAtLocationAction::CastSpellAtLocationAction(
    Game &game,
    ServicesView &services,
    std::shared_ptr<Spell> spell,
    std::shared_ptr<Location> targetLocation,
    int metaMagic,
    bool cheat,
    ProjectilePathType projectilePathType,
    bool instantSpell) :
    Action(game, services, ActionType::CastSpellAtLocation),
    _spell(std::move(spell)),
    _targetLocation(std::move(targetLocation)),
    _schedule(_spell->conjTime, _spell->castTime, _spell->catchTime),
    _cheat(cheat),
    _projectilePathType(normalizeProjectilePath(projectilePathType)),
    _instantSpell(instantSpell) {
    (void)metaMagic; // The shared script command normalizes this to NONE.
}

void CastSpellAtLocationAction::execute(
    std::shared_ptr<Action> self,
    Object &actor,
    float dt) {

    auto *creature = dyn_cast<Creature>(&actor);
    if (isCompleted() || isCancelled()) return;
    if (_game.combat().isActionPaused(*self)) return;
    if (_restorePresentation) {
        _restorePresentation = false;
        if (_committed) actor.setSpellCastContext(_castContext);
        if (_started && creature) {
            lock();
            if (_ownsMovementRestriction) creature->setMovementRestricted(true);
            const auto &animation = _dispatched ? _spell->catchAnim : _spell->castAnim;
            if (!animation.empty()) {
                creature->playAnimation(animation);
                auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(creature->sceneNode());
                SavedCastAction saved; _schedule.save(saved);
                if (body) body->setAnimationTime(std::max(0.0f, saved.elapsed -
                    (_dispatched ? saved.conjureTime + saved.castTime : 0.0f)));
            }
        }
    }
    if (!_targetLocation || actor.isDead() ||
        (creature && !creature->canCastSpells())) { finish(actor); return; }
    if (!_started) {
        if (!admitSpellCast(actor, *_spell, _cheat)) { finish(actor); return; }
        if (!_cheat && !withinSpellRange(actor, *_spell, _targetLocation->position())) {
            if (!creature || !creature->canMove()) { finish(actor); return; }
            creature->navigateTo(_targetLocation->position(), true, spellRange(actor, *_spell), dt);
            return;
        }
    }
    if (creature) creature->face(_targetLocation->position());
    if (_instantSpell) {
        if (commit(actor)) release(actor);
        finish(actor);
        return;
    }
    const CombatRound *round = creature ? &_game.combat().addAction(self, actor) : nullptr;
    if (round && round->suspends(*self)) return;
    const auto state = round ? _schedule.update(*round, *self, dt) : _schedule.update(true, true, dt);


    switch (state) {
    case SpellSchedule::Conjure: {
        lock();
        _started = true;
        if (creature) creature->beginSpellActivity(static_cast<int>(_spell->type), false);
        if (creature) _game.combat().beginCast(self, *creature,
            _spell->conjTime + _spell->castTime + _spell->catchTime);
        if (creature) {
            _ownsMovementRestriction = !creature->movementLockedByAction();
            creature->setMovementType(Creature::MovementType::None);
            creature->setMovementRestricted(true);
        }

        _presentationId = _services.game.projectiles.beginSpell(actor, nullptr,
            _targetLocation->position(), *_spell, _projectilePathType, _game, _services);
        if (creature && !_spell->castAnim.empty()) {
            creature->playAnimation(_spell->castAnim,
                scene::AnimationProperties::fromFlags(scene::AnimationFlags::blend));
        }
        return;
    }
    case SpellSchedule::Cast: {
        if (!commit(actor)) { finish(actor); return; }
        if (_spell->castSound) {
            _services.audio.mixer.play(
                _spell->castSound,
                audio::AudioType::Sound,
                /*gain=*/1.0f,
                /*loop=*/false,
                actor.position());
        }
        return;
    }
    case SpellSchedule::Effect: {
        release(actor);
        break;
    }
    case SpellSchedule::Finish: {
        finish(actor);
        return;
    }
    default:
        break;
    }
}

bool CastSpellAtLocationAction::commit(Object &actor) {
    if (_committed) return !isCompleted() && !isCancelled();
    if (_commitAttempted || (!_cheat && !withinSpellRange(actor, *_spell, _targetLocation->position()))) return false;
    _commitAttempted = true;
    _committed = commitSpellCast(actor, *_spell, _cheat, _castContext);
    return _committed && !isCompleted() && !isCancelled() && !actor.isDead();
}

void CastSpellAtLocationAction::release(Object &actor) {
    if (_dispatched || !_committed) return;
    const uint32_t projectileMilliseconds = _instantSpell ? 0 : spellProjectileTimeMilliseconds(*_spell,
        actor.position(), _targetLocation->position(), _projectilePathType, _game.isTSL());
    _projectileTime = projectileMilliseconds / 1000.0f;
    _dispatched = queueSpellImpact(_game, *_spell, actor, nullptr, *_targetLocation,
        _castContext, nullptr, projectileMilliseconds);
    if (!_dispatched) { finish(actor); return; }
    _services.game.projectiles.releaseSpell(_presentationId, _projectileTime, _game, _services);
    if (auto *creature = dyn_cast<Creature>(&actor); creature && !_spell->catchAnim.empty())
        creature->playAnimation(_spell->catchAnim);
}

bool CastSpellAtLocationAction::cancel(std::shared_ptr<Action>, Object &actor) {
    if (auto *creature = dyn_cast<Creature>(&actor)) _game.combat().transferEquipment(*creature);
    _services.game.projectiles.cancelSpell(_presentationId);
    finish(actor);
    return true;
}

void CastSpellAtLocationAction::finish(Object &caster) {
    if (_ownsMovementRestriction) {
        if (auto *creature = dyn_cast<Creature>(&caster)) creature->setMovementRestricted(false);
        _ownsMovementRestriction = false;
    }
    _services.game.projectiles.cancelSpell(_presentationId);
    if (auto *creature = dyn_cast<Creature>(&caster)) _game.combat().finishCast(*creature);
    complete();
}


std::optional<SavedActionRecord> CastSpellAtLocationAction::saveFacingState() const {
    SavedActionRecord record = originalSavedAction().value_or(SavedActionRecord {});
    record.actionId = 15; record.parameters.clear(); record.declaredParameterCount = 0;
    SavedCastAction state;
    state.spellId = static_cast<int>(_spell->type);
    state.casterLevel = _castContext.casterLevel; state.forceCost = _castContext.forcePointCost;
    state.path = static_cast<uint8_t>(_projectilePathType);
    state.presentationId = _presentationId; state.projectileTime = _projectileTime;
    _schedule.save(state);
    state.flags = (_cheat ? SavedCastAction::Cheat : 0u) |
        (_instantSpell ? SavedCastAction::Instant : 0u) |
        (_started ? SavedCastAction::Started : 0u) |
        (_commitAttempted ? SavedCastAction::CommitAttempted : 0u) |
        (_committed ? SavedCastAction::Committed : 0u) |
        (_dispatched ? SavedCastAction::Released : 0u) |
        (_ownsMovementRestriction ? SavedCastAction::MovementOwned : 0u) |
        (_itemConsumed ? SavedCastAction::ItemConsumed : 0u);
    state.flags |= SavedCastAction::LocationTarget;
    state.position = _targetLocation->position(); state.facing = _targetLocation->facing();
    record.cast = std::move(state);
    return record;
}
void CastSpellAtLocationAction::restoreCastState(const SavedCastAction &state) {
    _schedule.restore(state);
    _castContext = {state.spellId, state.casterLevel, 0, state.forceCost};
    _presentationId = state.presentationId; _projectileTime = state.projectileTime;
    _started = (state.flags & SavedCastAction::Started) != 0;
    _commitAttempted = (state.flags & SavedCastAction::CommitAttempted) != 0;
    _committed = (state.flags & SavedCastAction::Committed) != 0;
    _dispatched = (state.flags & SavedCastAction::Released) != 0;
    _ownsMovementRestriction = (state.flags & SavedCastAction::MovementOwned) != 0;
    _itemConsumed = (state.flags & SavedCastAction::ItemConsumed) != 0;
    _restorePresentation = true;
}

} // namespace game

} // namespace reone
