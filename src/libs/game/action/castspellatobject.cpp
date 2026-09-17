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

#include "reone/game/action/castspellatobject.h"
#include "reone/audio/mixer.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/scene/node/model.h"
#include "reone/game/projectiles.h"
#include "reone/game/object/creature.h"

namespace reone {

namespace game {

CastSpellAtObjectAction::CastSpellAtObjectAction(
    Game &game,
    ServicesView &services,
    std::shared_ptr<Spell> spell,
    std::shared_ptr<Object> target,
    std::optional<std::shared_ptr<Item>> item,
    bool cheat, int metaMagic, int domainLevel,
    ProjectilePathType projectilePathType, bool instantSpell,
    std::optional<size_t> itemProperty, std::optional<int> itemCasterLevel) :
    Action(game, services, ActionType::CastSpellAtObject),
    _spell(std::move(spell)),
    _target(std::move(target)),
    _item(std::move(item)),
    _schedule(_spell->conjTime, _spell->castTime, _spell->catchTime),
    _cheat(cheat),
    _projectilePathType(normalizeProjectilePath(projectilePathType)),
    _instantSpell(instantSpell),
    _itemCasterLevel(itemCasterLevel) {
    (void)metaMagic; // The shared script command normalizes this to NONE.
    (void)domainLevel; // Neither title has domain spell slots.
    requireRuntimeObject(_target);
    if (_item && *_item) {
        requireRuntimeObject(*_item);
        _itemProperty = itemProperty ? itemProperty : (*_item)->spellProperty(_spell->type);
    }
}

void CastSpellAtObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
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
    if (!_target || actor.isDead() || !runtimeDependenciesLive() ||
        (creature && !creature->canCastSpells())) { finish(actor); return; }
    if (!_started) {
        if (!admitSpellCast(actor, *_spell, _cheat || _item.has_value(), _item.has_value()) ||
            !itemAvailable(actor)) { finish(actor); return; }
        if (!_cheat && !withinSpellRange(actor, *_spell, _target->position(), _target.get())) {
            if (!creature || !creature->canMove()) { finish(actor); return; }
            creature->navigateTo(_target->position(), true, spellRange(actor, *_spell, _target.get()), dt);
            return;
        }
    }
    if (creature) creature->face(*_target);
    if (_instantSpell) {
        if (commit(actor)) release(actor);
        finish(actor);
        return;
    }
    const CombatRound *round = creature ? &_game.combat().addAction(self, actor) : nullptr;
    if (round && round->suspends(*self)) return;
    const auto state = round ? _schedule.update(*round, *self, dt) : _schedule.update(true, true, dt);


    // Gameplay updates
    switch (state) {
    case SpellSchedule::Conjure: {
        lock();
        _started = true;
        if (creature) creature->beginSpellActivity(static_cast<int>(_spell->type), _item.has_value());
        if (creature) _game.combat().beginCast(self, *creature,
            _spell->conjTime + _spell->castTime + _spell->catchTime);

        if (creature) {
            _ownsMovementRestriction = !creature->movementLockedByAction();
            creature->setMovementType(Creature::MovementType::None);
            creature->setMovementRestricted(true);
        }

        scene::AnimationProperties animProp =
            scene::AnimationProperties::fromFlags(scene::AnimationFlags::blend);

        _presentationId = _services.game.projectiles.beginSpell(actor, _target.get(),
            _target->position(), *_spell, _projectilePathType, _game, _services);
        if (creature) creature->playAnimation(!_spell->castAnim.empty()
            ? _spell->castAnim : (_spell->itemTargeting ? "activate" : "inject"), animProp);
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

bool CastSpellAtObjectAction::itemAvailable(const Object &actor) const {
    if (!_item) return true;
    if (!*_item || !_itemProperty || (*_item)->owner() != actor.id()) return false;
    const auto &properties = (*_item)->properties();
    return *_itemProperty < properties.size() &&
        properties[*_itemProperty].subtype == static_cast<uint16_t>(_spell->type) &&
        (*_item)->canUseSpell(*_itemProperty);
}

bool CastSpellAtObjectAction::commit(Object &actor) {
    if (_committed) return !isCompleted() && !isCancelled();
    if (_commitAttempted || !itemAvailable(actor) ||
        (!_cheat && !withinSpellRange(actor, *_spell, _target->position(), _target.get()))) return false;
    _commitAttempted = true;
    _committed = commitSpellCast(actor, *_spell, _cheat || _item.has_value(), _castContext, _item.has_value());
    if (_committed && _item && _itemCasterLevel) {
        _castContext.casterLevel = std::max(0, *_itemCasterLevel);
        actor.setSpellCastContext(_castContext);
    }
    return _committed && !isCompleted() && !isCancelled() && !actor.isDead() && runtimeDependenciesLive();
}

void CastSpellAtObjectAction::release(Object &actor) {
    if (_dispatched || !_committed || !itemAvailable(actor) || !runtimeDependenciesLive()) return;
    const uint32_t projectileMilliseconds = _instantSpell ? 0 : spellProjectileTimeMilliseconds(*_spell,
        actor.position(), _target->position(), _projectilePathType, _game.isTSL());
    _projectileTime = projectileMilliseconds / 1000.0f;
    _dispatched = queueSpellImpact(_game, *_spell, actor, _target.get(),
        Location(_target->position(), _target->getFacing()), _castContext,
        _item ? _item->get() : nullptr, projectileMilliseconds);
    if (!_dispatched) { finish(actor); return; }
    _services.game.projectiles.releaseSpell(_presentationId, _projectileTime, _game, _services);
    if (auto *creature = dyn_cast<Creature>(&actor); creature && !_spell->catchAnim.empty())
        creature->playAnimation(_spell->catchAnim);
    if (_item && !_cheat && !_itemConsumed) {
        _itemConsumed = true;
        auto item = *_item;
        if (item->consumeSpellUse(*_itemProperty)) {
            // The event owns the released impact. Retiring its item must not
            // retract that event or cancel the casting animation itself.
            _runtimeDependencies.erase(std::remove_if(_runtimeDependencies.begin(), _runtimeDependencies.end(),
                [&](const auto &ref) { return ref.resolve().get() == item.get(); }), _runtimeDependencies.end());
            _game.queueObjectDestruction(*item, 0.0f);
        }
    }
}

bool CastSpellAtObjectAction::cancel(std::shared_ptr<Action>, Object &actor) {
    if (auto *creature = dyn_cast<Creature>(&actor)) _game.combat().transferEquipment(*creature);
    _services.game.projectiles.cancelSpell(_presentationId);
    finish(actor);
    return true;
}

void CastSpellAtObjectAction::finish(Object &caster) {
    if (_ownsMovementRestriction) {
        if (auto *creature = dyn_cast<Creature>(&caster)) creature->setMovementRestricted(false);
        _ownsMovementRestriction = false;
    }
    _services.game.projectiles.cancelSpell(_presentationId);
    if (auto *creature = dyn_cast<Creature>(&caster)) _game.combat().finishCast(*creature);
    complete();
}


std::optional<SavedActionRecord> CastSpellAtObjectAction::saveFacingState() const {
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
    state.target = SavedObjectReference::fromRuntimeId(_target->id());
    _game.bindSavedObjectReference(state.target);
    state.position = _target->position(); state.facing = _target->getFacing();
    if (_item) {
        state.flags |= SavedCastAction::ItemCast;
        state.item = SavedObjectReference::fromRuntimeId(*_item ? (*_item)->id() : script::kObjectInvalid);
        _game.bindSavedObjectReference(state.item);
        state.itemProperty = _itemProperty ? static_cast<int>(*_itemProperty) : -1;
        state.itemCasterLevel = _itemCasterLevel.value_or(-1);
    }
    record.cast = std::move(state);
    return record;
}
void CastSpellAtObjectAction::restoreCastState(const SavedCastAction &state) {
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
    if (_itemConsumed && _item) {
        _runtimeDependencies.erase(std::remove_if(_runtimeDependencies.begin(), _runtimeDependencies.end(),
            [&](const auto &ref) { return ref.resolve().get() == _item->get(); }), _runtimeDependencies.end());
    }
}

} // namespace game

} // namespace reone
