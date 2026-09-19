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
#include "../object.h"
#include "../types.h"

namespace reone {

namespace game {
class Item;
struct Spell;

class CastSpellAtObjectAction : public Action {
public:
    CastSpellAtObjectAction(
        Game &game,
        ServicesView &services,
        std::shared_ptr<Spell> spell,
        std::shared_ptr<Object> target,
        std::optional<std::shared_ptr<Item>> item,
        bool cheat = false,
        int metaMagic = 0,
        int domainLevel = 0,
        ProjectilePathType projectilePathType = ProjectilePathType::Default,
        bool instantSpell = false,
        std::optional<size_t> itemProperty = std::nullopt,
        std::optional<int> itemCasterLevel = std::nullopt);

    static bool classof(Action *from) {
        return from->type() == ActionType::CastSpellAtObject;
    }

    void execute(std::shared_ptr<Action> self, Object &actor, float dt) override;
    void finish(Object &caster);
    bool cancel(std::shared_ptr<Action> self, Object &actor) override;

    std::optional<SavedActionRecord> saveFacingState() const override;
    void restoreCastState(const SavedCastAction &state);
    bool suppressesEndRoundScript() const override { return _instantSpell || _cutsceneAttack; }
    bool holdsCombatRound() const override { return _started && _schedule.holdsRound() && !isCompleted() && !isCancelled(); }
    const std::shared_ptr<Object> &target() const { return _target; }
    const std::shared_ptr<Spell> &spell() const { return _spell; }
    const std::optional<std::shared_ptr<Item>> &item() const { return _item; }

private:
    bool itemAvailable(const Object &actor) const;
    bool commit(Object &actor);
    void release(Object &actor);

    std::shared_ptr<Spell> _spell;
    std::shared_ptr<Object> _target;
    std::optional<std::shared_ptr<Item>> _item;
    SpellSchedule _schedule;
    uint64_t _presentationId {0};
    float _projectileTime {0.0f};
    bool _restorePresentation {false};
    bool _itemConsumed {false};
    bool _cheat;
    ProjectilePathType _projectilePathType;
    bool _instantSpell;
    bool _ownsMovementRestriction {false};
    SpellCastContext _castContext;
    std::optional<size_t> _itemProperty;
    std::optional<int> _itemCasterLevel;
    bool _started {false};
    bool _commitAttempted {false};
    bool _committed {false};
    bool _dispatched {false};
};

} // namespace game

} // namespace reone
