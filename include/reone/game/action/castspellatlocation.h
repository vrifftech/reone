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
class CastSpellAtLocationAction : public Action {
public:
    CastSpellAtLocationAction(Game &game, ServicesView &services,
        std::shared_ptr<Spell> spell, std::shared_ptr<Location> targetLocation,
        int metaMagic, bool cheat, ProjectilePathType projectilePathType, bool instantSpell);
    static bool classof(Action *from) { return from->type() == ActionType::CastSpellAtLocation; }
    void execute(std::shared_ptr<Action> self, Object &actor, float dt) override;
    bool cancel(std::shared_ptr<Action> self, Object &actor) override;
    std::optional<SavedActionRecord> saveFacingState() const override;
    void restoreCastState(const SavedCastAction &state);
    bool suppressesEndRoundScript() const override { return _instantSpell || _cutsceneAttack; }
    bool holdsCombatRound() const override { return _started && _schedule.holdsRound() && !isCompleted() && !isCancelled(); }
    const std::shared_ptr<Spell> &spell() const { return _spell; }
private:
    void finish(Object &caster);
    bool commit(Object &actor);
    void release(Object &actor);
    std::shared_ptr<Spell> _spell;
    std::shared_ptr<Location> _targetLocation;
    SpellSchedule _schedule;
    uint64_t _presentationId {0};
    float _projectileTime {0.0f};
    bool _restorePresentation {false};
    bool _itemConsumed {false};
    SpellCastContext _castContext;
    bool _ownsMovementRestriction {false};
    bool _cheat;
    ProjectilePathType _projectilePathType;
    bool _instantSpell;
    bool _started {false};
    bool _commitAttempted {false};
    bool _committed {false};
    bool _dispatched {false};
};

} // namespace game

} // namespace reone
