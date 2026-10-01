/*
 * Copyright (c) 2025 The reone project contributors
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

#include "commonactions.h"

#include "reone/system/randomutil.h"

#include "reone/audio/di/services.h"
#include "reone/audio/mixer.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/audioclips.h"

#include "reone/game/action.h"
#include "reone/game/action/doorsaber.h"
#include "reone/game/action/playanimation.h"
#include "reone/game/action/wait.h"
#include "reone/game/event.h"
#include "reone/game/game.h"
#include "reone/game/object.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/item.h"
#include "reone/game/object/placeable.h"
#include "reone/game/party.h"

#include <algorithm>
#include "reone/game/action/opendoor.h"
#include "reone/game/di/services.h"
#include "reone/game/script/runner.h"
#include "reone/game/action/closedoor.h"
#include "reone/game/action/lockobject.h"
#include "reone/game/action/unlockobject.h"
#include "reone/system/logutil.h"
#include "reone/game/action/opencontainer.h"

namespace reone {

namespace game {

static constexpr float kLockWorkDuration = 1.5f;
static constexpr int kLockedEvent = 28;
static constexpr int kFailToOpenEvent = 34;
static constexpr int kKeyRequiredStrRef = 1431;

bool springTrapOnUse(Object &target, Object &actor) {
    auto creature = std::dynamic_pointer_cast<Creature>(actor.game().getObjectById(actor.id()));
    if (!creature) return false;
    if (auto *door = dyn_cast<Door>(&target); door && door->isTrapped() && door->isTrapHostileTo(*creature)) {
        door->triggerTrap(creature, false);
        return true;
    }
    if (auto *placeable = dyn_cast<Placeable>(&target);
        placeable && placeable->isTrapped() && placeable->isTrapHostileTo(*creature)) {
        placeable->triggerTrap(creature, false);
        return true;
    }
    return false;
}

void tryUnlockDoorWithKey(Game &game, Door &door, Object &actor, Party &party) {
    if (!door.isLocked() || !door.isKeyRequired() || door.keyName().empty()) {
        return;
    }
    Object *keyOwner = &actor;
    auto key = actor.getItemByTag(door.keyName());
    if (!key) {
        auto player = party.player();
        if (player && player->id() != actor.id()) {
            key = player->getItemByTag(door.keyName());
            keyOwner = player.get();
        }
    }
    if (!key) {
        return;
    }
    door.setLocked(false);
    if (door.isAutoRemoveKey()) {
        bool last = false;
        keyOwner->removeItem(key, last);
        if (last) {
            game.destroyRuntimeObjectGraph(key);
        }
    }
}

bool workAtLock(Action &action, Object &target, Object &actor, bool &working, float dt) {
    auto *creature = dyn_cast<Creature>(&actor);
    if (!creature || working) return true;
    // The walk to a lock takes the use point anew once when it changes, and runs.
    if (!creature->navigateToUse(target, 0.0f, dt, true, true)) return false;
    creature->turnToward(target);
    creature->playAnimation(AnimationType::LoopingUnlockDoor);
    working = true;
    actor.addActionBefore(action, actor.game().newAction<WaitAction>(kLockWorkDuration));
    return false;
}

void signalFailToOpen(Object &target, Object &actor, bool quiet) {
    actor.game().queueScriptEvent(
        target, &actor, quiet ? Event(kFailToOpenEvent, {1}, {}, {}, {}) : Event(kFailToOpenEvent));
}

// A lock that wants its key refuses the actor: the object is told it failed to
// open, and the creature the player controls hears the lock wants its key.
static void refuseKeyedLock(Object &target, Object &actor) {
    signalFailToOpen(target, actor);
    auto leader = actor.game().party().getLeader();
    if (leader && leader.get() == &actor) actor.game().addFeedbackMessage(kKeyRequiredStrRef);
}

void unlockDoor(Door &door, Object &actor) {
    if (!door.isLocked()) {
        return;
    }
    tryUnlockDoorWithKey(actor.game(), door, actor, actor.game().party());
    if (door.isLocked() && door.isKeyRequired()) {
        refuseKeyedLock(door, actor);
        return;
    }

    door.setLocked(false);
    door.open();
    door.onOpen(actor.id());
}

void unlockPlaceable(Placeable &placeable, Object &actor) {
    if (!placeable.isLocked()) {
        return;
    }
    if (placeable.isKeyRequired()) {
        refuseKeyedLock(placeable, actor);
        return;
    }

    placeable.setLocked(false);
}

// Locking an object that is not locked: with its key the actor locks it; a
// lock that wants its key refuses anything else; an object locks itself; a
// creature locks it when a d20, or 20 out of combat, and its Security rank
// reach the object's close lock DC, at least 1. A lock made signals the
// object that it was locked.
void lockObject(Object &target, Object &actor) {
    auto *door = dyn_cast<Door>(&target);
    auto *placeable = dyn_cast<Placeable>(&target);
    if (!door && !placeable) return;
    if (door ? door->isLocked() : placeable->isLocked()) return;
    const std::string &keyName = door ? door->keyName() : placeable->keyName();
    bool locked = !keyName.empty() && actor.getItemByTag(keyName);
    if (!locked && (door ? door->isKeyRequired() : placeable->isKeyRequired())) {
        refuseKeyedLock(target, actor);
        return;
    }
    if (!locked) {
        if (&target == &actor) {
            locked = true;
        } else if (auto *creature = dyn_cast<Creature>(&actor)) {
            const int roll = creature->isInCombat() ? randomInt(1, 20) : 20;
            const int rank = static_cast<int8_t>(creature->getUnopposedSkillRank(SkillType::Security));
            const int dc = std::max(1, static_cast<int>(door ? door->closeLockDC() : placeable->closeLockDC()));
            locked = roll + rank >= dc;
        }
    }
    if (!locked) return;
    if (door) {
        door->setLocked(true);
    } else {
        placeable->setLocked(true);
    }
    actor.game().queueScriptEvent(target, &actor, Event(kLockedEvent));
}

void jumpToPositionFacing(Creature &creature, const glm::vec3 &position, float facing, float trailFacing,
                          float radius, bool clearLine, Game &game) {
    auto module = game.module();
    auto area = module ? module->area() : nullptr;
    if (!area) return;
    auto spot = area->computeSafeLocation(position, radius, creature, clearLine);
    if (!spot) return;

    // The position changes at once; the creature's model turns to the new facing.
    creature.setPosition(*spot);
    creature.turnTo(facing);

    Room *roomBefore = creature.room();
    area->determineObjectRoom(creature);
    Room *roomAfter = creature.room();

    if (auto leader = game.party().getLeader()) {
        if (leader->id() == creature.id()) {
            game.party().resetFollowPath(*area, *spot, trailFacing, true);
            area->onPartyLeaderMoved(roomBefore != roomAfter);
        }
    }
    // Areas of effect the creature carries jump with it before its own
    // entries and exits are noted.
    if (auto live = game.getObjectById<Creature>(creature.id())) {
        area->jumpCarriedAreaEffects(*live);
        area->updateSubAreaOccupancy(live);
    }
}

// After walking up to a locked door it steps right up to, a creature waits
// this long, facing it, before it opens it.
static constexpr float kPreciseOpenWait = 0.5f;

void OpenDoorAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (actor.type() == ObjectType::Creature) {
        auto &creature = static_cast<Creature &>(actor);
        if (!creature.navigateToUse(*_door, 0.0f, dt, true, false, false)) {
            _waitAfterApproach = _door->isPreciseUse() && _door->isLocked();
            return;
        }
        creature.turnToward(*_door);
        if (_waitAfterApproach) {
            _waitAfterApproach = false;
            actor.addActionBefore(*this, _game.newAction<WaitAction>(kPreciseOpenWait));
            return;
        }
        if (springTrapOnUse(*_door, actor)) {
            complete();
            return;
        }
    }

    // Allow a door to open itself, bypassing all requirements. This is used by
    // scripts that assign actions to doors.
    bool isObjectSelf = _door->id() == actor.id();
    if (isObjectSelf) {
        _door->open();
        _door->onOpen(actor.id());
        complete();
        return;
    }

    tryUnlockDoorWithKey(_game, *_door, actor, _game.party());

    if (!_door->isLocked()) {
        _door->open();
        _door->onOpen(actor.id());
    } else {
        signalFailToOpen(*_door, actor);
    }

    complete();
}

// Only the door is kept, with the run mode every open is queued with; an open
// taken up again walks up afresh.
std::optional<SavedActionRecord> OpenDoorAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 20;
    result.declaredParameterCount = 2;
    result.parameters = {{3, SavedObjectReference::fromRuntimeId(_door->id())}, {1, int32_t {0}}};
    return result;
}

void CloseDoorAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    auto creatureActor = _game.getObjectById<Creature>(actor.id());

    bool reached = !creatureActor || creatureActor->navigateToUse(*_door, 0.0f, dt, true, false, false);
    if (reached) {
        _door->close();
        complete();
    }
}

std::optional<SavedActionRecord> CloseDoorAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 21;
    result.declaredParameterCount = 2;
    result.parameters = {{3, SavedObjectReference::fromRuntimeId(_door->id())}, {1, int32_t {0}}};
    return result;
}

// A lock is worked at from the object's use point.
void LockObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (!_target) {
        complete();
        return;
    }
    if (!workAtLock(*this, *_target, actor, _working, dt)) return;
    lockObject(*_target, actor);
    complete();
}

// Only the object is kept; a lock taken up again walks up and works afresh.
std::optional<SavedActionRecord> LockObjectAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 39;
    result.declaredParameterCount = 1;
    result.parameters = {{3, SavedObjectReference::fromRuntimeId(_target ? _target->id() : kSavedRuntimeInvalidObjectId)}};
    return result;
}

// An unlock is worked at from the object's use point; a trap on the object
// then goes off instead.
void UnlockObjectAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (!_target || _target->isDead()) {
        complete();
        return;
    }
    if (!workAtLock(*this, *_target, actor, _working, dt)) return;
    if (springTrapOnUse(*_target, actor)) {
        complete();
        return;
    }

    switch (_target->type()) {
    case ObjectType::Door:
        unlockDoor(static_cast<Door &>(*_target), actor);
        break;
    case ObjectType::Placeable:
        unlockPlaceable(static_cast<Placeable &>(*_target), actor);
        break;
    default:
        break;
    }
    complete();
}

// An unlock is kept as the object, no item used on the lock and no item
// property; taken up again it is worked as a Security unlock.
std::optional<SavedActionRecord> UnlockObjectAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 38;
    result.declaredParameterCount = 3;
    result.parameters = {
        {3, SavedObjectReference::fromRuntimeId(_target ? _target->id() : kSavedRuntimeInvalidObjectId)},
        {3, SavedObjectReference::fromRuntimeId(kSavedRuntimeInvalidObjectId)},
        {1, int32_t {0}},
    };
    return result;
}

// A door saber's work is the burn-door clip of the wielder's sword class on a
// character model, one row per class from single sword on; anything else
// works for two seconds without a clip.
static constexpr int kBurnDoorAnimationRow = 387;
static constexpr float kDoorSaberWorkSeconds = 2.0f;
static constexpr int kNotLockedStrRef = 1430;
static constexpr int kUnlockVoiceChance = 20;

// Every pass powers the creature's lightsabers and holds it in combat state
// without a battle cry. It goes to within a metre of the door, turns to it,
// then works at it. When the work is done a hostile trap on the
// door goes off instead; a door no longer locked is left as it is, and the
// creature the player controls hears that it is not locked; otherwise the
// door is unlocked and opened, and one time in five the creature says the
// unlock went well. No key is wanted and no experience is given.
void DoorSaberAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    assert(actor.type() == ObjectType::Creature);
    auto &creature = static_cast<Creature &>(actor);
    if (creature.isDead() || creature.isTemporarilyDead()) {
        complete();
        return;
    }

    creature.setLightsabersPowered(true, true);
    creature.setCombatState(true, CombatActivation::Indirect, false);
    if (!creature.navigateToUse(*_door, 0.0f, dt, true, true)) return;
    if (!_approached) {
        _approached = true;
        creature.turnToward(*_door);
        return;
    }
    if (!_working) {
        _working = true;
        auto leader = _game.party().getLeader();
        if (leader && leader.get() == &creature) {
            if (auto clip = _services.resource.audioClips.get("gui_doorsaber")) {
                _services.audio.mixer.play(std::move(clip), audio::AudioType::Sound);
            }
        }
        const auto wield = creature.getWieldType();
        const bool swordClass = wield == CreatureWieldType::SingleSword ||
                                wield == CreatureWieldType::DoubleBladedSword ||
                                wield == CreatureWieldType::DualSwords;
        if (creature.modelType() != Creature::ModelType::Creature && swordClass) {
            const auto animation = static_cast<AnimationType>(10000 + kBurnDoorAnimationRow + static_cast<int>(wield));
            actor.addActionBefore(*this, _game.newAction<PlayAnimationAction>(animation, 1.0f, kDoorSaberWorkSeconds));
        } else {
            actor.addActionBefore(*this, _game.newAction<WaitAction>(kDoorSaberWorkSeconds));
        }
        return;
    }

    if (springTrapOnUse(*_door, actor)) {
        complete();
        return;
    }
    if (!_door->isLocked()) {
        auto leader = _game.party().getLeader();
        if (leader && leader.get() == &creature) _game.addFeedbackMessage(kNotLockedStrRef);
        complete();
        return;
    }
    _door->setLocked(false);
    _door->open();
    _door->onOpen(actor.id());
    if (randomInt(0, 99) < kUnlockVoiceChance && creature.isHeardByLeader()) {
        creature.playSound(resource::SoundSetEntry::UnlockSuccess);
    }
    complete();
}

// A door saber taken away puts the creature back in its usual pose.
bool DoorSaberAction::cancel(std::shared_ptr<Action> self, Object &actor) {
    auto &creature = static_cast<Creature &>(actor);
    creature.resumeStateDrivenAnimation();
    return true;
}

// Only the door is kept; a door saber taken up again walks up, turns and
// works afresh.
std::optional<SavedActionRecord> DoorSaberAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 67;
    result.declaredParameterCount = 1;
    result.parameters = {{static_cast<uint32_t>(SavedActionParameterType::Object),
                          SavedObjectReference::fromRuntimeId(_door->id())}};
    return result;
}

static constexpr int kUsedEvent = 25;
// After walking up to an object, a creature waits this long, facing it,
// before it uses it.
static constexpr float kUseWaitAfterApproach = 0.5f;

// A use is made from the object's use point: a creature out of use range goes
// there, turns to the object and waits half a second first. A use does not
// lock the creature's facing. A placeable without an inventory is then used. A use of a container waits for it to swing open, then opens it
// for the controlled creature, which also counts as using it. An unusable or
// open container refuses the use, and a creature the player does not control
// only hears it open.
void OpenContainerAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    if (!_object || !isa<Creature>(actor)) {
        complete();
        return;
    }
    auto *placeable = dyn_cast<Placeable>(_object.get());
    if (placeable && placeable->hasInventory() && placeable->isLocked()) {
        complete();
        return;
    }

    auto &creatureActor = cast<Creature>(actor);
    if (!creatureActor.navigateToUse(*_object, 0.0f, dt)) {
        _approached = true;
        return;
    }
    if (_approached) {
        _approached = false;
        creatureActor.turnToward(*_object);
        actor.addActionBefore(*this, _game.newAction<WaitAction>(kUseWaitAfterApproach));
        return;
    }
    if (!placeable) {
        if (!springTrapOnUse(*_object, actor)) _game.openContainer(_object);
        complete();
        return;
    }
    if (!placeable->isUsable() || springTrapOnUse(*_object, actor)) {
        complete();
        return;
    }
    if (!placeable->hasInventory()) {
        _game.queueScriptEvent(*placeable, &actor, Event(kUsedEvent));
        complete();
        return;
    }
    if (&actor != _game.party().getLeader().get()) {
        placeable->playOpenedSound();
        _game.queueScriptEvent(*placeable, &actor, Event(kUsedEvent));
        complete();
        return;
    }
    if (placeable->isOpen()) {
        complete();
        return;
    }
    if (!placeable->isInventoryOpenPending()) {
        actor.addActionBefore(*this, _game.newAction<WaitAction>(placeable->beginOpeningInventory()));
        return;
    }
    placeable->completeOpeningInventory(actor);
    _game.queueScriptEvent(*placeable, &actor, Event(kUsedEvent));
    complete();
}

// Only the object is kept; a use taken up again walks up and opens afresh.
std::optional<SavedActionRecord> OpenContainerAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 40;
    result.declaredParameterCount = 1;
    result.parameters = {{3, SavedObjectReference::fromRuntimeId(_object ? _object->id() : kSavedRuntimeInvalidObjectId)}};
    return result;
}

} // namespace game

} // namespace reone
