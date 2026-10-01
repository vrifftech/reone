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
#include "reone/game/action/castspellatlocation.h"
#include "reone/audio/mixer.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/party.h"
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
    std::optional<size_t> itemProperty, std::optional<int> itemCasterLevel,
    std::optional<SpellSelection> selection, int associatedFeat, bool fake) :
    Action(game, services, ActionType::CastSpellAtObject),
    _spell(std::move(spell)),
    _target(std::move(target)),
    _item(std::move(item)),
    _schedule(_spell->conjTime, _spell->castTime, _spell->catchTime),
    _cheat(cheat),
    _projectilePathType(normalizeProjectilePath(projectilePathType)),
    _instantSpell(instantSpell),
    _fake(fake),
    _itemCasterLevel(itemCasterLevel),
    _selection(selection),
    _associatedFeat(associatedFeat) {
    (void)metaMagic; // The shared script command normalizes this to NONE.
    (void)domainLevel; // Neither title has domain spell slots.
    requireRuntimeObject(_target);
    _itemProperty = itemProperty;
    if (_item && *_item) {
        requireRuntimeObject(*_item);
        if (!_itemProperty) _itemProperty = (*_item)->spellProperty(_spell->type);
        _itemType = (*_item)->itemType();
    }
}

void CastSpellAtObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto *creature = dyn_cast<Creature>(&actor);
    if (isCompleted() || isCancelled()) return;
    if (creature) _game.party().setCombatMessage(*creature, _game.party().idleCombatMessage());
    // An item use fails, leaving its user posed, once its item or its spell is
    // gone, or once the item has no use left before it takes effect.
    if (_item && !_dispatched) {
        if (!itemAvailable()) {
            const bool noUseLeft = *_item && (*_item)->isRuntimeLive() && _itemProperty &&
                *_itemProperty < (*_item)->properties().size() && !(*_item)->hasSpellUse(*_itemProperty);
            finish(actor);
            presentFailedItemUse(_game, actor, noUseLeft);
            return;
        }
        const auto propertySpell = static_cast<SpellType>((*_item)->properties()[*_itemProperty].subtype);
        const auto effectiveSpell = itemSpellForActor(actor, propertySpell);
        if (effectiveSpell != _spell->type) {
            const auto definition = _services.game.spells.get(effectiveSpell);
            if (!definition) { finish(actor); presentFailedItemUse(_game, actor); return; }
            _spell = definition;
            // A started item use keeps its item type's times.
            if (!_started) _schedule.setDurations(_spell->conjTime, _spell->castTime, _spell->catchTime);
            if (_committed) _castContext.spellId = static_cast<int>(effectiveSpell);
        }
    }
    // After its release too, as soon as its item is gone, unless the item's
    // use plays on without it.
    if (_item && _dispatched && !(*_item && (*_item)->isRuntimeLive()) && itemUseEndsWithItem(*_itemType)) {
        finish(actor);
        presentFailedItemUse(_game, actor);
        return;
    }
    if (_restorePresentation) {
        _restorePresentation = false;
        if (_started || _committed) {
            auto &context = actor.spellScriptContext();
            if (!context.restored() && _target && runtimeDependenciesLive()) {
                publishSpellContext(actor);
            }
            _ownsSpellTarget = context.activeTarget() != nullptr;
        }
        if (_started && creature) {
            if (_ownsMovementRestriction) creature->setMovementRestricted(true);
            if (_target) _presenter.restore(*creature, *_spell, _schedule);
        }
    }
    // A party member at zero vitality stops casting; a cast that can no longer
    // be cleared runs on through the other states that stop actions. A cast
    // also fails once its target creature is lost, and a power once its
    // caster's equipment no longer allows it. A failed item use leaves its
    // user posed. A released use that plays on without its item no longer
    // looks at its target.
    const bool playsOn = _item && _dispatched && !itemUseEndsWithItem(*_itemType);
    if (!_target || actor.isDead() ||
        (!playsOn && (!runtimeDependenciesLive() || spellTargetLost(actor, *_target))) ||
        (creature && (creature->isTemporarilyDead() ||
                      ((!_started || isClearable()) && !creature->canCastSpells()) ||
                      (!_item && !equipmentAllowsSpell(*creature, *_spell))))) {
        finish(actor);
        if (_item) presentFailedItemUse(_game, actor);
        return;
    }
    // A caster that already acted in a round that has not ended waits for its end.
    if (!_started && creature && _game.combat().awaitsRoundEnd(*creature, self.get())) return;
    // A caster keeps turning toward its spell's target; using an item does not.
    if (creature && !_item) creature->setOrientationLock(_target->id());
    if (creature && _associatedFeat != -1 &&
        creature->featRemainingUses(static_cast<FeatType>(static_cast<uint16_t>(_associatedFeat))) == 0) {
        finish(actor);
        return;
    }
    if (!_started) {
        if (!admitSpellCast(actor, *_spell, _cheat || _fake || _item.has_value(), _item.has_value(), _selection) ||
            !itemAvailable()) { finish(actor); return; }
        if (creature) {
            actor.spellScriptContext().setActiveTarget(_target);
            _ownsSpellTarget = true;
        }
        if (!_cheat && !withinSpellRange(actor, *_spell, _target->position(), _target.get())) {
            if (!creature || !creature->canMove()) { finish(actor); return; }
            creature->navigateTo(_target->position(), true, spellRange(actor, *_spell, _target.get()), dt, _target.get());
            // A target no path reaches fails the cast.
            if (creature->navigationFailed()) finish(actor);
            return;
        }
    }
    // Approach and casting share this action. Refresh the decision counter only after
    // approach completes.
    if (creature) creature->refreshCombatDecisionTimer();
    if (creature) creature->setDesiredFacingToward(_target->position());
    if (_instantSpell) {
        if (creature) creature->beginSpellActivity(static_cast<int>(_spell->type), _item.has_value(), _spell->hostile);
        if (creature) _game.combat().beginInstantCast(self, *creature, _target.get());
        // An instant cast shows its release as any cast does.
        _presentationId = _services.game.projectiles.beginSpell(actor, _target.get(),
            _target->position(), *_spell, _projectilePathType, _game, _services);
        if (commit(actor)) release(actor);
        finish(actor);
        return;
    }
    const CombatRound *round = creature ? &_game.combat().addAction(self, actor) : nullptr;
    if (!_started && round && round->suspends(*self)) return;
    const auto state = round ? _schedule.update(*round, *self, dt) : _schedule.update(true, dt);
    auto *targetCreature = dyn_cast<Creature>(_target.get());
    // What the cast shows this frame; the conjure waits until the caster stands.
    if (creature && state != SpellSchedule::Conjure)
        _presenter.update(*creature, *_spell, _itemType, targetCreature,
            _target->position(), _schedule, state);
    if (creature && !_item && _committed && isClearable() && state > SpellSchedule::Cast &&
        !continuePaidCast(*creature, *self)) { finish(actor); return; }

    // Gameplay updates
    switch (state) {
    case SpellSchedule::Conjure: {
        publishSpellContext(actor);
        _started = true;
        // An item use cannot be cleared once it has started.
        if (_item) setClearable(false);
        debug(str(boost::format("Spell started: actor=%u spell=%d")
            % actor.id() % static_cast<int>(_spell->type)), LogChannel::Combat);
        if (creature) creature->beginSpellActivity(static_cast<int>(_spell->type), _item.has_value(), _spell->hostile, !_fake);
        if (creature) {
            float pause = _spell->conjTime + _spell->castTime + _spell->catchTime;
            if (_item) {
                // An item use takes effect and ends at its item type's times.
                const auto &use = _presenter.itemUse(*creature, *_spell, *_itemType, targetCreature, _target->position());
                _schedule.setDurations(use.impact, std::max(0.0f, use.end - use.impact), 0.0f);
                pause = use.end;
            }
            // An item use holds a round it starts for as long as it lasts, without
            // holding the partner; a cast holds both for all three times.
            _game.combat().beginCast(self, *creature, pause, _item.has_value(), _fake);
        }

        if (creature) {
            _ownsMovementRestriction = !creature->movementLockedByAction();
            creature->setMovementType(Creature::MovementType::None);
            creature->setMovementRestricted(true);
        }

        _services.game.projectiles.cancelSpell(_presentationId);
        _presentationId = _services.game.projectiles.beginSpell(actor, _target.get(),
            _target->position(), *_spell, _projectilePathType, _game, _services);
        if (creature && !_item) creature->spellCastVisuals().showConjure(*creature, *_spell);
        if (creature)
            _presenter.update(*creature, *_spell, _itemType, targetCreature,
                _target->position(), _schedule, state);
        return;
    }
    case SpellSchedule::Cast: {
        if (!commit(actor)) { finish(actor); return; }
        if (creature && !_item && !continuePaidCast(*creature, *self)) { finish(actor); return; }
        // The impact goes out with the payment; only its projectile delays it.
        release(actor);
        return;
    }
    case SpellSchedule::Finish: {
        // A caught cast ends as a failure: it spends no feat use and keeps its
        // orientation lock and cast clip until the round pause poses its caster.
        const bool caught = !_item && _spell->catchTime > 0.0f;
        if (!caught) {
            // Only normal cast completion owns the associated-feat debit. Neither
            // talent selection, release nor cancellation charges this parameter.
            if (creature && _associatedFeat != -1)
                creature->spendFeatUse(static_cast<FeatType>(static_cast<uint16_t>(_associatedFeat)));
            // A completed cast releases the caster's orientation lock.
            if (creature && !_item) creature->setOrientationLock(script::kObjectInvalid);
        }
        finish(actor, caught);
        return;
    }
    default:
        break;
    }
}

// An item use needs its item, wherever it now is, and its property with a
// use left.
bool CastSpellAtObjectAction::itemAvailable() const {
    if (!_item) return true;
    return *_item && (*_item)->isRuntimeLive() && _itemProperty &&
        *_itemProperty < (*_item)->properties().size() && (*_item)->hasSpellUse(*_itemProperty);
}

std::shared_ptr<Action> CastSpellAtObjectAction::toLocationUse() const {
    auto use = _game.newAction<CastSpellAtLocationAction>(_spell, std::make_shared<Location>(_commandLocation, 0.0f),
        0, _cheat, _projectilePathType, _instantSpell, _item, _itemProperty, _itemCasterLevel, _selection,
        _associatedFeat, _fake);
    use->setUserAction(isUserAction());
    return use;
}

void CastSpellAtObjectAction::publishSpellContext(Object &actor) {
    const auto *creature = dyn_cast<Creature>(&actor);
    int level = _itemCasterLevel.value_or(creature
        ? creature->spellCasterLevel(*_spell, _item.has_value()) : 0);
    int castingClass = creature && !_cheat && !_item ? creature->spellCastingClass(*_spell) : kUnselectedCastingClass;
    if (_selection && !_item) {
        castingClass = _selection->classIndex;
        if (_selection->hasExplicitLevel()) level = _selection->casterLevel;
        else if (creature && _selection->sourceKind() == CastingSourceKind::Class) level = creature->adjustedClassLevel(castingClass);
        else if (creature && _selection->sourceKind() == CastingSourceKind::SpellLikeAbility) level = creature->spellLikeAbilityCasterLevel(static_cast<int>(_spell->type));
    }
    const SpellCastContext context = _committed ? _castContext : SpellCastContext {
        static_cast<int>(_spell->type), std::max(0, level), 255, 0,
        castingClass};
    // The spell's target location is the command's, facing as the caster does.
    const Location location(_commandLocation, scriptFacingFromObject(actor.getFacing()));
    actor.spellScriptContext().setImpact(context, _target, _item ? *_item : nullptr, &location);
    if (creature) actor.spellScriptContext().setActiveTarget(_target);
}

bool CastSpellAtObjectAction::commit(Object &actor) {
    if (_committed) return !isCompleted() && !isCancelled();
    // Once the cast has started, the target may have stepped away: the cast
    // goes on.
    if (_commitAttempted || !itemAvailable() ||
        (!_cheat && !_started && !withinSpellRange(actor, *_spell, _target->position(), _target.get()))) return false;
    _commitAttempted = true;
    // A fake cast spends nothing and announces nothing.
    if (_fake) {
        _committed = true;
        return !isCompleted() && !isCancelled() && !actor.isDead();
    }
    _committed = commitSpellCast(actor, *_spell, _cheat || _item.has_value(), _castContext, _item.has_value(), _selection);
    if (_committed && _item && _itemCasterLevel) {
        _castContext.casterLevel = *_itemCasterLevel;
        actor.setSpellCastContext(_castContext);
    }
    if (_committed) publishSpellContext(actor);
    return _committed && !isCompleted() && !isCancelled() && !actor.isDead() && runtimeDependenciesLive();
}

void CastSpellAtObjectAction::release(Object &actor) {
    if (_dispatched || !_committed || !itemAvailable() || !runtimeDependenciesLive()) return;
    if (_item) {
        if (auto *creature = dyn_cast<Creature>(&actor)) publishItemCastLevel(*creature);
    }
    // An instant cast's projectile flies, and its impact waits, for as long as
    // the caster's last measured flight.
    if (!_instantSpell)
        actor.setLastSpellProjectileMilliseconds(spellProjectileTimeMilliseconds(*_spell,
            actor.position(), _target->position(), _projectilePathType, _game.isTSL()));
    const uint32_t projectileMilliseconds = actor.lastSpellProjectileMilliseconds();
    _projectileTime = projectileMilliseconds / 1000.0f;
    // A fake cast shows the projectile and has no impact.
    _dispatched = _fake || queueSpellImpact(_game, *_spell, actor, _target.get(),
        Location(_commandLocation, scriptFacingFromObject(actor.getFacing())), _castContext,
        _item ? _item->get() : nullptr, projectileMilliseconds);
    if (!_dispatched) { finish(actor); return; }
    _services.game.projectiles.releaseSpell(_presentationId, _projectileTime, _game, _services);
    presentSpellRelease(actor, *_spell);
    if (_item && !_cheat && !_itemConsumed) {
        _itemConsumed = true;
        auto item = *_item;
        if (item->consumeSpellUse(*_itemProperty)) {
            // The event owns the released impact; retiring its item must not
            // retract that event. The use itself ends once the item is gone.
            _runtimeDependencies.erase(std::remove_if(_runtimeDependencies.begin(), _runtimeDependencies.end(),
                [&](const auto &ref) { return ref.resolve().get() == item.get(); }), _runtimeDependencies.end());
            _game.queueObjectDestruction(*item, 0.0f);
        }
    }
}

bool CastSpellAtObjectAction::cancel(std::shared_ptr<Action>, Object &actor) {
    auto *creature = dyn_cast<Creature>(&actor);
    if (creature) _game.combat().transferEquipment(*creature);
    _services.game.projectiles.cancelSpell(_presentationId);
    finish(actor);
    // A cleared cast leaves its caster in the pause or ready pose.
    if (creature && !creature->isDead() && !creature->isTemporarilyDead()) creature->showPauseReadyAnimation(false);
    return true;
}

void CastSpellAtObjectAction::dropPresentation() {
    _services.game.projectiles.cancelSpell(_presentationId);
}

void CastSpellAtObjectAction::finish(Object &caster, bool keepCastHold) {
    if (!isCompleted()) {
        debug(str(boost::format("Spell finished: actor=%u spell=%d committed=%d released=%d")
            % caster.id() % static_cast<int>(_spell->type) % _committed % _dispatched), LogChannel::Combat);
    }
    if (_ownsSpellTarget) {
        caster.spellScriptContext().clearActiveTarget();
        _ownsSpellTarget = false;
    }
    if (_ownsMovementRestriction) {
        if (auto *creature = dyn_cast<Creature>(&caster)) creature->setMovementRestricted(false);
        _ownsMovementRestriction = false;
    }
    _services.game.projectiles.cancelSpell(_presentationId);
    if (auto *creature = dyn_cast<Creature>(&caster); creature && !keepCastHold) creature->releaseCastAnimation();
    // A spell that ends before its release takes its conjure visuals with it.
    if (auto *creature = dyn_cast<Creature>(&caster); creature && _started && !_dispatched && !_item && !isCompleted())
        creature->spellCastVisuals().clear();
    complete();
}

// A paid spell stays clearable only while its round is engaged, and a real
// cast fails once its caster is entangled.
bool CastSpellAtObjectAction::continuePaidCast(Creature &caster, const Action &queued) {
    setClearable(_game.combat().isRoundEngaged(queued));
    return _fake || !interruptEntangledCast(caster);
}


std::optional<SavedActionRecord> CastSpellAtObjectAction::saveFacingState() const {
    SavedActionRecord record = originalSavedAction().value_or(SavedActionRecord {});
    record.actionId = _item ? 46 : 15; record.parameters.clear(); record.declaredParameterCount = 0;
    SavedCastAction state;
    state.spellId = static_cast<int>(_spell->type);
    state.casterLevel = _castContext.casterLevel; state.forceCost = _castContext.forcePointCost;
    state.metaMagic = _castContext.metaMagic;
    state.castingClass = _castContext.castingClass;
    state.associatedFeat = _associatedFeat;
    state.selectedClass = _selection ? _selection->classIndex : -1;
    state.selectedLevel = _selection ? _selection->casterLevel : kUnspecifiedCasterLevel;
    state.path = static_cast<uint8_t>(_projectilePathType);
    state.presentationId = _presentationId; state.projectileTime = _projectileTime;
    _schedule.save(state);
    state.flags = (_cheat ? SavedCastAction::Cheat : 0u) |
        (_instantSpell ? SavedCastAction::Instant : 0u) |
        (_fake ? SavedCastAction::Fake : 0u) |
        (_started ? SavedCastAction::Started : 0u) |
        (_commitAttempted ? SavedCastAction::CommitAttempted : 0u) |
        (_committed ? SavedCastAction::Committed : 0u) |
        (_dispatched ? SavedCastAction::Released : 0u) |
        (_ownsMovementRestriction ? SavedCastAction::MovementOwned : 0u);
    state.target = SavedObjectReference::fromRuntimeId(_target->id());
    _game.bindSavedObjectReference(state.target);
    state.position = _commandLocation; state.facing = _target->getFacing();
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
void CastSpellAtObjectAction::restartItemUse(const SavedCastAction &state) {
    assert(_item);
    _commandLocation = state.position;
    _presentationId = state.presentationId;
}

void CastSpellAtObjectAction::restoreCastState(const SavedCastAction &state) {
    // An item use starts over after a load instead.
    assert(!_item);
    _schedule.restore(state);
    _associatedFeat = state.associatedFeat;
    _selection = state.selectedClass == -1 ? std::nullopt
        : std::optional<SpellSelection> {SpellSelection {state.selectedClass, state.selectedLevel}};
    _castContext = {state.spellId, state.casterLevel, state.metaMagic, state.forceCost, state.castingClass};
    _presentationId = state.presentationId; _projectileTime = state.projectileTime;
    _started = (state.flags & SavedCastAction::Started) != 0;
    _commitAttempted = (state.flags & SavedCastAction::CommitAttempted) != 0;
    _committed = (state.flags & SavedCastAction::Committed) != 0;
    _dispatched = (state.flags & SavedCastAction::Released) != 0;
    _ownsMovementRestriction = (state.flags & SavedCastAction::MovementOwned) != 0;
    _commandLocation = state.position;
    _restorePresentation = true;
    // A paid spell derives its clearability again on its next frame.
}

} // namespace game

} // namespace reone
