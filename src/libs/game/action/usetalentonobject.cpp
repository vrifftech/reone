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

#include "reone/game/action/usetalentonobject.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/game.h"

namespace reone {

namespace game {

void UseTalentOnObjectAction::dispatchToAction() {
    assert(!_action && "dispatchToAction is called twice");
    switch (_chosenTalent->type()) {
    case TalentType::Feat: {
        _action = _game.newAction<UseFeatAction>(
            static_cast<FeatType>(_chosenTalent->value()),
            _target);
        break;
    }
    case TalentType::Spell: {
        auto spell = _services.game.spells.get(static_cast<SpellType>(_chosenTalent->value()));
        if (!spell) {
            return;
        }
        std::optional<std::shared_ptr<Item>> item;
        std::optional<size_t> property;
        std::optional<int> casterLevel;
        if (_chosenTalent->item() != script::kObjectInvalid) {
            auto source = _game.getObjectById<Item>(_chosenTalent->item());
            if (!source) return;
            item = std::move(source);
            if (_chosenTalent->itemPropertyIndex() >= 0)
                property = static_cast<size_t>(_chosenTalent->itemPropertyIndex());
            if (_chosenTalent->casterLevel() != 0xff)
                casterLevel = _chosenTalent->casterLevel();
        }
        _action = _game.newAction<CastSpellAtObjectAction>(
            std::move(spell), _target, item, /*cheat=*/false,
            _chosenTalent->metaType(), /*domainLevel=*/0,
            ProjectilePathType::Default, /*instantSpell=*/false, property, casterLevel);
        break;
    }
    default:
        break;
    }
}

void UseTalentOnObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (isCompleted() || isCancelled()) return;
    if (!_action) {
        complete();
        return;
    }

    _action->execute(self, actor, dt);
    if (_action->locked()) lock();

    if (_action->isCompleted()) {
        complete();
    }
}

bool UseTalentOnObjectAction::cancel(std::shared_ptr<Action>, Object &actor) {
    if (_action) {
        if (!_action->cancel(_action, actor)) return false;
        _action->markCancelled();
        _action->complete();
    }
    complete();
    return true;
}

std::optional<SavedActionRecord> UseTalentOnObjectAction::saveFacingState() const {
    // ActionUseTalentOnObject is a runtime dispatcher. Physical feats already
    // have the canonical ActionId 12 representation; spell actions carry
    // their versioned continuation in ActionId 15. Unsupported talents stay opaque.
    return _action ? _action->saveFacingState() : std::nullopt;
}

} // namespace game

} // namespace reone
