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

#include "reone/game/combat.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "reone/game/action/attackobject.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/castspellatlocation.h"
#include "reone/game/action/movetolocation.h"
#include "reone/game/action/movetoobject.h"
#include "reone/game/action/unequipitem.h"
#include "reone/game/party.h"
#include "reone/game/d20/spell.h"
#include "reone/game/equipmentoperation.h"
#include "reone/game/equipmentrules.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/scene/di/services.h"
#include "reone/scene/graphs.h"
#include "reone/system/logutil.h"
#include "reone/system/randomutil.h"
#include "reone/system/smallset.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static bool isUnavailableAttackTarget(const Creature &creature) {
    return creature.isDead() || creature.isTemporarilyDead();
}

// A pause with no one behind it holds every entry. A knocked-down creature's
// entry waits until it is up again.
bool CombatRound::suspends(const Action &action) const {
    for (const auto &entry : actions) {
        if (entry.action.get() != &action && &entry.action->combatAction() != &action) continue;
        auto attacker = entry.attacker.resolve();
        if (attacker && attacker->isKnockedDown()) return true;
        if (pauseRemaining <= 0.0f) return false;
        auto owner = pauseOwner.resolve();
        return !owner || attacker != owner;
    }
    return pauseRemaining > 0.0f;
}

bool CombatRound::canExecute(Action &action) const {
    if (suspends(action)) return false;
    State requiredState[] = {CombatRound::FirstAction, CombatRound::SecondAction};
    assert(actions.size() <= 2 && "no more than 2 actors in a round");
    for (unsigned i = 0; i < actions.size(); ++i) {
        if (actions[i].action.get() == &action) {
            return !actions[i].retired && actions[i].participantBindingsLive() &&
                   actions[i].remainsInActorQueue() &&
                   action.runtimeDependenciesLive() &&
                   !action.isCompleted() &&
                   !action.isCancelled() &&
                   state >= requiredState[actions[i].slot] &&
                   state != CombatRound::Finished;
        }
    }
    return false;
}

bool CombatRound::RoundAction::participantBindingsLive() const {
    return attacker.resolve() &&
           (target.empty() || target.resolve());
}

bool CombatRound::RoundAction::remainsInActorQueue() const {
    if (!actorQueueAssociated || action->isCompleted()) {
        return true;
    }
    auto actor = attacker.resolve();
    return actor && std::find(
                        actor->actions().begin(),
                        actor->actions().end(),
                        action) != actor->actions().end();
}

static std::shared_ptr<Object> getTarget(Action &queued) {
    Action &action = queued.combatAction();
    if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&action))
        return cast->spell()->hostile ? cast->target() : nullptr;
    if (auto *attack = dyn_cast<AttackObjectAction>(&action)) {
        return attack->target();
    }
    if (auto *feat = dyn_cast<UseFeatAction>(&action)) {
        return feat->target();
    }
    if (auto *stance = dyn_cast<CombatStanceAction>(&action))
        return stance->stance() == CombatStance::TotalDefense ? stance->target() : nullptr;

    return nullptr;
}

static void recordCombatAction(
    Object &actor,
    const std::shared_ptr<Object> &target,
    Action &action) {

    const auto *spell = dyn_cast<CastSpellAtObjectAction>(&action.combatAction());
    if (!target || (!isHostileAction(action.combatAction()) && !(spell && spell->spell()->hostile))) {
        return;
    }

    target->setLastHostileActor(actor.id());
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        if (spell) creature->setAttemptedSpellTarget(target->id());
        else creature->setAttemptedAttackTarget(target->id());
    }
}

// An instant cast, or the attack an instant cast makes.
static bool isInstantCast(Action &command) {
    if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&command)) return cast->instantSpell();
    if (auto *cast = dyn_cast<CastSpellAtLocationAction>(&command)) return cast->instantSpell();
    if (auto *feat = dyn_cast<UseFeatAction>(&command)) return feat->isInstantCastAttack();
    return false;
}

static bool isItemUse(Action &command) {
    if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&command)) return cast->item().has_value();
    if (auto *cast = dyn_cast<CastSpellAtLocationAction>(&command)) return cast->item().has_value();
    return false;
}

static bool isHostileForContinuation(const Creature &source, const Creature &target) {
    return source.getReputationToward(target) <= 10;
}

std::shared_ptr<Creature> Combat::findNearestEnemy(
    Area &area,
    const Creature &attacker,
    const Creature &observer,
    float maxDistance,
    const Object *excluded) {

    std::shared_ptr<Creature> nearest;
    float nearestDistance = maxDistance;

    for (const std::shared_ptr<Object> &object : area.getObjectsByType(ObjectType::Creature)) {
        auto candidate = std::static_pointer_cast<Creature>(object);
        if (candidate->id() == attacker.id() ||
            candidate->id() == observer.id() ||
            candidate.get() == excluded ||
            isUnavailableAttackTarget(*candidate)) {
            continue;
        }

        bool isEnemy = observer.id() == attacker.id()
                           ? isHostileForContinuation(*candidate, attacker)
                           : isHostileForContinuation(attacker, *candidate);
        if (!isEnemy || !observer.perception().sees(candidate->id())) {
            continue;
        }

        float distance = attacker.getDistanceTo(*candidate) -
                         observer.creaturePersonalSpace() -
                         candidate->creaturePersonalSpace();
        if (distance >= nearestDistance || !area.isObjectSeen(observer, *candidate)) {
            continue;
        }

        nearest = std::move(candidate);
        nearestDistance = distance;
    }

    return nearest;
}

CombatRound *Combat::findRoundForAction(
    const std::shared_ptr<Action> &action, const Creature &attacker) {

    for (auto &round : _rounds) {
        for (CombatRound::RoundAction &roundAction : round->actions) {
            auto boundAttacker = roundAction.attacker.resolve();
            if (!roundAction.retired && boundAttacker.get() == &attacker &&
                roundAction.action == action) {
                return round.get();
            }
        }
    }
    return nullptr;
}

// If there is an incomplete combat round where attacker and target roles
// are reversed, append to that round.
CombatRound *Combat::tryAppendAction(
    const std::shared_ptr<Action> &action,
    const std::shared_ptr<Creature> &attacker,
    const std::shared_ptr<Object> &target,
    bool actorQueueAssociated) {

    for (auto &round : _rounds) {
        if (round->state == CombatRound::Finished) {
            // Finished rounds may not be retired until all actions are
            // completed. Do not consider them for new rounds.
            continue;
        }

        bool isReversed = false;
        for (CombatRound::RoundAction &roundAction : round->actions) {
            if (!roundAction.retired && roundAction.attacker.resolve().get() == target.get() &&
                roundAction.target.resolve().get() == attacker.get()) {
                isReversed = true;
                break;
            }
        }

        if (!isReversed || round->actions.size() > 1) {
            continue;
        }

        // Found a round to append.
        round->actions.emplace_back(
            action, attacker, target, actorQueueAssociated);
        round->actions.back().slot = 1;
        round->duel = true;
        return round.get();
    }
    return nullptr;
}

const CombatRound &Combat::addAction(const std::shared_ptr<Action> &action, Object &actor) {
    auto attacker = _game.getObjectById<Creature>(actor.id());
    assert(attacker && attacker.get() == &actor && "Combat action actor is not the live registered Creature");
    attacker->setCurrentCombatAction(action);
    bool actorQueueAssociated = std::find(
                                    attacker->actions().begin(),
                                    attacker->actions().end(),
                                    action) != attacker->actions().end();

    // If attacker has already started a combat round, return it.
    if (CombatRound *round = findRoundForAction(action, *attacker)) {
        return *round;
    }

    auto &command = action->combatAction();
    std::shared_ptr<Object> target = getTarget(*action);
    // Physical activation belongs to accepted submission and attacked events,
    // not to creating (or appending) a presentation round. Keep the separate
    // spell/cutscene registration path outside that physical-command contract.
    // Combat state is set before the round starts, so a creature that cannot
    // enter combat leaves it without ending the round it is about to start.
    const auto *feat = dyn_cast<UseFeatAction>(&command);
    const bool physical = isa<AttackObjectAction>(&command) ||
        (feat && isPhysicalAttackFeat(feat->feat()));
    auto activateCombat = [&]() {
        if (physical || isa<CombatStanceAction>(&command)) return;
        if (isHostileAction(command)) attacker->setCombatState(true);
        if (target && isa<Creature>(target))
            cast<Creature>(target)->setCombatState(true, CombatActivation::Indirect);
    };

    // A creature fights one round at a time. A round that an earlier command
    // began, and that command has finished with, is still the creature's.
    for (auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (auto &entry : round->actions) {
            if (entry.retired || entry.attacker.resolve() != attacker || entry.action == action ||
                !entry.action->isCompleted()) continue;
            if (command.joinsRunningRound()) {
                // A cast or item use takes its turn inside that round, which
                // keeps its target, clock and engagement.
                entry.action = action;
                entry.actorQueueAssociated = actorQueueAssociated;
                entry.joined = true;
                recordCombatAction(actor, target, *action);
                activateCombat();
                return *round;
            }
            // Any other command starts the round afresh; the round it gives up
            // has no end, and a partner it holds is left as it is.
            entry.retired = true;
            entry.action->retireCombatRound();
            entry.target.reset();
        }
    }

    // Starting a round, or entering the one its target began, ends the pause
    // held while owning none, and the round has not yet told the attacker its
    // weapon is ineffective.
    _ownerPauses.erase(attacker->id());
    attacker->setWeaponIneffectiveReported(false);

    // Starting a round records its target, or none when it targets the actor itself.
    if (target && target.get() != attacker.get()) attacker->setRoundTarget(target);
    else attacker->clearRoundTarget();
    // The round's owner locks its orientation on the round's target.
    attacker->setOrientationLock(target ? target->id() : script::kObjectInvalid);
    // Find an existing round where target and attacker roles are reversed, and
    // append the action to this round. An instant cast starts a round of its
    // own that engages no one.
    if (target && !isInstantCast(command)) {
        if (CombatRound *round = tryAppendAction(
                action, attacker, target, actorQueueAssociated)) {
            recordCombatAction(actor, target, *action);
            configureSpellPair(*round, action, *attacker);
            debug(str(boost::format("Append attack: %s -> %s") % actor.tag() % target->tag()), LogChannel::Combat);
            return *round;
        }
    }

    activateCombat();

    // Otherwise, start a new combat round
    _rounds.emplace_back(std::make_shared<CombatRound>(
        action, attacker, target, actorQueueAssociated));
    CombatRound &newRound = *_rounds.back();
    newRound.id = _nextRoundId++;

    if (target) {
        recordCombatAction(actor, target, *action);
        debug(str(boost::format("Start round: %s -> %s") % actor.tag() % target->tag()), LogChannel::Combat);
    } else {
        debug(str(boost::format("Start round: %s") % actor.tag()), LogChannel::Combat);
    }

    configureSpellPair(newRound, action, *attacker);
    return newRound;
}

// The dispatcher holds the head of the queue until the owner's round ends.
// A round whose action still waits behind it in the queue does not hold it,
// or neither could advance.
void CombatDispatchAction::execute(std::shared_ptr<Action>, Object &actor, float) {
    // While the world is held, the round waits, and the queue behind it.
    if (_game.holdsWorld()) return;
    auto *creature = dyn_cast<Creature>(&actor);
    if (!_game.combat().hasScheduled(actor) && !(creature && _game.combat().ownsDetachedRound(*creature))) complete();
}

// Clearing the dispatcher leaves the round's pending entries as they are.
bool CombatDispatchAction::cancel(std::shared_ptr<Action>, Object &) {
    complete();
    return true;
}

std::optional<SavedActionRecord> CombatDispatchAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 63;
    result.declaredParameterCount = 1;
    result.parameters = {{1, int32_t {_mode}}};
    return result;
}

static constexpr uint32_t kTotalDefenseMilliseconds = 1500;

// A party member at zero vitality cannot hold a stance; others only need to live.
static bool canHoldStance(Game &game, const Creature &actor) {
    if (actor.isDead()) return false;
    return !game.party().isMember(actor) || actor.currentHitPoints() > 0;
}

void CombatStanceAction::execute(std::shared_ptr<Action> self, Object &actor, float) {
    auto *creature = dyn_cast<Creature>(&actor);
    if (creature && _stance == CombatStance::TotalDefense)
        _game.party().setCombatMessage(*creature, _game.party().idleCombatMessage());
    if (!creature || !canHoldStance(_game, *creature)) {
        if (creature && _stance == CombatStance::TotalDefense) creature->cancelAllCombatModes();
        complete();
        return;
    }
    if (_stance == CombatStance::TotalDefense) {
        // Holding the stance counts as a combat decision for the end-of-round script.
        creature->refreshCombatDecisionTimer();
        if (!_game.holdsWorld() && !_game.combat().isActionPaused(*this) && !_game.combat().isOwnerPaused(*creature)) {
            // The stance begins while the creature has no Total Defense start.
            if (!creature->hasTotalDefenseStart()) {
                creature->setAttemptedAttackTarget(_target ? _target->id() : script::kObjectInvalid);
                if (_target) {
                    creature->setDesiredFacingToward(_target->position());
                    creature->setOrientationLock(_target->id());
                }
                creature->interruptActivities();
            }
            creature->setCombatStance(CombatStance::TotalDefense);
            // Starting the creature's round marks the start; a round already
            // running leaves it as it is.
            if (!_game.combat().ownsRound(*creature))
                creature->markTotalDefenseStart(_game.worldTimeDay(), _game.worldTimeOfDay());
            _game.combat().addAction(self, actor);
            executeTotalDefense(*creature);
        }
        return;
    }
    executeMeditative(*creature);
}

// Total Defense completes once the time of day part of the world time since
// its start reaches 1.5 s, checked only while its round is not paused. With no
// start marked, the whole time of day counts.
void CombatStanceAction::executeTotalDefense(Creature &actor) {
    uint32_t days = 0;
    uint32_t elapsed = 0;
    _game.subtractWorldTimes(_game.worldTimeDay(), _game.worldTimeOfDay(),
        actor.totalDefenseDay(), actor.totalDefenseTime(), days, elapsed);
    if (elapsed < kTotalDefenseMilliseconds) return;
    actor.clearTotalDefenseStart();
    actor.setCombatStance(CombatStance::None);
    complete();
}

void CombatStanceAction::executeMeditative(Creature &actor) {
    if (actor.combatStance() != CombatStance::Meditative) {
        actor.setCombatStance(CombatStance::Meditative);
        return;
    }
    // The stance lasts until another command waits behind it.
    if (actor.actions().nodes.size() >= 2) {
        actor.setCombatStance(CombatStance::None);
        complete();
    }
}

bool CombatStanceAction::cancel(std::shared_ptr<Action>, Object &actor) {
    // Clearing a stance command always drops the stance; Total Defense also
    // hands on the round's pending equipment.
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        creature->setCombatStance(CombatStance::None);
        if (_stance == CombatStance::TotalDefense) _game.combat().transferEquipment(*creature);
    }
    return true;
}

std::optional<SavedActionRecord> CombatStanceAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = serializedActionId();
    result.parameters.clear();
    if (_stance == CombatStance::TotalDefense)
        result.parameters.push_back({3, SavedObjectReference::fromRuntimeId(
            _target ? _target->id() : kSavedRuntimeInvalidObjectId)});
    result.declaredParameterCount = static_cast<uint16_t>(result.parameters.size());
    return result;
}

bool Combat::ownsRound(const Creature &actor) const {
    return std::any_of(_rounds.begin(), _rounds.end(), [&actor](const auto &round) {
        return round->state != CombatRound::Finished &&
               std::any_of(round->actions.begin(), round->actions.end(), [&actor](const auto &entry) {
                   return !entry.retired && entry.attacker.resolve().get() == &actor;
               });
    });
}

bool Combat::ownsDetachedRound(const Creature &actor) const {
    const auto &nodes = actor.actions().nodes;
    bool owned = false;
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (const auto &entry : round->actions) {
            if (entry.retired || entry.attacker.resolve().get() != &actor) continue;
            owned = true;
            if (std::any_of(nodes.begin(), nodes.end(), [&](const auto &node) { return node->action == entry.action; }))
                return false;
        }
    }
    return owned;
}

bool Combat::hasScheduled(const Object &actor) const {
    auto found = _scheduled.find(actor.id());
    return found != _scheduled.end() && found->second->actor.resolve().get() == &actor &&
        !found->second->actions.empty();
}

void Combat::ensureDispatcher(Creature &actor, bool toFront) {
    for (const auto &node : actor.actions().nodes)
        if (node->actionId == 63 && node->action && !node->action->isCompleted() && !node->action->isCancelled()) return;
    if (!toFront) {
        actor.addAction(_game.newAction<CombatDispatchAction>());
        return;
    }
    // A physical attack that its round has taken up stays at the front of the
    // queue for the round, so the dispatcher goes behind it.
    const auto &nodes = actor.actions().nodes;
    const bool attackRunning = !nodes.empty() && nodes.front()->action && nodes.front()->action->locked() &&
                               !nodes.front()->action->isCompleted() && !nodes.front()->action->isCancelled() &&
                               (isa<AttackObjectAction>(nodes.front()->action.get()) ||
                                isa<UseFeatAction>(nodes.front()->action.get()));
    if (!attackRunning) {
        actor.addActionOnTop(_game.newAction<CombatDispatchAction>());
    } else if (nodes.size() > 1 && nodes[1]->action) {
        actor.addActionBefore(*nodes[1]->action, _game.newAction<CombatDispatchAction>());
    } else {
        actor.addAction(_game.newAction<CombatDispatchAction>());
    }
}

// A cutscene attacker that masters an engaged exchange starts its partner's
// round as a cutscene round too: neither of them runs the end-of-round script
// or fights on from it. The pair's first attacker masters it.
static bool isCutsceneRoundFor(const CombatRound &round, const CombatRound::RoundAction &entry) {
    if (entry.action->combatAction().isCutsceneAttack()) return true;
    if (!round.duel || round.actions.empty()) return false;
    const auto &first = round.actions.front();
    return first.attacker.resolve().get() != entry.attacker.resolve().get() &&
        first.action->combatAction().isCutsceneAttack();
}

// The end of a round pause returns a living, standing, able creature to its
// pause or ready pose, except in a cutscene round.
static void readyAfterRoundPause(Creature &creature, bool cutsceneRound) {
    if (creature.isDead() || creature.isKnockedDown() || creature.movementType() != Creature::MovementType::None) return;
    // Unable to act for its round, meditation included.
    if (creature.isDebilitated(true) || cutsceneRound) return;
    creature.showPauseReadyAnimation(true);
}

// A scheduled entry's pause ends like any round pause: the creature, and the
// partner of a pair it masters, come back to their pose.
void Combat::readyAfterScheduledPause(Creature &actor) {
    std::vector<std::pair<std::shared_ptr<Creature>, bool>> posing;
    bool own = false;
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished || round->actions.empty()) continue;
        const auto &first = round->actions.front();
        const bool master = round->duel && !first.retired && first.attacker.resolve().get() == &actor;
        for (const auto &entry : round->actions) {
            auto creature = entry.retired ? nullptr : entry.attacker.resolve();
            if (!creature || (creature.get() != &actor && !master)) continue;
            own = own || creature.get() == &actor;
            posing.emplace_back(std::move(creature), isCutsceneRoundFor(*round, entry));
        }
    }
    for (const auto &[creature, cutscene] : posing) readyAfterRoundPause(*creature, cutscene);
    if (!own) readyAfterRoundPause(actor, false);
}

void Combat::reactToDamage(Creature &victim, const std::shared_ptr<Object> &damager, const DamageReaction &reaction) {
    if (victim.isDebilitated(true)) return;
    // Any other damage type makes a living creature flinch, whatever it is doing.
    if (!reaction.jolt) {
        if (victim.currentHitPoints() > 0) victim.playDamageFlinch();
        return;
    }
    // Only a creature standing in its plain pause or ready loop is jolted.
    if (!victim.showsPauseReadyAnimation()) return;
    const int milliseconds = reaction.milliseconds;
    CombatRound *owned = nullptr;
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (const auto &entry : round->actions)
            if (!entry.retired && entry.attacker.resolve().get() == &victim) owned = round.get();
    }
    auto pausedBy = damager ? _game.getObjectById(damager->id()) : nullptr;
    const float seconds = std::max(0, milliseconds) / 1000.0f;
    // A round already held keeps its pause.
    if (owned) {
        if (owned->pauseRemaining <= 0.0f) {
            rescaleRound(*owned, std::max(0, milliseconds), false);
            owned->pauseRemaining = seconds;
            owned->pauseOwner = pausedBy;
        }
    } else {
        auto &pause = _ownerPauses[victim.id()];
        if (pause.remaining <= 0.0f) {
            pause.actor = _game.getObjectById<Creature>(victim.id());
            pause.pausedBy = pausedBy;
            pause.remaining = seconds;
        }
    }
    // A creature the damage has brought down shows no spasm.
    if (victim.currentHitPoints() > 0) victim.playAnimation(AnimationType::LoopingSpasm);
}

std::optional<Combat::OwnerHold> Combat::savedOwnerHold(const Object &actor) const {
    const auto paused = _ownerPauses.find(actor.id());
    if (paused == _ownerPauses.end() || !paused->second.engaged || paused->second.remaining <= 0.0f ||
        paused->second.actor.resolve().get() != &actor) return std::nullopt;
    return OwnerHold {paused->second.remaining, paused->second.pausedBy.resolve()};
}

void Combat::restoreOwnerHold(const std::shared_ptr<Creature> &actor, float remaining,
                              const std::shared_ptr<Object> &pausedBy) {
    auto &hold = _ownerPauses[actor->id()];
    hold.actor = actor;
    hold.pausedBy = pausedBy;
    hold.remaining = remaining;
    hold.engaged = true;
}

bool Combat::isOwnerPaused(const Creature &actor) const {
    if (actor.isKnockedDown()) return true;
    const auto paused = _ownerPauses.find(actor.id());
    return paused != _ownerPauses.end() && paused->second.remaining > 0.0f;
}

// Going down replaces the pauses on the rounds only the creature takes part in,
// and the one it has while owning no round.
void Combat::holdKnockedDown(const Creature &creature) {
    _ownerPauses.erase(creature.id());
    for (auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        bool own = false;
        bool others = false;
        for (const auto &entry : round->actions) {
            if (entry.retired) continue;
            if (entry.attacker.resolve().get() == &creature) own = true;
            else others = true;
        }
        if (!own || others) continue;
        round->pauseRemaining = 0.0f;
        round->pauseOwner.reset();
    }
}

void Combat::settleRequested() {
    if (_settleRequests.empty()) return;
    const auto requests = std::move(_settleRequests);
    _settleRequests.clear();
    for (const auto &request : requests) {
        auto action = request.lock();
        if (!action) continue;
        std::shared_ptr<CombatRound> settling;
        for (const auto &round : _rounds) {
            if (round->state == CombatRound::Finished) continue;
            for (const auto &entry : round->actions) {
                if (!entry.retired && (entry.action == action || &entry.action->combatAction() == action.get()))
                    settling = round;
            }
            if (settling) break;
        }
        if (!settling) continue;
        // The requester, and in an engaged pair its partner too, come back to
        // their pose once the pause is over, after any round end it brings.
        std::vector<std::pair<std::shared_ptr<Creature>, bool>> posing;
        for (const auto &entry : settling->actions) {
            if (entry.retired) continue;
            const bool own = entry.action == action || &entry.action->combatAction() == action.get();
            if (!own && !settling->duel) continue;
            if (auto actor = entry.attacker.resolve())
                posing.emplace_back(std::move(actor), isCutsceneRoundFor(*settling, entry));
        }
        // No time passes; only the end conditions are tested. The round is
        // kept alive through its end scripts.
        updateRound(*settling, 0.0f);
        for (const auto &[actor, cutscene] : posing) readyAfterRoundPause(*actor, cutscene);
    }
}

bool Combat::isEngaged(const Creature &creature) const {
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished || !round->duel) continue;
        for (const auto &entry : round->actions)
            if (!entry.retired && entry.attacker.resolve().get() == &creature) return true;
    }
    return false;
}

// A round not yet started always has room; one paused by someone else has
// none; otherwise a reaction fits while half a second of the round remains.
bool Combat::hasReactionRoom(const Creature &creature, const Creature &attacker) const {
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        const bool owned = std::any_of(round->actions.begin(), round->actions.end(), [&](const auto &entry) {
            return !entry.retired && entry.attacker.resolve().get() == &creature;
        });
        if (!owned) continue;
        if (round->pauseRemaining > 0.0f && round->pauseOwner.resolve().get() != &attacker) return false;
        if (round->state == CombatRound::Pending) return true;
        const int remaining = static_cast<int>(round->duration * 1000.0f) - static_cast<int>(round->time * 1000.0f);
        return 1000 + remaining >= 1500;
    }
    const auto paused = _ownerPauses.find(creature.id());
    return paused == _ownerPauses.end() || paused->second.remaining <= 0.0f ||
        paused->second.pausedBy.resolve().get() == &attacker;
}

std::vector<std::pair<int, Action *>> Combat::pendingScheduled(const Object &actor) const {
    std::vector<std::pair<int, Action *>> result;
    auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor) return result;
    for (const auto &entry : found->second->actions) {
        if (!entry || entry->saved.applied) continue;
        result.emplace_back(entry->saved.type, entry->node ? entry->node->action.get() : nullptr);
    }
    return result;
}

static bool scheduledCommandMatches(const SavedScheduledAction &saved) {
    if (!saved.command) return saved.type == 3;
    const auto id = saved.command->actionId;
    switch (saved.type) {
    case 1: case 11: return id == 12;
    case 6: return id == 8;
    case 7: return id == 11;
    case 9: return id == 15;
    case 10: return id == 46;
    case 12: return id == 1 || id == 17;
    case 13: return id == 68 || id == 69;
    case 14: return id == 71;
    default: return false;
    }
}


bool Combat::scheduleEquipment(Object &object, const OrdinaryActionQueue::Node &node) {
    auto actor = std::dynamic_pointer_cast<Creature>(_game.getObjectById(object.id()));
    if (!actor || !node || !node->action || node->action->originalSavedAction()) return false;
    const auto id = node->action->serializedActionId();
    if (id != 8 && id != 11) return false;
    auto command = node->action->saveFacingState();
    if (!command || command->parameters.size() < 3) return false;
    auto target = std::get_if<SavedObjectReference>(&command->parameters[0].payload);
    auto flags = std::get_if<int32_t>(&command->parameters[2].payload);
    if (!target || !flags) return false;
    const uint32_t slot = id == 8 ? static_cast<uint32_t>(std::get<int32_t>(command->parameters[1].payload)) : 0;
    // An item already worn is not queued again for the head, the body or a
    // primary hand.
    if (id == 8) {
        const uint32_t primary = equipmentSlotMask(InventorySlots::head) | equipmentSlotMask(InventorySlots::body) |
                                 equipmentSlotMask(InventorySlots::rightWeapon) |
                                 equipmentSlotMask(InventorySlots::leftWeapon);
        const bool worn = std::any_of(actor->equipment().begin(), actor->equipment().end(), [&](const auto &entry) {
            return entry.second && entry.second->id() == target->id;
        });
        if ((slot & primary) && worn) {
            node->action->markCancelled();
            return true;
        }
    }
    if (!actor->isCommandable()) {
        node->action->markCancelled();
        return true;
    }
    // Body armour neither goes on nor comes off in direct combat.
    if (isArmorChangeRefused(*actor)) {
        const bool armorOn = id == 8 && slot == equipmentSlotMask(InventorySlots::body);
        auto body = actor->getEquippedItem(InventorySlots::body);
        const bool armorOff = id == 11 && body && body->id() == target->id;
        if (armorOn || armorOff) {
            node->action->markCancelled();
            reportArmorChangeRefused(_game, *actor, armorOff);
            return true;
        }
    }
    command->bindObjectReferences(_game);
    auto state = scheduledOwner(*actor);
    if (id == 8) {
        for (auto &pending : state->actions) {
            if (pending->saved.type != 6 || pending->saved.applied || !pending->node || !pending->node->action ||
                (pending->saved.inventorySlot != slot && pending->saved.target.id != target->id)) continue;
            // Replacing a scheduled item retains its slot in the queue and its timing.
            if (pending->saved.command && pending->saved.command->parameters.size() == 3)
                command->parameters[2] = pending->saved.command->parameters[2];
            auto replacement = command->toRuntimeAction(_game);
            if (!replacement) return false;
            pending->node->action->markCancelled();
            pending->node->action = std::move(replacement);
            pending->saved.target = *target;
            pending->saved.inventorySlot = slot;
            pending->saved.command = std::move(command);
            return true;
        }
        for (auto &queued : actor->actions().nodes) {
            if (queued->actionId != 8 || !queued->action || queued->action->isCompleted() || queued->action->isCancelled()) continue;
            auto previous = queued->action->saveFacingState();
            if (!previous || previous->parameters.size() != 3) continue;
            auto item = std::get_if<SavedObjectReference>(&previous->parameters[0].payload);
            auto oldSlot = std::get_if<int32_t>(&previous->parameters[1].payload);
            if (item && oldSlot && (item->id == target->id || static_cast<uint32_t>(*oldSlot) == slot)) {
                command->parameters[2] = previous->parameters[2];
                auto replacement = command->toRuntimeAction(_game);
                if (!replacement) return false;
                queued->action->markCancelled();
                queued->action = std::move(replacement);
                return true;
            }
        }
    } else {
        // An unequip of an item already waiting to come off, in the round or
        // in the queue, adds nothing. A queued one takes the new request's
        // instant flag as its container, so it takes nothing off either.
        const bool scheduled = std::any_of(state->actions.begin(), state->actions.end(), [&](const auto &pending) {
            return pending->saved.type == 7 && !pending->saved.applied && pending->node && pending->node->action &&
                   pending->saved.target.id == target->id;
        });
        UnequipItemAction *queued = nullptr;
        if (!scheduled) {
            for (const auto &entry : actor->actions().nodes) {
                if (entry->actionId != 11 || !entry->action || entry->action->isCompleted() ||
                    entry->action->isCancelled()) continue;
                auto *unequip = dyn_cast<UnequipItemAction>(entry->action.get());
                if (unequip && unequip->item() && unequip->item()->id() == target->id) {
                    queued = unequip;
                    break;
                }
            }
        }
        if (queued) queued->overwriteContainer(*flags);
        if (scheduled || queued) {
            node->action->markCancelled();
            return true;
        }
    }
    // A script equip or unequip waits for the round whether or not it is instant.
    if (!actor->isInCombat() || actor->combatActivationType() != CombatActivation::Direct) return false;
    auto entry = std::make_shared<ScheduledEntry>();
    entry->node = node;
    entry->saved.type = id == 8 ? 6 : 7;
    entry->saved.target = *target;
    entry->saved.inventorySlot = slot;
    if (id == 11) entry->saved.repository = std::get<SavedObjectReference>(command->parameters[1].payload);
    entry->saved.command = std::move(command);
    entry->referencesBound = entry->saved.bindObjectReferences(_game);
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        if (std::any_of(round->actions.begin(), round->actions.end(), [&](const auto &action) {
                return !action.retired && action.attacker.resolve() == actor;
            })) rescaleRound(*round, 1500, true);
    }
    state->actions.push_front(std::move(entry));
    ensureDispatcher(*actor);
    return true;
}

std::shared_ptr<Combat::ScheduledOwner> Combat::scheduledOwner(Creature &actor) {
    auto live = std::dynamic_pointer_cast<Creature>(_game.getObjectById(actor.id()));
    auto &state = _scheduled[actor.id()];
    if (!state || state->actor.resolve() != live) {
        state = std::make_shared<ScheduledOwner>();
        state->actor = live;
        for (const auto &round : _rounds) {
            if (round->state == CombatRound::Finished) continue;
            for (const auto &entry : round->actions)
                if (!entry.retired && entry.attacker.resolve() == live) state->time = round->time;
        }
    }
    return state;
}

static constexpr size_t kScheduledCapacity = 4;

bool Combat::appendScheduled(Creature &actor, std::shared_ptr<ScheduledEntry> entry, bool dispatcherToFront) {
    if (!addScheduled(actor, std::move(entry))) return false;
    ensureDispatcher(actor, dispatcherToFront);
    return true;
}

bool Combat::addScheduled(Creature &actor, std::shared_ptr<ScheduledEntry> entry) {
    auto owner = scheduledOwner(actor);
    // A round holds at most four pending entries; later requests are dropped.
    if (owner->actions.size() >= kScheduledCapacity) return false;
    if (entry->saved.command) {
        entry->referencesBound = entry->saved.bindObjectReferences(_game);
        if (auto action = entry->saved.command->toRuntimeAction(_game)) {
            entry->node = std::make_shared<ActionQueueNode>();
            entry->node->action = std::move(action);
            entry->node->actionId = entry->saved.command->actionId;
        }
    }
    owner->actions.push_back(std::move(entry));
    return true;
}

bool Combat::scheduleStance(Creature &actor, const std::shared_ptr<Object> &target) {
    auto entry = std::make_shared<ScheduledEntry>();
    entry->saved.type = 13;
    entry->saved.animation = 10001;
    entry->saved.animationTime = 3000;
    entry->saved.numAttacks = static_cast<int32_t>(CombatStance::TotalDefense);
    entry->saved.target = SavedObjectReference::fromRuntimeId(target ? target->id() : kSavedRuntimeInvalidObjectId);
    SavedActionRecord command;
    command.actionId = 68;
    command.parameters.push_back({3, entry->saved.target});
    command.declaredParameterCount = 1;
    entry->saved.command = std::move(command);
    return appendScheduled(actor, std::move(entry), true);
}

bool Combat::scheduleSwitchWeapons(Creature &actor) {
    auto entry = std::make_shared<ScheduledEntry>();
    entry->saved.type = 14;
    entry->saved.animationTime = 1000;
    entry->saved.target = SavedObjectReference::fromRuntimeId(kSavedRuntimeInvalidObjectId);
    SavedActionRecord command;
    command.actionId = 71;
    entry->saved.command = std::move(command);
    // A queued swap waits behind the commands already queued.
    return appendScheduled(actor, std::move(entry), false);
}

void Combat::scheduleCast(Creature &actor, const std::shared_ptr<Action> &cast) {
    std::shared_ptr<Object> target;
    if (auto *atObject = dyn_cast<CastSpellAtObjectAction>(cast.get())) target = atObject->target();
    const bool itemUse = isItemUse(*cast);
    auto entry = std::make_shared<ScheduledEntry>();
    entry->saved.type = itemUse ? 10 : 9;
    entry->saved.animation = itemUse ? 0 : 10017;
    entry->saved.animationTime = itemUse ? 0 : 500;
    entry->saved.target = SavedObjectReference::fromRuntimeId(target ? target->id() : kSavedRuntimeInvalidObjectId);
    // The entry names its command as a cast or an item use; the command itself
    // is the one given.
    SavedActionRecord command;
    command.actionId = itemUse ? 46 : 15;
    entry->saved.command = std::move(command);
    entry->node = std::make_shared<ActionQueueNode>();
    entry->node->action = cast;
    entry->node->actionId = itemUse ? 46 : 15;
    auto owner = scheduledOwner(actor);
    // A round holds at most four pending entries; later requests are dropped.
    if (owner->actions.size() < kScheduledCapacity) owner->actions.push_back(std::move(entry));
    // Out of combat an item use clears the creature's actions while an entry
    // is due; a cast does so only while none is.
    const bool due = !owner->actions.empty() &&
        owner->actions.front()->saved.timer <= static_cast<int>(owner->time * 1000.0f);
    if (!actor.isInCombat() && due == itemUse) clearControlledActions(actor);
    ensureDispatcher(actor, false);
}

void Combat::clearActions(Creature &actor) {
    // A creature that cannot be commanded keeps everything.
    actor.clearAllActions();
}

void Combat::clearControlledActions(Creature &actor) {
    if (&actor == _game.party().getLeader().get()) actor.clearAllActions(true);
}

void Combat::clearAllOrders(Creature &actor) {
    // The actions go as a forced ClearAllActions takes them, the one its
    // round has taken up included; the pending entries, the targets and the
    // spell targets go whether or not the creature can be commanded.
    actor.clearAllActions(true);
    discardEquipment(actor);
    actor.clearRoundTarget();
    actor.setAttemptedAttackTarget(script::kObjectInvalid);
    actor.clearCurrentAttackTarget();
    actor.setAttemptedSpellTarget(script::kObjectInvalid);
    actor.spellScriptContext().clearActiveTarget();
}

std::shared_ptr<Combat::ScheduledEntry> Combat::makeAttackEntry(const std::shared_ptr<Object> &target, FeatType feat,
                                                                const CutsceneAttack *cutscene) const {
    if (!target) return nullptr;
    // An ordinary attack needs a living target, and a closed door.
    if (!cutscene) {
        if (auto door = dyn_cast<Door>(target.get())) {
            if (door->isOpen() || door->currentHitPoints() <= 0) return nullptr;
        } else if (isa<Placeable>(target.get())) {
            if (target->currentHitPoints() <= 0) return nullptr;
        } else if (target->isDead()) {
            return nullptr;
        }
    }
    const CutsceneAttack forced = cutscene ? *cutscene : CutsceneAttack {};
    auto entry = std::make_shared<ScheduledEntry>();
    entry->saved.type = feat != FeatType::Invalid ? 11 : 1;
    entry->saved.animation = 10009;
    entry->saved.animationTime = 1500;
    entry->saved.numAttacks = cutscene ? 0 : 1;
    entry->saved.target = SavedObjectReference::fromRuntimeId(target->id());
    SavedActionRecord command;
    command.actionId = 12;
    command.declaredParameterCount = 10;
    command.parameters = {
        {1, int32_t {cutscene ? 1 : 0}},
        {3, entry->saved.target},
        {1, int32_t {1}},
        {1, int32_t {10009}},
        {1, int32_t {1500}},
        {1, int32_t {entry->saved.numAttacks}},
        {1, static_cast<int32_t>(feat)},
        {1, int32_t {forced.animation}},
        {1, int32_t {forced.result}},
        {1, int32_t {forced.damage}},
    };
    entry->saved.command = std::move(command);
    return entry;
}

std::shared_ptr<Action> Combat::scheduleAttack(Creature &actor, const std::shared_ptr<Object> &target,
                                               FeatType feat, const CutsceneAttack *cutscene) {
    auto entry = makeAttackEntry(target, feat, cutscene);
    // The entry is added first, up to the round's four, and a target it
    // accepts becomes the attempted target if there is none, even when the
    // entry did not fit; then, out of combat, the controlled creature drops
    // what it was doing, but not its pending entries; then the dispatcher goes
    // in, whether or not there was an entry. It goes in front of a TSL player
    // attack; KotOR and cutscene attacks append it.
    const bool toFront = _game.isTSL() && !cutscene;
    const auto scheduled = entry;
    const bool added = entry && addScheduled(actor, std::move(entry));
    if (scheduled && actor.getAttemptedAttackTarget() == script::kObjectInvalid)
        actor.setAttemptedAttackTarget(target->id());
    if (!actor.isInCombat()) clearControlledActions(actor);
    ensureDispatcher(actor, toFront);
    if (!added) return nullptr;
    return scheduled->node ? scheduled->node->action : nullptr;
}

// The move closes to a fifth of a metre and is forced to its end after a
// tenth of a second.
static constexpr float kCutsceneMoveRange = 0.2f;
static constexpr float kCutsceneMoveTimeout = 0.1f;
static constexpr int32_t kTimedMoveFlag = 4;

void Combat::scheduleCutsceneMove(Creature &actor, const std::shared_ptr<Object> &object, const glm::vec3 &point, bool run) {
    auto entry = std::make_shared<ScheduledEntry>();
    entry->saved.type = 12;
    entry->saved.target = SavedObjectReference::fromRuntimeId(object ? object->id() : kSavedRuntimeInvalidObjectId);
    // The point and the run flag travel in a move record.
    const auto area = _game.module() ? _game.module()->area() : nullptr;
    SavedActionRecord command;
    command.actionId = 1;
    command.declaredParameterCount = 13;
    command.parameters = {
        {2, point.x},
        {2, point.y},
        {2, point.z},
        {3, SavedObjectReference::fromRuntimeId(area ? area->id() : kSavedRuntimeInvalidObjectId)},
        {3, entry->saved.target},
        {1, int32_t {(run ? 1 : 0) | kTimedMoveFlag}},
        {2, kCutsceneMoveRange},
        {1, int32_t {0}},
        {2, kCutsceneMoveTimeout},
        {2, 0.0f},
        {2, 0.0f},
        {1, int32_t {0}},
        {1, int32_t {0}},
    };
    entry->saved.command = std::move(command);
    // The entry goes in first, up to the round's four; then, out of combat,
    // the controlled creature drops what it was doing; then the dispatcher is
    // appended.
    addScheduled(actor, std::move(entry));
    if (!actor.isInCombat()) clearControlledActions(actor);
    ensureDispatcher(actor, false);
}

void Combat::dispatchCutsceneMove(Creature &actor, const ScheduledEntry &entry) {
    if (!entry.saved.command || entry.saved.command->parameters.size() != 13) return;
    const auto &parameters = entry.saved.command->parameters;
    const auto *x = std::get_if<float>(&parameters[0].payload);
    const auto *y = std::get_if<float>(&parameters[1].payload);
    const auto *z = std::get_if<float>(&parameters[2].payload);
    const auto *flags = std::get_if<int32_t>(&parameters[5].payload);
    if (!x || !y || !z || !flags) return;
    const bool run = (*flags & 1) != 0;
    const auto object = entry.saved.target.boundObject();
    if (object) {
        actor.addActionOnTop(_game.newAction<MoveToObjectAction>(
            object, run, kCutsceneMoveRange, true, kCutsceneMoveTimeout, false, kCutsceneMoveRange));
    } else {
        actor.addActionOnTop(_game.newAction<MoveToLocationAction>(
            std::make_shared<Location>(glm::vec3(*x, *y, *z), actor.getFacing()), run, true, kCutsceneMoveTimeout,
            kCutsceneMoveRange));
    }
}

std::shared_ptr<Action> Combat::addRoundAttack(Creature &actor, const std::shared_ptr<Object> &target) {
    auto entry = makeAttackEntry(target, FeatType::Invalid, nullptr);
    if (!entry) return nullptr;
    const auto scheduled = entry;
    // The target becomes the attempted target if there is none, even when the
    // entry does not fit.
    const bool added = addScheduled(actor, std::move(entry));
    if (actor.getAttemptedAttackTarget() == script::kObjectInvalid) actor.setAttemptedAttackTarget(target->id());
    if (!added) return nullptr;
    return scheduled->node ? scheduled->node->action : nullptr;
}

void Combat::takeNewAttackTarget(Creature &attacker, const std::shared_ptr<Creature> &target) {
    attacker.setOrientationLock(target->id());
    attacker.setAttemptedAttackTarget(target->id());
    auto found = _scheduled.find(attacker.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &attacker) return;
    const auto reference = SavedObjectReference::fromRuntimeId(target->id());
    for (auto &pending : found->second->actions) {
        // A feat attack keeps its target.
        if (pending->saved.type != 1 || pending->saved.retargettable != 1 || !pending->saved.command ||
            pending->saved.command->parameters.size() < 2 || !pending->node || !pending->node->action) continue;
        auto *attack = dyn_cast<AttackObjectAction>(pending->node->action.get());
        if (!attack) continue;
        // The attack stays what it was, the player's order or the round's
        // continuation, and is only aimed anew.
        attack->retarget(target);
        pending->saved.command->parameters[1].payload = reference;
        pending->saved.target = reference;
        pending->referencesBound = pending->saved.bindObjectReferences(_game);
    }
}

bool Combat::removeLastScheduled(Creature &actor) {
    auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor) return false;
    auto &actions = found->second->actions;
    // An entry already under way is no longer pending.
    if (actions.empty() || actions.back()->saved.applied) return false;
    const auto entry = actions.back();
    actions.pop_back();
    if (entry->node && entry->node->action) entry->node->action->markCancelled();
    return true;
}

void Combat::removeAllScheduled(Creature &actor) {
    while (removeLastScheduled(actor)) {}
}

// An item use is handed on unless it is aimed at a creature that has died.
// A hostile item spell takes its user out of stealth. An item spell the
// hostile menu does not list, aimed at another object, is used at the use's
// location instead; in TSL only a hostile spell, and never a grenade's or a
// rocket's.
bool Combat::dispatchItemUse(Creature &actor, ScheduledEntry &entry) {
    auto *atObject = dyn_cast<CastSpellAtObjectAction>(entry.node->action.get());
    const auto target = atObject ? atObject->target() : nullptr;
    if (auto *creature = dyn_cast<Creature>(target.get()); creature && creature->isRuntimeLive() && creature->isDead())
        return false;
    const auto *atLocation = atObject ? nullptr : dyn_cast<CastSpellAtLocationAction>(entry.node->action.get());
    const auto &item = atObject ? atObject->item() : atLocation->item();
    const auto property = atObject ? atObject->itemProperty() : atLocation->itemProperty();
    if (!item || !*item || !(*item)->isRuntimeLive() || !property || *property >= (*item)->properties().size())
        return true;
    const auto propertySpell = static_cast<SpellType>((*item)->properties()[*property].subtype);
    const auto listed = _services.game.spells.get(propertySpell);
    const auto spell = _services.game.spells.get(itemSpellForActor(actor, propertySpell));
    const bool hostile = spell && spell->hostile;
    if (hostile) actor.setStealthMode(false);
    if ((listed && listed->hostileSlot > 0) || !target || target.get() == &actor) return true;
    if (_game.isTSL()) {
        const int itemType = (*item)->itemType();
        if (!hostile || itemType == 6 || itemType == 49) return true;
    }
    entry.node->action = atObject->toLocationUse();
    entry.saved.target = SavedObjectReference::fromRuntimeId(kSavedRuntimeInvalidObjectId);
    return true;
}

// TSL powers whose cast, handed on by the dispatcher, leaves its caster in stealth.
static bool castKeepsStealth(const Game &game, const Action &cast) {
    if (!game.isTSL()) return false;
    std::shared_ptr<Spell> spell;
    if (cast.type() == ActionType::CastSpellAtObject) spell = static_cast<const CastSpellAtObjectAction &>(cast).spell();
    else if (cast.type() == ActionType::CastSpellAtLocation) spell = static_cast<const CastSpellAtLocationAction &>(cast).spell();
    if (!spell) return false;
    static constexpr std::array<int, 6> kStealthPowers {181, 182, 184, 200, 201, 269};
    return std::find(kStealthPowers.begin(), kStealthPowers.end(), static_cast<int>(spell->type)) != kStealthPowers.end();
}

void Combat::dispatchScheduled(Creature &actor, const ScheduledEntry &entry) {
    actor.setRoundActionKind(entry.saved.type);
    if (entry.saved.type == 13) {
        actor.addStanceActions(static_cast<CombatStance>(entry.saved.numAttacks),
                               entry.saved.target.boundObject(), false, true, true);
    } else if (entry.saved.type == 14) {
        actor.addSwitchWeaponsAction(true);
    }
}

void Combat::rescaleRound(CombatRound &round, int milliseconds, bool force) {
    int length = static_cast<int>(round.duration * 1000.0f);
    if (length < milliseconds) {
        if (milliseconds - length <= 1000) length = milliseconds + 1;
        else if (force) length = milliseconds;
    }
    const int next = length - milliseconds;
    for (auto &[id, owner] : _scheduled) {
        const auto actor = owner->actor.resolve();
        if (!actor || !std::any_of(round.actions.begin(), round.actions.end(), [&](const auto &entry) {
                return !entry.retired && entry.attacker.resolve() == actor;
            })) continue;
        for (auto &entry : owner->actions)
            entry->saved.timer = length > 0 ? static_cast<int>(static_cast<float>(entry->saved.timer) / length * next) : 0;
        owner->time = length > 0 ? owner->time / length * next : 0.0f;
    }
    const int elapsed = static_cast<int>(round.time * 1000.0f);
    const int scaled = length ? static_cast<int>(static_cast<float>(elapsed) / length * next) : next;
    round.time = scaled / 1000.0f;
    round.duration = scaled < 0 ? 0.0f : std::max(0, next) / 1000.0f;
}

// The round dispatcher is the creature's current action.
static bool dispatcherHeadsQueue(const Creature &actor) {
    const auto &nodes = actor.actions().nodes;
    if (nodes.empty() || nodes.front()->actionId != 63) return false;
    const auto &head = nodes.front()->action;
    return head && !head->isCompleted() && !head->isCancelled();
}

// Having handed on an entry, the dispatcher waits behind every queued command.
static void moveDispatcherBehind(Creature &actor) {
    for (const auto &node : actor.actions().nodes) {
        if (node->actionId == 63 && node->action && !node->action->isCompleted() && !node->action->isCancelled()) {
            actor.moveActionNodeToBack(node);
            return;
        }
    }
}

bool Combat::refusesAttackOnFriend(const Creature &attacker, const Object &target) {
    const auto *creature = dyn_cast<const Creature>(&target);
    const int state = attacker.effectState();
    return creature && creature->getReputationToward(attacker) > 89 && state != 1 && state != 16;
}

bool Combat::refusesUnperceivedTarget(const Creature &caster, Action &cast) {
    auto *atObject = dyn_cast<CastSpellAtObjectAction>(&cast);
    if (!atObject || (!atObject->item() && atObject->fake())) return false;
    auto *target = dyn_cast<Creature>(atObject->target().get());
    if (!target || target == &caster || caster.isPartyMember()) return false;
    const auto &perception = caster.perception();
    const uint32_t id = target->id();
    return !perception.has(id) || (perception.isInvisible(id) && !perception.sees(id));
}

bool Combat::refusesAttack(const Creature &attacker) {
    if (attacker.isCommandable()) return false;
    const int state = attacker.effectState();
    return !attacker.game().isTSL() || (state != 1 && state != 16);
}

bool Combat::awaitsRoundEnd(const Creature &actor, const Action *self) const {
    // A creature held while it owns no round waits as well.
    if (isOwnerPaused(actor)) return true;
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (const auto &entry : round->actions) {
            if (entry.retired || entry.attacker.resolve().get() != &actor) continue;
            if (self && (entry.action.get() == self || &entry.action->combatAction() == self)) continue;
            if (entry.action->combatAction().tookRoundAction()) return true;
            // A round that a finished command began holds the next one while
            // it is paused.
            if (entry.action->isCompleted() && round->pauseRemaining > 0.0f) return true;
        }
    }
    return false;
}

bool Combat::blocksOwnerActions(const Creature &actor) const {
    if (!actor.actions().nodes.empty()) {
        const auto &head = actor.actions().nodes.front()->action;
        if (head && head->isScheduledCommand() && !head->isCompleted() && !head->isCancelled()) return false;
    }
    const auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor || found->second->actions.empty()) return false;
    const auto &head = found->second->actions.front();
    if (head->saved.applied) return true;
    // A due entry waits for the dispatcher, and a round action for the end of
    // the round its creature has acted in; whatever runs ahead of it goes on.
    if (head->saved.type != 3 && (!dispatcherHeadsQueue(actor) ||
        (!head->saved.isEquipment() && awaitsRoundEnd(actor, nullptr)))) return false;
    return head->saved.timer <= static_cast<int>(found->second->time * 1000.0f);
}

void Combat::updateEquipment(float dt) {
    _equipmentTime.clear();
    std::vector<std::shared_ptr<ScheduledOwner>> owners;
    for (const auto &[id, owner] : _scheduled) owners.push_back(owner);
    for (const auto &owner : owners) {
        auto actor = owner->actor.resolve();
        if (!actor) {
            for (auto it = _scheduled.begin(); it != _scheduled.end();) {
                if (it->second == owner) it = _scheduled.erase(it);
                else ++it;
            }
            continue;
        }
        if (actor->isDead()) { discardEquipment(*actor); continue; }
        // A creature held by a time stop keeps its schedule where it is.
        if (_game.isFrozenByTimeStop(*actor)) continue;
        if (!actor->actions().nodes.empty()) {
            const auto &head = actor->actions().nodes.front()->action;
            if (head && head->isScheduledCommand() && !head->isCompleted() && !head->isCancelled()) continue;
        }
        // A knocked-down creature's schedule stands still.
        if (actor->isKnockedDown()) continue;
        float externalPause = 0.0f;
        for (const auto &round : _rounds) {
            if (round->state == CombatRound::Finished || round->pauseRemaining <= 0.0f) continue;
            if (std::any_of(round->actions.begin(), round->actions.end(), [&](const auto &entry) {
                    return !entry.retired && entry.attacker.resolve() == actor;
                })) externalPause = std::max(externalPause, round->pauseRemaining);
        }
        if (const auto paused = _ownerPauses.find(actor->id()); paused != _ownerPauses.end())
            externalPause = std::max(externalPause, paused->second.remaining);
        if (externalPause >= dt) continue;
        float frame = std::max(0.0f, dt - externalPause);
        while (!owner->actions.empty()) {
            auto entry = owner->actions.front();
            if (!std::isfinite(entry->saved.remainingPause) || entry->saved.remainingPause < 0.0f ||
                (entry->saved.applied && (!entry->saved.isEquipment() && entry->saved.type != 3))) {
                if (!entry->refusalReported) {
                    warn("Scheduled action state cannot execute", LogChannel::Combat);
                    entry->refusalReported = true;
                }
                _equipmentTime[actor->id()] += frame;
                frame = 0.0f;
                break;
            }
            if (!entry->saved.applied) {
                const float wait = std::max(0.0f, entry->saved.timer / 1000.0f - owner->time);
                const float used = std::min(frame, wait);
                owner->time += used;
                frame -= used;
                if (used < wait) break;
                // Only the dispatcher, running at the head of the queue, takes an entry,
                // and it takes no round action while its creature's round action is spent.
                if (entry->saved.type != 3 && (!dispatcherHeadsQueue(*actor) ||
                    (!entry->saved.isEquipment() && awaitsRoundEnd(*actor, nullptr)))) break;
                if (entry->saved.type == 3) {
                    entry->saved.applied = true;
                    entry->saved.remainingPause = std::max(0, entry->saved.animationTime) / 1000.0f;
                    actor->setRoundActionKind(3);
                    actor->setStealthMode(false);
                } else if (entry->saved.type == 12) {
                    // A cutscene move is taken whether or not its object is
                    // still there. One restored without its point moves
                    // nowhere.
                    owner->actions.pop_front();
                    actor->setRoundActionKind(12);
                    actor->setStealthMode(false);
                    dispatchCutsceneMove(*actor, *entry);
                    moveDispatcherBehind(*actor);
                    frame = 0.0f;
                    break;
                } else if ((entry->saved.type == 1 || entry->saved.type == 11) &&
                           (!entry->node || !entry->node->action || !entry->node->action->runtimeDependenciesLive() ||
                            !actor->permitsAction(*entry->node->action) || refusesAttack(*actor) ||
                            (entry->saved.target.boundObject() &&
                             refusesAttackOnFriend(*actor, *entry->saved.target.boundObject())))) {
                    // An attack that cannot be taken, taken by a creature that
                    // cannot be commanded, or aimed at a friend, is used up
                    // without an attack. One taken up by a creature no longer
                    // able to attack fails at once and ends its combat modes.
                    const auto target = entry->saved.target.boundObject();
                    if (entry->node && entry->node->action && entry->node->action->runtimeDependenciesLive() &&
                        !refusesAttack(*actor) && !(target && refusesAttackOnFriend(*actor, *target)) &&
                        !actor->permitsAction(*entry->node->action)) {
                        actor->cancelAllCombatModes();
                    }
                    owner->actions.pop_front();
                    moveDispatcherBehind(*actor);
                    frame = 0.0f;
                    break;
                } else if ((entry->saved.type == 9 || entry->saved.type == 10) &&
                           (!entry->referencesBound || !entry->node || !entry->node->action)) {
                    // A restored cast whose target or item is gone is used up
                    // without casting.
                    owner->actions.pop_front();
                    moveDispatcherBehind(*actor);
                    frame = 0.0f;
                    break;
                } else if (entry->saved.isEquipment() &&
                           (!entry->referencesBound ||
                            (entry->node && entry->node->action && !entry->node->action->runtimeDependenciesLive()))) {
                    // An equip or unequip whose item or container is gone is
                    // still taken: its creature leaves stealth and holds its
                    // round for the entry's pause, but nothing is equipped or
                    // taken off.
                    entry->saved.applied = true;
                    entry->saved.remainingPause = std::max(0, entry->saved.animationTime) / 1000.0f;
                    actor->setRoundActionKind(entry->saved.type);
                    actor->setStealthMode(false);
                    actor->setMovementType(Creature::MovementType::None);
                    continue;
                } else if (!entry->referencesBound || !scheduledCommandMatches(entry->saved) || !entry->node || !entry->node->action) {
                    if (!entry->refusalReported) {
                        warn("Scheduled action cannot execute: " + std::to_string(entry->saved.type), LogChannel::Combat);
                        entry->refusalReported = true;
                    }
                    _equipmentTime[actor->id()] += frame;
                    frame = 0.0f;
                    break;
                }
                if (entry->saved.type != 3) {
                    auto action = entry->node->action;
                    const bool cast = entry->saved.type == 9 || entry->saved.type == 10;
                    if (!cast && (!action->runtimeDependenciesLive() || !actor->permitsAction(*action))) {
                        if (!entry->refusalReported) {
                            warn("Scheduled action is not currently executable", LogChannel::Combat);
                            entry->refusalReported = true;
                        }
                        _equipmentTime[actor->id()] += frame;
                        frame = 0.0f;
                        break;
                    }
                    if (entry->saved.type == 13 || entry->saved.type == 14) {
                        owner->actions.pop_front();
                        // Every dispatched kind but the stance leaves stealth.
                        if (entry->saved.type == 14) actor->setStealthMode(false);
                        dispatchScheduled(*actor, *entry);
                        moveDispatcherBehind(*actor);
                        frame = 0.0f;
                        break;
                    }
                    if (!entry->saved.isEquipment()) {
                        actor->setRoundActionKind(entry->saved.type);
                        const bool attack = entry->saved.type == 1 || entry->saved.type == 11;
                        if (attack || (entry->saved.type == 9 && !castKeepsStealth(_game, *action)))
                            actor->setStealthMode(false);
                        if (cast) {
                            // An item use aimed at a creature that has died is
                            // dropped, and the dispatcher stays at the head.
                            if (entry->saved.type == 10 && !dispatchItemUse(*actor, *entry)) {
                                owner->actions.pop_front();
                                frame = 0.0f;
                                break;
                            }
                            // A cast handed on starts without spell targets. One
                            // whose target or item is gone, taken by a creature
                            // that cannot be commanded, or aimed at a creature
                            // its caster does not perceive, is used up without
                            // casting.
                            actor->setAttemptedSpellTarget(script::kObjectInvalid);
                            actor->spellScriptContext().clearActiveTarget();
                            if (!actor->isCommandable() || !entry->node->action->runtimeDependenciesLive() ||
                                refusesUnperceivedTarget(*actor, *entry->node->action)) {
                                owner->actions.pop_front();
                                moveDispatcherBehind(*actor);
                                frame = 0.0f;
                                break;
                            }
                            action = entry->node->action;
                        }
                        // Taking an attack entry queues its attack as the attack command does.
                        if (attack) {
                            if (auto target = entry->saved.target.boundObject()) actor->recordQueuedAttack(*target);
                        }
                        owner->actions.pop_front();
                        action->setScheduledCommand(true);
                        actor->requeueActionNode(entry->node);
                        moveDispatcherBehind(*actor);
                        frame = 0.0f;
                        break;
                    }
                    entry->saved.applied = true;
                    entry->saved.remainingPause = std::max(0, entry->saved.animationTime) / 1000.0f;
                    actor->setRoundActionKind(entry->saved.type);
                    actor->setStealthMode(false);
                    actor->setMovementType(Creature::MovementType::None);
                    action->execute(action, *actor, 0.0f);
                    const auto active = _scheduled.find(actor->id());
                    if (active == _scheduled.end() || active->second != owner || !actor->isRuntimeLive()) break;
                    if (owner->actions.empty() || owner->actions.front() != entry) continue;
                }
            }
            const float used = std::min(frame, entry->saved.remainingPause);
            entry->saved.remainingPause -= used;
            frame -= used;
            _equipmentTime[actor->id()] += used;
            if (entry->saved.remainingPause > 0.0f) break;
            owner->actions.pop_front();
            readyAfterScheduledPause(*actor);
            if (frame <= 0.0f) break;
        }
        owner->time += frame;
        const auto active = _scheduled.find(actor->id());
        if (active != _scheduled.end() && active->second == owner && owner->actions.empty()) _scheduled.erase(active);
    }
}

void Combat::transferEquipment(Creature &actor) {
    const auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor) return;
    const auto owner = found->second;
    const std::vector<std::shared_ptr<ScheduledEntry>> pending(owner->actions.begin(), owner->actions.end());
    // Reverse traversal with front insertion preserves the remaining schedule order.
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        const auto &entry = *it;
        if (entry->saved.applied || !entry->saved.isEquipment() || !entry->referencesBound ||
            !entry->node || !entry->node->action || !entry->node->action->runtimeDependenciesLive()) continue;
        auto position = std::find(owner->actions.begin(), owner->actions.end(), entry);
        if (position == owner->actions.end()) continue;
        owner->actions.erase(position);
        actor.requeueActionNode(entry->node);
    }
    if (owner->actions.empty()) _scheduled.erase(found);
}

void Combat::discardEquipment(Object &actor) {
    const auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor) return;
    auto owner = found->second;
    _scheduled.erase(found);
    for (const auto &entry : owner->actions) {
        if (entry->node && entry->node->action && !entry->saved.applied) {
            auto action = entry->node->action;
            action->markCancelled();
            // A pending cast has not reached its creature's queue, so nothing
            // of it is cleared from the creature.
            if (entry->saved.type != 9 && entry->saved.type != 10) action->cancel(action, actor);
        }
    }
}

std::vector<SavedScheduledAction> Combat::saveScheduled(const Object &actor) const {
    std::vector<SavedScheduledAction> result;
    const auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor) return result;
    for (const auto &entry : found->second->actions) {
        auto saved = entry->saved;
        if (entry->node && entry->node->action) {
            if (auto command = entry->node->action->saveFacingState()) {
                command->groupActionId = entry->node->groupId;
                // A pending attack entry keeps no cutscene values: it comes
                // back as an ordinary attack.
                if ((saved.type == 1 || saved.type == 11) && command->actionId == 12 &&
                    command->parameters.size() == 10) {
                    command->parameters[0].payload = int32_t {0};
                    command->parameters[5].payload = int32_t {1};
                    command->parameters[7].payload = int32_t {0};
                    command->parameters[8].payload = int32_t {4};
                    command->parameters[9].payload = int32_t {0};
                }
                saved.command = std::move(command);
            } else if (saved.type == 1 || saved.type == 11) {
                // An attack whose target is gone is not kept.
                continue;
            } else if (!saved.applied) {
                throw ValidationException("Scheduled equipment action has no save representation");
            }
        }
        result.push_back(std::move(saved));
    }
    return result;
}

float Combat::scheduledTime(const Object &actor) const {
    const auto found = _scheduled.find(actor.id());
    return found != _scheduled.end() && found->second->actor.resolve().get() == &actor ? found->second->time : 0.0f;
}

void Combat::restoreScheduled(const std::shared_ptr<Creature> &actor,
                              const std::vector<SavedScheduledAction> &records,
                              const std::vector<bool> &bound, float time) {
    if (!actor || records.empty()) return;
    auto state = std::make_shared<ScheduledOwner>();
    state->actor = actor;
    state->time = time;
    for (size_t index = 0; index < records.size(); ++index) {
        auto entry = std::make_shared<ScheduledEntry>();
        entry->saved = records[index];
        entry->referencesBound = index < bound.size() && bound[index];
        if (entry->referencesBound && entry->saved.command && scheduledCommandMatches(entry->saved)) {
            auto action = entry->saved.command->toRuntimeAction(_game);
            if (action) {
                entry->node = std::make_shared<ActionQueueNode>();
                entry->node->action = std::move(action);
                entry->node->actionId = entry->saved.command->actionId;
                entry->node->groupId = entry->saved.command->groupActionId;
            }
        }
        // Even unexecutable records retain their ordered head position and payload.
        state->actions.push_back(std::move(entry));
    }
    _scheduled[actor->id()] = std::move(state);
    actor->setCombatState(true);
}

void Combat::cancelActions(Object &actor) {
    discardEquipment(actor);
    std::vector<std::shared_ptr<Action>> actions;
    for (const auto &round : _rounds) {
        for (const auto &entry : round->actions) {
            if (entry.attacker.resolve().get() == &actor &&
                std::find(actions.begin(), actions.end(), entry.action) == actions.end())
                actions.push_back(entry.action);
        }
    }
    // An action that cannot be cleared runs on and keeps the pauses it set.
    const bool kept = std::any_of(actions.begin(), actions.end(), [](const auto &action) {
        return !action->isCompleted() && !action->isCancelled() && !action->isClearable();
    });
    if (auto *creature = dyn_cast<Creature>(&actor); creature && !kept) releasePartner(*creature);
    for (const auto &action : actions) {
        if (action->isCompleted() || action->isCancelled() || !action->isClearable()) continue;
        action->markCancelled();
        action->cancel(action, actor);
    }
}

void Combat::releasePartner(Creature &actor) {
    for (const auto &round : _rounds) {
        if (round->pauseOwner.resolve().get() == &actor) {
            round->pauseOwner.reset();
            round->pauseRemaining = 0.0f;
        }
        if (round->master.resolve().get() == &actor || round->engaged.resolve().get() == &actor) {
            round->master.reset();
            round->engaged.reset();
            round->duel = false;
        }
    }
}

void Combat::finishOwner(Creature &actor, int runEndRound, bool suppressScript) {
    actor.finishCombatRound();
    // A round target that is down, or a door or container now open, is forgotten.
    if (auto target = actor.getRoundTarget()) {
        const auto *creature = dyn_cast<Creature>(target.get());
        const auto *door = dyn_cast<Door>(target.get());
        const auto *placeable = dyn_cast<Placeable>(target.get());
        if ((creature && isUnavailableAttackTarget(*creature)) ||
            (door && door->state() != DoorState::Closed) || (placeable && placeable->isOpen()))
            actor.clearRoundTarget();
    }
    // An attempted target that is gone or dead is forgotten.
    if (const auto attempted = actor.getAttemptedAttackTarget(); attempted != script::kObjectInvalid) {
        const auto object = _game.getObjectById(attempted);
        if (!object || object->isDead()) actor.setAttemptedAttackTarget(script::kObjectInvalid);
    }
    const auto spellTarget = actor.spellScriptContext().activeTarget();
    if (!spellTarget || spellTarget->isDead()) actor.spellScriptContext().clearActiveTarget();
    actor.setMovementRestricted(false);
    actor.setMovementType(Creature::MovementType::None);
    // Every round end shows the pause or ready pose, cutscene rounds included.
    if (!actor.isDead() && !actor.isTemporarilyDead()) actor.showPauseReadyAnimation(false);
    const auto leader = _game.party().getLeader();
    // TSL also withholds the script from a player character at zero vitality.
    if (runEndRound && !suppressScript && actor.currentSerializedActionId() != 1 &&
        leader.get() != &actor && !actor.isDead() &&
        (!_game.isTSL() || !actor.isPC() || actor.currentHitPoints() > 0))
        actor.runEndRoundScript();
    // The controlled leader's round end can pause play while it is fighting.
    if (runEndRound && leader.get() == &actor && actor.clientCombatMode())
        _game.requestAutoPause(AutoPauseReason::EndOfCombatRound);
}

void Combat::endRound(Creature &actor, int runEndRound) {
    std::vector<std::pair<CombatRound::RoundAction, bool>> finished;
    bool suppressed = false;
    actor.refreshBodyFuel();
    for (const auto &round : _rounds) {
        for (auto &entry : round->actions) {
            if (entry.attacker.resolve().get() != &actor || entry.retired) continue;
            const bool cutscene = isCutsceneRoundFor(*round, entry);
            finished.emplace_back(entry, cutscene);
            suppressed |= entry.action->combatAction().suppressesEndRoundScript() || cutscene;
            entry.retired = true;
            entry.action->retireCombatRound();
            entry.target.reset();
        }
    }
    releasePartner(actor);
    transferEquipment(actor);
    finishOwner(actor, runEndRound, suppressed);
    if (runEndRound) for (const auto &[entry, cutscene] : finished) continueOwner(entry, cutscene);
}

void Combat::update(float dt) {
    pruneInvalidRounds();
    updateEquipment(dt);
    // A pause held while owning no round ends like any round pause.
    for (auto it = _ownerPauses.begin(); it != _ownerPauses.end();) {
        auto actor = it->second.actor.resolve();
        if (actor && _game.isFrozenByTimeStop(*actor)) {
            ++it;
            continue;
        }
        it->second.remaining -= dt;
        if (actor && it->second.remaining > 0.0f) {
            ++it;
            continue;
        }
        it = _ownerPauses.erase(it);
        if (actor) readyAfterRoundPause(*actor, false);
    }
    const RoundQueue rounds = _rounds;
    for (const auto &round : rounds) {
        if (std::find(_rounds.begin(), _rounds.end(), round) == _rounds.end()) continue;
        // A round waits while any of its attackers is held by a time stop.
        if (std::any_of(round->actions.begin(), round->actions.end(), [this](const auto &entry) {
                auto attacker = entry.attacker.resolve();
                return !entry.retired && attacker && _game.isFrozenByTimeStop(*attacker);
            })) continue;
        float equipmentPause = 0.0f;
        for (const auto &entry : round->actions) {
            if (auto actor = entry.attacker.resolve(); actor && !entry.retired)
                equipmentPause = std::max(equipmentPause, _equipmentTime[actor->id()]);
        }
        updateRound(*round, std::max(0.0f, dt - equipmentPause));
    }
    pruneInvalidRounds();
}

static bool isActionFinished(const CombatRound::RoundAction &action) {
    return action.retired || action.action->isCompleted() || action.action->isCancelled();
}

void Combat::pruneInvalidRounds() {
    const RoundQueue rounds = _rounds;
    for (const auto &round : rounds) {
        if (std::find(_rounds.begin(), _rounds.end(), round) == _rounds.end()) continue;
        // Retire invalid owners independently; a live partner keeps its command.
        std::vector<CombatRound::RoundAction> abandoned;
        for (auto &entry : round->actions) {
            if (entry.retired) continue;
            if (!entry.action->isCompleted() && (entry.action->isCancelled() ||
                !entry.participantBindingsLive() || !entry.remainsInActorQueue() ||
                !entry.action->runtimeDependenciesLive())) {
                abandoned.push_back(entry);
                entry.retired = true;
            }
        }
        for (const auto &entry : abandoned) {
            entry.action->retireCombatRound();
            auto actor = entry.attacker.resolve();
            if (actor) {
                releasePartner(*actor);
                if (!entry.action->isCancelled()) entry.action->cancel(entry.action, *actor);
            }
            entry.action->markCancelled();
            entry.action->complete();
            // A cleared command does not cancel the round: its owner still
            // gets the round's end.
            if (actor && actor->isRuntimeLive() && !ownsRound(*actor))
                finishOwner(*actor, 1, entry.action->combatAction().suppressesEndRoundScript() ||
                                       isCutsceneRoundFor(*round, entry));
        }
        const bool retired = std::all_of(round->actions.begin(), round->actions.end(), [](const auto &e) { return e.retired; });
        if (retired || (round->state == CombatRound::Finished &&
            std::all_of(round->actions.begin(), round->actions.end(), isActionFinished))) {
            auto position = std::find(_rounds.begin(), _rounds.end(), round);
            if (position != _rounds.end()) _rounds.erase(position);
        }
    }
}

void Combat::updateRound(CombatRound &round, float dt) {
    if (round.state == CombatRound::Finished) return;
    // A round whose every owner is knocked down stands still.
    bool owned = false;
    bool standing = false;
    for (const auto &entry : round.actions) {
        auto actor = entry.retired ? nullptr : entry.attacker.resolve();
        if (!actor) continue;
        owned = true;
        standing = standing || !actor->isKnockedDown();
    }
    if (owned && !standing) return;
    float advance = std::max(0.0f, dt);
    // Whoever's round pause runs out comes back to its pose, after any round
    // end the leftover time brings.
    std::vector<std::pair<std::shared_ptr<Creature>, bool>> posing;
    if (round.pauseRemaining > 0.0f) {
        const float used = std::min(advance, round.pauseRemaining);
        round.pauseRemaining -= used;
        advance -= used;
        if (round.pauseRemaining > 0.0f) return;
        round.pauseOwner.reset();
        for (const auto &entry : round.actions) {
            if (entry.retired) continue;
            if (auto actor = entry.attacker.resolve()) posing.emplace_back(std::move(actor), isCutsceneRoundFor(round, entry));
        }
    }
    round.time += advance;
    if (round.state == CombatRound::Pending) round.state = CombatRound::FirstAction;
    // A timer driven below zero by a rescale ends the round at once.
    const bool overdrawn = round.time < 0.0f;
    if (round.state == CombatRound::FirstAction && (overdrawn || round.time >= 0.5f * round.duration))
        round.state = CombatRound::SecondAction;
    bool held = false;
    bool targetDown = false;
    for (const auto &entry : round.actions) {
        auto actor = entry.attacker.resolve();
        if (entry.retired || !actor) continue;
        if (blocksOwnerActions(*actor) || (!entry.action->isCancelled() && entry.action->holdsCombatRound())) held = true;
        auto target = std::dynamic_pointer_cast<Creature>(entry.target.resolve());
        if (target && isUnavailableAttackTarget(*target)) targetDown = true;
    }
    // Once nothing holds it, a round whose target is down ends early.
    const bool expired = round.state == CombatRound::SecondAction && (overdrawn || round.time >= round.duration);
    if (!held && (expired || targetDown)) {
        round.state = CombatRound::Finished;
        finishRound(round);
    }
    for (const auto &[actor, cutscene] : posing) readyAfterRoundPause(*actor, cutscene);
}

bool Combat::isActionPaused(const Action &action) const {
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (const auto &entry : round->actions) {
            if (!entry.retired && (entry.action.get() == &action || &entry.action->combatAction() == &action)) {
                auto actor = entry.attacker.resolve();
                return (actor && blocksOwnerActions(*actor)) || round->suspends(action);
            }
        }
    }
    return false;
}

bool Combat::isRoundMaster(const Creature &creature) const {
    for (const auto &round : _rounds) {
        if (round->state != CombatRound::Finished && round->master.resolve().get() == &creature) return true;
    }
    return false;
}
bool Combat::isEngagedBy(const Creature &attacker, const Creature &target) const {
    if (target.isDebilitated(true)) return false;
    const auto attempted = target.getAttemptedAttackTarget();
    const bool attempting = attempted != script::kObjectInvalid;
    const auto spell = target.attemptedSpellTarget();
    bool engaged = !attempting || attempted == attacker.id() || spell.get() == &attacker;
    if (_game.party().getLeader().get() == &target) {
        if (!attempting && !spell) return false;
        if ((attempting && attempted != attacker.id()) || (spell && spell.get() != &attacker)) return false;
        const auto ordered = target.getOrderedAttackTarget();
        if (ordered != script::kObjectInvalid && ordered != attacker.id()) return false;
    }
    return engaged;
}

bool Combat::engagePhysicalTarget(const Creature &attacker, Object &target, int pauseMilliseconds) {
    auto *partner = dyn_cast<Creature>(&target);
    if (!partner || !isEngagedBy(attacker, *partner)) return false;
    // An attacker that masters the exchange starts its partner's side of it,
    // which locks the partner's orientation on the attacker. It masters when
    // it is in the party, or when the partner is attempting to attack no one,
    // or to attack it without mastering a round of its own.
    const auto partnerAttempted = partner->getAttemptedAttackTarget();
    if (_game.party().isMember(attacker) || partnerAttempted == script::kObjectInvalid ||
        (partnerAttempted == attacker.id() && !isRoundMaster(*partner)))
        partner->setOrientationLock(attacker.id());
    // The attack's round is engaged with the partner, and the attacker masters
    // it unless someone already does.
    if (auto action = attacker.currentCombatAction()) {
        if (CombatRound *round = findRoundForAction(action, attacker)) {
            if (round->master.empty()) round->master = _game.getObjectById(attacker.id());
            round->engaged = _game.getObjectById(partner->id());
        }
    }
    // Two halves of a shared round already keep its partners apart, and a
    // partner busy in a round of its own keeps it.
    if (ownsRound(*partner)) return true;
    // A partner owning no round waits out the attacker's pause, whoever held it before.
    auto &pause = _ownerPauses[partner->id()];
    pause.actor = _game.getObjectById<Creature>(partner->id());
    pause.pausedBy = _game.getObjectById(attacker.id());
    pause.remaining = pauseMilliseconds / 1000.0f;
    pause.engaged = true;
    return true;
}
bool Combat::isRoundEngaged(const Action &action) const {
    for (const auto &round : _rounds) {
        if (round->state == CombatRound::Finished) continue;
        for (const auto &entry : round->actions) {
            if (!entry.retired && (entry.action.get() == &action || &entry.action->combatAction() == &action))
                return round->duel || round->engaged.resolve() != nullptr;
        }
    }
    return false;
}

void Combat::configureSpellPair(CombatRound &round, const std::shared_ptr<Action> &action, Creature &caster) {
    auto *cast = dyn_cast<CastSpellAtObjectAction>(&action->combatAction());
    // An instant cast's round engages no one.
    if (!cast || cast->instantSpell()) return;
    auto target = std::dynamic_pointer_cast<Creature>(getTarget(*action));
    const bool engaged = target && isEngagedBy(caster, *target);
    bool master = false;
    if (engaged) {
        const auto targetAttempted = target->getAttemptedAttackTarget();
        master = _game.party().isMember(caster) || targetAttempted == script::kObjectInvalid ||
            (targetAttempted == caster.id() && !isRoundMaster(*target));
    }
    round.master = engaged ? (master ? _game.getObjectById(caster.id()) : std::static_pointer_cast<Object>(target)) : nullptr;
    round.engaged = engaged ? target : nullptr;
    if (engaged && round.state == CombatRound::Pending && round.actions.size() == 2) {
        auto masterObject = round.master.resolve();
        for (auto &entry : round.actions) entry.slot = entry.attacker.resolve() == masterObject ? 0 : 1;
    }
    // When the second command arrives while still pending, its master owns slot zero.
}
void Combat::beginCast(const std::shared_ptr<Action> &action, Creature &caster, float seconds, bool itemUse, bool fake) {
    auto *owner = findRoundForAction(action, caster);
    if (!owner || owner->state == CombatRound::Finished) return;
    const bool joined = std::any_of(owner->actions.begin(), owner->actions.end(), [&](const auto &entry) {
        return !entry.retired && entry.action == action && entry.joined;
    });
    // In a round its creature was already in, an item use neither holds nor
    // shortens the round, and a cast sets it up only while nothing holds it.
    if (joined && (itemUse || owner->pauseRemaining > 0.0f)) return;
    std::shared_ptr<Creature> target;
    if (joined) {
        // Such a round keeps its engagement; while it is engaged, the cast holds
        // the creature it is aimed at.
        auto *cast = dyn_cast<CastSpellAtObjectAction>(&action->combatAction());
        if (cast && (owner->duel || owner->engaged.resolve()))
            target = std::dynamic_pointer_cast<Creature>(cast->target());
    } else {
        // Preserve the role selected when the pair was admitted. Selecting it again
        // would count this pair's own master as an unrelated engaged round.
        if (owner->master.empty()) configureSpellPair(*owner, action, caster);
        // A fake cast that starts its round holds no partner, nor does an item use.
        if (!itemUse && !fake) target = std::dynamic_pointer_cast<Creature>(owner->engaged.resolve());
    }
    if (target.get() == &caster) target.reset();
    const int pause = std::max(0, static_cast<int>(seconds * 1000.0f));
    bool partnerHeld = false;
    for (auto &candidate : _rounds) {
        auto &round = *candidate;
        if (round.state == CombatRound::Finished) continue;
        bool partner = target && std::any_of(round.actions.begin(), round.actions.end(), [&](const auto &entry) {
            return !entry.retired && entry.attacker.resolve() == target;
        });
        if (&round != owner && !partner) continue;
        partnerHeld = partnerHeld || partner;
        rescaleRound(round, pause, false);
        round.pauseRemaining += pause / 1000.0f;
        round.pauseOwner = _game.getObjectById(caster.id());
        if (partner && &round != owner) {
            round.master = owner->master;
            round.engaged = _game.getObjectById(caster.id());
        }
    }
    // A partner that owns no round is held all the same, by itself, and loses
    // no round time. The cast that begins a round engages it.
    if (target && !partnerHeld) {
        auto &hold = _ownerPauses[target->id()];
        hold.actor = target;
        hold.pausedBy = std::static_pointer_cast<Object>(target);
        hold.remaining += pause / 1000.0f;
        hold.engaged = hold.engaged || !joined;
    }
}

void Combat::beginInstantCast(const std::shared_ptr<Action> &action, Creature &caster, Object *target) {
    // The attack on a creature, door or container makes the round: a Power
    // Attack in front of the queue that needs no approach, engages no one and
    // leaves the round free once its pause is over.
    if (target && (isa<Creature>(target) || isa<Door>(target) || isa<Placeable>(target))) {
        auto attack = _game.newAction<UseFeatAction>(FeatType::PowerAttack, _game.getObjectById(target->id()));
        attack->markInstantCastAttack();
        caster.addActionOnTop(std::move(attack));
        return;
    }
    // With any other target the round runs its full length at once.
    addAction(action, caster);
}

std::optional<SavedRoundClock> Combat::saveRound(const Action &action) const {
    for (const auto &round : _rounds) for (const auto &entry : round->actions) {
        if (entry.action.get() != &action || entry.retired) continue;
        SavedRoundClock result;
        result.id = round->id; result.slot = entry.slot; result.state = round->state; result.joined = entry.joined;
        result.elapsed = round->time; result.duration = round->duration;
        result.pauseRemaining = round->pauseRemaining;
        auto owner = round->pauseOwner.resolve();
        result.pauseOwner = SavedObjectReference::fromRuntimeId(owner ? owner->id() : script::kObjectInvalid);
        _game.bindSavedObjectReference(result.pauseOwner);
        auto master = round->master.resolve(); auto engaged = round->engaged.resolve();
        result.master = SavedObjectReference::fromRuntimeId(master ? master->id() : script::kObjectInvalid);
        result.engaged = SavedObjectReference::fromRuntimeId(engaged ? engaged->id() : script::kObjectInvalid);
        _game.bindSavedObjectReference(result.master); _game.bindSavedObjectReference(result.engaged);
        return result;
    }
    return std::nullopt;
}

void Combat::restoreRound(const std::shared_ptr<Action> &action,
                         const std::shared_ptr<Creature> &actor, const SavedRoundClock &saved) {
    if (!saved.id || saved.slot < 0 || saved.slot > 1 || saved.state < 0 || saved.state > CombatRound::Finished ||
        !std::isfinite(saved.elapsed) || !std::isfinite(saved.duration) || saved.duration < 0.0f ||
        !std::isfinite(saved.pauseRemaining) || saved.pauseRemaining < 0.0f || !actor) return;
    if (findRoundForAction(action, *actor)) return;
    actor->setCurrentCombatAction(action);
    if (isHostileAction(action->combatAction())) actor->setCombatState(true);
    CombatRound *round = nullptr;
    for (auto &candidate : _rounds) if (candidate->id == saved.id) { round = candidate.get(); break; }
    if (!round) {
        _rounds.emplace_back(std::make_shared<CombatRound>(action, actor, getTarget(*action), true));
        round = _rounds.back().get(); round->id = saved.id;
        round->state = static_cast<CombatRound::State>(saved.state);
        round->time = saved.elapsed; round->duration = saved.duration;
        round->pauseOwner = saved.pauseOwner.boundObject();
        round->master = saved.master.boundObject(); round->engaged = saved.engaged.boundObject();
        round->pauseRemaining = round->pauseOwner.resolve() ? saved.pauseRemaining : 0.0f;
        _nextRoundId = std::max(_nextRoundId, saved.id + 1);
    } else {
        if (round->actions.size() == 2) return;
        for (const auto &entry : round->actions) if (entry.slot == saved.slot) return;
        round->actions.emplace_back(action, actor, getTarget(*action), true);
    }
    round->actions.back().slot = saved.slot;
    // An item use starts over after a load inside the round it had begun,
    // which keeps the pause it was given then rather than taking a new one.
    round->actions.back().joined = saved.joined || isItemUse(action->combatAction());
    round->duel = round->actions.size() == 2;
    if (round->duel && round->actions[0].slot > round->actions[1].slot)
        std::swap(round->actions[0], round->actions[1]);
    if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&action->combatAction());
        cast && cast->spell()->hostile)
        actor->setAttemptedSpellTarget(cast->target()->id());
}

void Combat::finishRound(CombatRound &round) {
    const std::vector<CombatRound::RoundAction> entries(round.actions.begin(), round.actions.end());
    SmallSet<Creature *, 2> owners;
    for (const auto &entry : entries) {
        auto actor = entry.attacker.resolve();
        if (!entry.retired && actor && owners.insert(actor.get()).second) actor->refreshBodyFuel();
    }
    for (auto &entry : round.actions) {
        if (!entry.retired) {
            // The round's end clears its attack records without ending the
            // action's queued lifetime. Undelivered events read the cleared records.
            entry.action->retireCombatRound();
        }
    }
    owners.clear();
    for (const auto &entry : entries) {
        auto actor = entry.attacker.resolve();
        if (!entry.retired && actor && owners.insert(actor.get()).second) {
            releasePartner(*actor);
            finishOwner(*actor, 1, entry.action->combatAction().suppressesEndRoundScript() ||
                                   isCutsceneRoundFor(round, entry));
        }
    }
    for (const auto &entry : entries) if (!entry.retired) continueOwner(entry, isCutsceneRoundFor(round, entry));
}

// The leader's next attack is an entry on its round, as the player's orders
// are, but nothing is cleared for it. Taking it on the target the player
// ordered forgets that order.
void Combat::scheduleContinuation(Creature &actor, const std::shared_ptr<Object> &target) {
    auto action = addRoundAttack(actor, target);
    if (actor.getOrderedAttackTarget() == target->id()) actor.setOrderedAttackTarget(script::kObjectInvalid);
    if (!action) return;
    if (auto *attack = dyn_cast<AttackObjectAction>(action.get())) attack->markRoundContinuation();
}

void Combat::continueOwner(const CombatRound::RoundAction &entry, bool cutsceneRound) {
    auto actor = entry.attacker.resolve();
    if (!actor || entry.action->isCancelled() ||
        isUnavailableAttackTarget(*actor) || cutsceneRound) return;
    // Only the controlled creature fights on into the next round.
    if (actor != _game.party().getLeader()) return;
    Action &command = entry.action->combatAction();
    auto *cast = dyn_cast<CastSpellAtObjectAction>(&command);
    const bool castRound = cast || isa<CastSpellAtLocationAction>(&command);
    const bool instant = isInstantCast(command);
    // A cast or item use makes a spell round; an instant cast does not.
    const bool spellRound = castRound && !instant;
    // A cast's round is on the creature it is aimed at, unless that is the
    // caster; a cast at a location has none.
    std::shared_ptr<Object> roundTarget = entry.target.resolve();
    if (castRound) roundTarget = cast && cast->target().get() != actor.get() ? cast->target() : nullptr;
    auto roundCreature = std::dynamic_pointer_cast<Creature>(roundTarget);
    const bool died = roundCreature && isUnavailableAttackTarget(*roundCreature);
    // A spell round is kept on its spell's target, the caster included; an item
    // use leaves nothing to keep once it has ended. The round's end forgets a
    // target that is down, and an opened door or container.
    std::shared_ptr<Object> kept = spellRound ? (cast && !cast->item() ? cast->target() : nullptr) : roundTarget;
    if (const auto creature = std::dynamic_pointer_cast<Creature>(kept); creature && isUnavailableAttackTarget(*creature))
        kept.reset();
    else if (const auto door = std::dynamic_pointer_cast<Door>(kept); door && !spellRound && door->state() != DoorState::Closed)
        kept.reset();
    else if (const auto placeable = std::dynamic_pointer_cast<Placeable>(kept); placeable && !spellRound && placeable->isOpen())
        kept.reset();
    // Round commands still waiting keep the round from staying on its target:
    // the round's pending entries, and the casts, attacks and stances that wait
    // in the queue without one.
    bool pending = hasScheduled(*actor);
    for (const auto &node : actor->actions().nodes) {
        if (!node->action || node->action == entry.action || node->action->isCompleted() || node->action->isCancelled())
            continue;
        Action &queued = node->action->combatAction();
        const auto *stance = dyn_cast<CombatStanceAction>(&queued);
        if (isa<AttackObjectAction>(&queued) || isa<UseFeatAction>(&queued) ||
            (stance && stance->stance() == CombatStance::TotalDefense) ||
            isa<CastSpellAtObjectAction>(&queued) || isa<CastSpellAtLocationAction>(&queued))
            pending = true;
    }
    // A target that died hands the round on to a new one in any case.
    bool retarget = died;
    bool stays = false;
    if (!kept) {
        retarget = retarget || !pending;
    } else if (!pending) {
        const auto keptCreature = std::dynamic_pointer_cast<Creature>(kept);
        if (!keptCreature) {
            stays = true;
        } else if (isHostileForContinuation(*actor, *keptCreature)) {
            // A spell round stays on an enemy only within cleave range.
            stays = !spellRound ||
                glm::distance(actor->position(), keptCreature->position()) <= actor->maxCleaveRange(keptCreature.get());
        } else if (_game.isTSL()) {
            // TSL stays on a creature that is not an enemy, and after a spell
            // on the caster itself looks for one.
            stays = true;
            retarget = retarget || keptCreature == actor;
        }
    }
    std::shared_ptr<Object> target = kept;
    if (retarget) {
        std::shared_ptr<Creature> enemy;
        auto module = _game.module();
        auto area = module ? module->area() : nullptr;
        if (area) {
            const float searchRange = actor->maxCleaveRange(actor.get());
            // After a kill the search starts around the fallen target.
            if (died) enemy = findNearestEnemy(*area, *actor, *roundCreature, searchRange);
            if (!enemy) enemy = findNearestEnemy(*area, *actor, *actor, searchRange);
        }
        // With no enemy left the creature drops its combat modes; its combat
        // state runs out on its own.
        if (!enemy) {
            actor->cancelAllCombatModes();
            return;
        }
        takeNewAttackTarget(*actor, enemy);
        _game.setLastTarget(enemy->id(), true);
        target = enemy;
    } else if (!stays) {
        return;
    }
    // The next round's entry waits behind whatever is already queued.
    ensureDispatcher(*actor, false);
    // After a Total Defense round the stance is taken again.
    if (actor->roundActionKind() == 13) scheduleStance(*actor, target);
    else scheduleContinuation(*actor, target);
}

} // namespace game

} // namespace reone
