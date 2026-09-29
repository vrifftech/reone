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

#include "../action.h"
#include "../castspell.h"
#include "../location.h"

namespace reone {

namespace game {

struct Spell;
class Item;
class CastSpellAtLocationAction : public Action {
public:
    CastSpellAtLocationAction(Game &game, ServicesView &services,
        std::shared_ptr<Spell> spell, std::shared_ptr<Location> targetLocation,
        int metaMagic, bool cheat, ProjectilePathType projectilePathType, bool instantSpell,
        std::optional<std::shared_ptr<Item>> item = std::nullopt,
        std::optional<size_t> itemProperty = std::nullopt,
        std::optional<int> itemCasterLevel = std::nullopt,
        std::optional<SpellSelection> selection = std::nullopt,
        int associatedFeat = -1,
        bool fake = false);
    static bool classof(Action *from) { return from->type() == ActionType::CastSpellAtLocation; }
    void execute(std::shared_ptr<Action> self, Object &actor, float dt) override;
    bool cancel(std::shared_ptr<Action> self, Object &actor) override;
    std::optional<SavedActionRecord> saveFacingState() const override;
    void restoreCastState(const SavedCastAction &state);
    // An instant cast starts an ordinary round, which runs its end-of-round script.
    bool suppressesEndRoundScript() const override { return (_fake && !_instantSpell) || _cutsceneAttack; }
    bool holdsCombatRound() const override { return _schedule.holdsRound() && !isCompleted() && !isCancelled(); }
    // An instant cast takes no action of its round.
    bool tookRoundAction() const override { return _dispatched && !_fake && !_instantSpell; }
    bool joinsRunningRound() const override { return !_instantSpell; }
    bool instantSpell() const { return _instantSpell; }
    const std::shared_ptr<Spell> &spell() const { return _spell; }
    const std::optional<std::shared_ptr<Item>> &item() const { return _item; }
    const std::optional<size_t> &itemProperty() const { return _itemProperty; }
    /** Removes the unreleased projectile shown while conjuring. */
    void dropPresentation();
    /** The cast has reached its end and no longer occupies its creature. */
    bool castEnded() const { return !_schedule.awaitingRelease(); }
private:
    void finish(Object &caster, bool keepCastHold = false);
    bool commit(Object &actor);
    bool continuePaidCast(Creature &caster, const Action &queued);
    bool itemAvailable() const;
    void publishSpellContext(Object &actor);
    void release(Object &actor);
    std::shared_ptr<Spell> _spell;
    std::shared_ptr<Location> _targetLocation;
    std::optional<std::shared_ptr<Item>> _item;
    std::optional<size_t> _itemProperty;
    // The item's base item type, which decides how its use is shown and
    // whether it plays on without the item.
    std::optional<int> _itemType;
    std::optional<int> _itemCasterLevel;
    std::optional<SpellSelection> _selection;
    int _associatedFeat {-1};
    SpellSchedule _schedule;
    CastPresenter _presenter;
    uint64_t _presentationId {0};
    float _projectileTime {0.0f};
    bool _restorePresentation {false};
    bool _itemConsumed {false};
    SpellCastContext _castContext;
    bool _ownsMovementRestriction {false};
    bool _ownsSpellTarget {false};
    bool _cheat;
    ProjectilePathType _projectilePathType;
    bool _instantSpell;
    bool _fake;
    bool _started {false};
    bool _commitAttempted {false};
    bool _committed {false};
    bool _dispatched {false};
};

} // namespace game

} // namespace reone
