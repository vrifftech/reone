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

#include "reone/game/action/useskill.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include <boost/algorithm/string.hpp>

#include "commonactions.h"
#include "reone/game/animations.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/module.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/object/trigger.h"
#include "reone/game/party.h"
#include "reone/game/script/runner.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/strings.h"
#include "reone/system/logutil.h"
#include "reone/system/randomutil.h"

namespace reone {

namespace game {

// Demolitions

static constexpr float kMineWorkTime = 4.5f;
static constexpr float kSetMineWorkTime = 2.0f;
static constexpr float kMineUseRangeExtra = 0.25f;
static constexpr float kSetMineUseRangeExtra = 0.5f;
static constexpr int kDisarmActionStrRef = 1529;
static constexpr int kSetMineActionStrRef = 1530;
static constexpr int kRecoverActionStrRef = 1531;
static constexpr int kExamineActionStrRef = 1532;
static constexpr int kSkillSuccessStrRef = 1392;
static constexpr int kSkillFailureStrRef = 1393;
static constexpr int kSkillReportStrRef = 1408;
static constexpr int kDemolitionsNameStrRef = 324;
// Client animation rows for the mine work.
static constexpr int kGetFromGroundAnimation = 40;
static constexpr int kGetFromTableAnimation = 41;
static constexpr int kUnlockDoorAnimation = 47;
static constexpr int kUnlockContainerAnimation = 48;
static constexpr int kDisableMineAnimation = 51;
static constexpr int kSetMineAnimation = 52;
// Module script event: an item was lost.
static constexpr int kItemLostEvent = 20;
// Set-mine blast bonus by trap type, starting at type 4.
static constexpr int kMineBlastBonus[] = {3, 9, 0, 3, 9, 0, 0, 0, 0, 0, 0, 3, 9, 0, 0, 9, 12, 9, 12, 0, 0, 9, 12};

enum class MineWork {
    Disarm,
    Recover,
    Flag,
    Examine
};

enum SkillResult {
    SkillFailure = 0,
    SkillSuccess = 1,
    SkillCriticalFailure = 2,
    SkillTake20Failure = 3,
    SkillOwnMine = 4
};

static MineWork mineWork(int subSkill) {
    switch (subSkill) {
    case static_cast<int>(SubSkill::RecoverTrap):
        return MineWork::Recover;
    case static_cast<int>(SubSkill::FlagTrap):
        return MineWork::Flag;
    case static_cast<int>(SubSkill::ExamineTrap):
        return MineWork::Examine;
    default:
        return MineWork::Disarm;
    }
}

static const Item::PropertyEntry *firstActiveProperty(const Item &item) {
    for (const auto &property : item.properties()) {
        if (item.isPropertyActive(property)) return &property;
    }
    return nullptr;
}

static bool isTrapKit(const Item &item) {
    auto property = firstActiveProperty(item);
    return property && property->propertyName == static_cast<uint16_t>(ItemProperty::Trap);
}

static bool canUseDemolitions(const Creature &creature) {
    // Demolitions cannot be used untrained.
    return creature.attributes().hasSkill(SkillType::Demolitions);
}

// Mines set by the actor, or by a party member or PC while the actor is one, are its own.
static bool isOwnMine(Game &game, const Creature &actor, const Trigger &trap, bool creatorOnly) {
    auto creator = trap.trapCreator();
    if (!creator) return false;
    if (creator.get() == &actor) return true;
    if (creatorOnly) return false;
    auto &party = game.party();
    return (party.isMember(*creator) || creator->isPC()) && (party.isMember(actor) || actor.isPC());
}

bool UseSkillAction::setsMine() const {
    return _itemUsed && isTrapKit(*_itemUsed);
}

uint32_t UseSkillAction::serializedActionId() const {
    if (originalSavedAction()) return Action::serializedActionId();
    switch (_skill) {
    case SkillType::Security: return 38;
    case SkillType::Demolitions:
        if (setsMine()) return 29;
        switch (mineWork(_subSkill)) {
        case MineWork::Recover: return 26;
        case MineWork::Flag: return 27;
        case MineWork::Examine: return 28;
        case MineWork::Disarm: return 25;
        }
        return 25;
    default: return Action::serializedActionId();
    }
}

void UseSkillAction::executeDemolitions(Creature &actor, float dt) {
    auto area = _game.module() ? _game.module()->area() : nullptr;
    const bool setMine = setsMine();
    if (!area || (!setMine && !_target) || !canUseDemolitions(actor)) {
        complete();
        return;
    }
    if (setMine) {
        // The kit must still be carried, there must be somewhere to set it, and the area limit holds for everyone.
        if (!_itemUsed->isHeld() || !_target || !area->playerCanSetMines()) {
            complete();
            return;
        }
    } else {
        const MineWork work = mineWork(_subSkill);
        if (work == MineWork::Disarm || work == MineWork::Recover) {
            bool refused = false;
            if (auto *trigger = dyn_cast<Trigger>(_target.get())) refused = trigger->isTrap() && !trigger->trapDisarmable();
            if (auto *door = dyn_cast<Door>(_target.get())) refused = door->isTrapped() && !door->trapDisarmable();
            if (auto *placeable = dyn_cast<Placeable>(_target.get())) refused = placeable->isTrapped() && !placeable->trapDisarmable();
            if (refused) {
                complete();
                return;
            }
        }
    }

    const bool onSelf = !_target || _target.get() == &actor;
    if (_phase == 0) {
        // Approach to use range.
        if (!onSelf) {
            float extra = 0.0f;
            if (setMine) {
                extra = kSetMineUseRangeExtra;
            } else if (mineWork(_subSkill) == MineWork::Disarm || mineWork(_subSkill) == MineWork::Recover) {
                extra = kMineUseRangeExtra;
            }
            if (!actor.navigateToUse(*_target, extra, dt)) return;
            actor.turnToward(*_target);
        }
        // Work at it for a while.
        int animation = kSetMineAnimation;
        if (!setMine) {
            const MineWork work = mineWork(_subSkill);
            const bool trigger = _target->type() == ObjectType::Trigger;
            const bool door = _target->type() == ObjectType::Door;
            switch (work) {
            case MineWork::Disarm:
                animation = trigger ? kDisableMineAnimation : door ? kUnlockDoorAnimation : kUnlockContainerAnimation;
                break;
            case MineWork::Recover:
                animation = trigger ? kDisableMineAnimation : kGetFromTableAnimation;
                break;
            default:
                animation = trigger ? kGetFromGroundAnimation : kGetFromTableAnimation;
                break;
            }
        }
        actor.playAnimation(
            _services.game.animations.getNameById(animation),
            scene::AnimationProperties::fromFlags(scene::AnimationFlags::loop));
        _workTime = setMine ? kSetMineWorkTime : kMineWorkTime;
        _phase = 1;
        return;
    }
    _workTime -= dt;
    if (_workTime > 0.0f) return;
    actor.resumeStateDrivenAnimation();
    if (setMine) {
        resolveSetMine(actor);
    } else {
        resolveMine(actor);
    }
    complete();
}

void UseSkillAction::reportSkill(Creature &actor, const Object *target, int actionStrRef, int roll, int rank, int dc,
                                 bool take20, int result) const {
    // Told only to the controlling player.
    if (_game.party().getLeader().get() != &actor) return;
    auto &strings = _services.resource.strings;
    const bool success = result == SkillSuccess || result == SkillOwnMine;
    _game.setCustomToken(0, actor.name());
    _game.setCustomToken(1, _game.getFeedbackText(success ? kSkillSuccessStrRef : kSkillFailureStrRef));
    _game.setCustomToken(2, actionStrRef ? strings.getText(actionStrRef) : "");
    _game.setCustomToken(3, std::to_string(roll + rank));
    _game.setCustomToken(4, std::to_string(roll));
    _game.setCustomToken(5, rank < 0 ? "-" : "+");
    _game.setCustomToken(6, strings.getText(kDemolitionsNameStrRef));
    _game.setCustomToken(7, std::to_string(std::abs(rank)));
    _game.setCustomToken(8, std::to_string(dc));
    _game.messageLog().add(
        MessageLog::kFeedbackMessageType,
        MessageLog::Style::Normal,
        _game.getFeedbackText(kSkillReportStrRef));
}

void UseSkillAction::resolveMine(Creature &actor) {
    const MineWork work = mineWork(_subSkill);
    const int rank = static_cast<int8_t>(actor.getUnopposedSkillRank(SkillType::Demolitions));
    const bool take20 = !actor.isInCombat();
    const int roll = take20 ? 20 : randomInt(1, 20);
    const int total = roll + rank;
    auto area = _game.module()->area();
    auto actorPtr = std::dynamic_pointer_cast<Creature>(_game.getObjectById(actor.id()));
    auto *trigger = dyn_cast<Trigger>(_target.get());
    auto *door = dyn_cast<Door>(_target.get());
    auto *placeable = dyn_cast<Placeable>(_target.get());
    if (!trigger && !door && !placeable) return;

    const int disarmDC = trigger ? trigger->trapDisarmDC() : door ? door->trapDisarmDC() : placeable->trapDisarmDC();
    const int trapType = trigger ? trigger->trapBaseType() : door ? door->trapBaseType() : placeable->trapBaseType();
    TrapDetection &detection = trigger ? trigger->trapDetection() : door ? door->trapDetection() : placeable->trapDetection();
    const int autoFailDC = _game.isTSL() ? 65 : 35;
    auto awardXP = [&](int dc) {
        if (!_game.isTSL() || !_game.party().isMember(actor)) return;
        auto player = _game.party().player();
        const int level = player ? player->attributes().getAggregateLevel() : 0;
        _game.party().awardXP(level * (dc >= level + 20 ? 15 : 10), XPSource::Skill);
    };
    auto recoverItem = [&]() {
        auto traps = _services.resource.twoDas.get("traps");
        const std::string resRef = traps ? boost::to_lower_copy(traps->getString(trapType, "resref")) : "";
        if (resRef.empty()) return;
        if (auto receiver = _game.party().sharedInventoryReceiver(actorPtr)) receiver->addItem(resRef);
    };

    switch (work) {
    case MineWork::Disarm: {
        if (trigger && isOwnMine(_game, actor, *trigger, false)) {
            trigger->disarmTrap(actor);
            area->destroyObject(*trigger);
            // The award compares against an unset DC; it takes the lower rate.
            awardXP(0);
            reportSkill(actor, trigger, kDisarmActionStrRef, roll, rank, disarmDC, take20, SkillOwnMine);
            return;
        }
        // A disarm DC is at least 1.
        const int dc = std::max(1, disarmDC);
        const bool success = dc <= autoFailDC && total >= dc;
        int result = success ? SkillSuccess : take20 ? SkillTake20Failure : total < dc - 10 ? SkillCriticalFailure : SkillFailure;
        if (success) {
            if (trigger) {
                trigger->disarmTrap(actor);
                area->destroyObject(*trigger);
            } else if (door) {
                door->disarmTrap(actor);
            } else {
                placeable->disarmTrap(actor);
            }
            awardXP(dc);
        }
        if (!success) actor.playSound(resource::SoundSetEntry::UnlockSuccess);
        if (!success && total < dc - 10) {
            if (trigger) {
                trigger->fireMine(actorPtr, false);
            } else if (door) {
                door->triggerTrap(actorPtr, false);
            } else {
                placeable->triggerTrap(actorPtr, false);
            }
        }
        reportSkill(actor, _target.get(), kDisarmActionStrRef, roll, rank, dc, take20, result);
        return;
    }
    case MineWork::Recover: {
        if (trigger && isOwnMine(_game, actor, *trigger, false)) {
            area->destroyObject(*trigger);
            recoverItem();
            reportSkill(actor, trigger, kRecoverActionStrRef, roll, rank, disarmDC + 10, take20, SkillOwnMine);
            return;
        }
        const int dc = std::max(1, disarmDC + 10);
        const bool success = total >= dc;
        int result = success ? SkillSuccess : take20 ? SkillTake20Failure : total < dc - 10 ? SkillCriticalFailure : SkillFailure;
        if (success) {
            if (trigger) {
                area->destroyObject(*trigger);
            } else if (door) {
                door->disarmTrap(actor);
            } else {
                placeable->disarmTrap(actor);
            }
            recoverItem();
            awardXP(dc);
        } else if (total < dc - 5) {
            if (trigger) {
                trigger->fireMine(actorPtr, false);
            } else if (door) {
                door->triggerTrap(actorPtr, false);
            } else {
                placeable->triggerTrap(actorPtr, false);
            }
        }
        reportSkill(actor, _target.get(), kRecoverActionStrRef, roll, rank, dc, take20, result);
        return;
    }
    case MineWork::Flag: {
        if (trigger && isOwnMine(_game, actor, *trigger, true)) {
            detection.flagged = true;
            reportSkill(actor, nullptr, 0, roll, rank, disarmDC, take20, SkillOwnMine);
            return;
        }
        const int dc = std::max(1, disarmDC - 5);
        const bool success = total >= dc;
        if (success) detection.flagged = true;
        reportSkill(actor, nullptr, 0, roll, rank, dc, take20,
                    success ? SkillSuccess : take20 ? SkillTake20Failure : total < dc - 10 ? SkillCriticalFailure : SkillFailure);
        return;
    }
    case MineWork::Examine: {
        const int dc = std::max(1, disarmDC - 7);
        const bool own = trigger && isOwnMine(_game, actor, *trigger, true);
        const bool success = own || total >= dc;
        reportSkill(actor, nullptr, kExamineActionStrRef, roll, rank, dc, take20,
                    own ? SkillOwnMine : success ? SkillSuccess : take20 ? SkillTake20Failure : total < dc - 10 ? SkillCriticalFailure : SkillFailure);
        return;
    }
    }
}

void UseSkillAction::resolveSetMine(Creature &actor) {
    auto area = _game.module()->area();
    auto actorPtr = std::dynamic_pointer_cast<Creature>(_game.getObjectById(actor.id()));
    auto property = firstActiveProperty(*_itemUsed);
    const int trapType = property ? property->subtype : 0;
    const int blastBonus = trapType >= 4 && trapType - 4 < static_cast<int>(std::size(kMineBlastBonus))
                               ? kMineBlastBonus[trapType - 4]
                               : 0;
    auto traps = _services.resource.twoDas.get("traps");
    int setDC = traps ? traps->getInt(trapType, "setdc", 0) : 0;
    if (_game.isTSL()) setDC = std::max(1, setDC + _game.trapDifficultyModifier());

    int rank = static_cast<int8_t>(actor.getUnopposedSkillRank(SkillType::Demolitions));
    // KotOR rewards five trained ranks with two more.
    if (!_game.isTSL() && actor.getUnopposedSkillRank(SkillType::Demolitions, true) >= 5) rank += 2;
    const bool onObject = _target && (_target->type() == ObjectType::Door || _target->type() == ObjectType::Placeable);
    const bool hasTarget = static_cast<bool>(_target);
    const bool take20 = _game.isTSL() ? !(actor.isInCombat() && !hasTarget) : !actor.isInCombat();
    const int roll = take20 ? 20 : randomInt(1, 20);
    const int total = roll + rank;
    int result;
    if (total >= setDC) {
        result = SkillSuccess;
    } else if (take20) {
        result = SkillTake20Failure;
    } else if (total >= setDC - 10) {
        result = SkillFailure;
    } else {
        result = SkillCriticalFailure;
    }
    const bool planted = result == SkillSuccess || result == SkillCriticalFailure;
    const int detectDC = (traps ? traps->getInt(trapType, "detectdcmod", 0) : 0) + total;
    const int disarmDC = (traps ? traps->getInt(trapType, "disarmdcmod", 0) : 0) + total;

    if (planted) {
        std::shared_ptr<Trigger> groundMine;
        if (!onObject) {
            groundMine = area->spawnMine(trapType, actor.position(), actorPtr, actor.faction(), detectDC, disarmDC, rank);
        } else {
            auto *door = dyn_cast<Door>(_target.get());
            auto *placeable = dyn_cast<Placeable>(_target.get());
            const bool trapped = door ? door->isTrapped() : placeable->isTrapped();
            // TSL consumes the kit and reports success on an already trapped object.
            if (!(trapped && _game.isTSL())) {
                std::shared_ptr<Trigger> linked;
                if (_game.isTSL()) {
                    linked = area->spawnMine(trapType, _target->position(), actorPtr, actor.faction(), detectDC, disarmDC, rank);
                    linked->hideLinkedMine();
                }
                const bool blast = _game.isTSL() && result != SkillCriticalFailure;
                if (door) {
                    door->armMine(trapType, detectDC, disarmDC, rank, linked, actorPtr, blastBonus, blast);
                } else {
                    placeable->armMine(trapType, detectDC, disarmDC, rank, linked, actorPtr, blastBonus, blast);
                }
            }
        }
        // One kit is spent, and TSL tells the module the setter lost it.
        auto kit = _itemUsed;
        if (_game.isTSL()) {
            if (auto module = _game.module()) {
                _game.queueScriptEvent(*module, &actor, *_game.newEvent(kItemLostEvent,
                    std::vector<int32_t>{}, std::vector<float>{}, std::vector<std::string>{},
                    std::vector<std::shared_ptr<Object>>{kit}));
            }
        }
        if (auto holder = _game.getObjectById(kit->owner())) {
            bool last = false;
            holder->removeItem(kit, last);
            if (last) _game.destroyRuntimeObjectGraph(kit);
        }
        // A critical failure sets the mine off on its setter.
        if (result == SkillCriticalFailure) {
            if (groundMine) {
                groundMine->fireMine(actorPtr, true);
            } else if (auto *door = dyn_cast<Door>(_target.get())) {
                door->triggerTrap(actorPtr, true);
            } else if (auto *placeable = dyn_cast<Placeable>(_target.get())) {
                placeable->triggerTrap(actorPtr, true);
            }
        }
    }
    actor.playSound(result == SkillSuccess ? resource::SoundSetEntry::DisarmMine : resource::SoundSetEntry::UnlockSuccess);
    actor.interruptActivities(true);
    actor.removeCombatInvisibilityEffects();
    reportSkill(actor, onObject ? _target.get() : nullptr, kSetMineActionStrRef, roll, rank, setDC, take20, result);
}

// END Demolitions

bool UseSkillAction::cancel(std::shared_ptr<Action> self, Object &actor) {
    if (_skill == SkillType::Demolitions && _phase == 1) {
        if (auto *creature = dyn_cast<Creature>(&actor)) creature->resumeStateDrivenAnimation();
        _phase = 0;
    }
    return true;
}

void UseSkillAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    switch (_skill) {
    case SkillType::Security: {
        if (!_target) {
            warn("ActionExecutor: unsupported Security target: null");
            complete();
            return;
        }
        if (_target->type() == ObjectType::Door) {
            if (!workAtLock(*this, *_target, actor, _workingAtLock, dt)) return;
            unlockDoor(static_cast<Door &>(*_target), actor);
            complete();
            return;
        }
        if (_target->type() == ObjectType::Placeable) {
            if (!workAtLock(*this, *_target, actor, _workingAtLock, dt)) return;
            auto &placeable = static_cast<Placeable &>(*_target);
            unlockPlaceable(placeable, actor);
            if (placeable.hasInventory() && !placeable.isLocked()) {
                placeable.openInventory(actor);
            }
            complete();
            return;
        }

        warn("ActionExecutor: unsupported Security target: " + std::to_string(_target->id()));
        complete();
        return;
    }
    case SkillType::Demolitions: {
        auto *creature = dyn_cast<Creature>(&actor);
        if (!creature) {
            complete();
            return;
        }
        executeDemolitions(*creature, dt);
        return;
    }
    default:
        warn("ActionExecutor: unsupported UseSkillAction");
        complete();
    }
}

} // namespace game

} // namespace reone
