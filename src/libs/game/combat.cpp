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
#include <cmath>

#include "reone/game/action/attackobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/castspellatlocation.h"
#include "reone/game/d20/spell.h"
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

static constexpr float kRoundDuration = 3.0f;
static constexpr float kDeactivateDelay = 8.0f;
static constexpr float kMaxAttackRange = 20.0f;
static constexpr float kAttackTargetSearchPadding = 2.0f;

static bool isUnavailableAttackTarget(const Creature &creature) {
    return creature.isDead() || creature.isTemporarilyDead();
}

bool CombatRound::suspends(const Action &action) const {
    auto owner = pauseOwner.resolve();
    if (pauseRemaining <= 0.0f || !owner) return false;
    for (const auto &entry : actions)
        if (entry.action.get() == &action || &entry.action->combatAction() == &action)
            return entry.attacker.resolve() != owner;
    return true;
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
                   state == requiredState[actions[i].slot];
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

static float getAttackTargetSearchRange(const Creature &attacker) {
    return std::min(attacker.getAttackRange(), kMaxAttackRange) + kAttackTargetSearchPadding;
}

static bool isHostileForContinuation(const Creature &source, const Creature &target) {
    return source.getReputationToward(target) <= 10;
}

static std::shared_ptr<Creature> findNearestEnemy(
    Area &area,
    const Creature &attacker,
    const Creature &observer,
    float maxDistance) {

    std::shared_ptr<Creature> nearest;
    float nearestDistance = maxDistance;

    for (const std::shared_ptr<Object> &object : area.getObjectsByType(ObjectType::Creature)) {
        auto candidate = std::static_pointer_cast<Creature>(object);
        if (candidate->id() == attacker.id() ||
            candidate->id() == observer.id() ||
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
    if (!attacker || attacker.get() != &actor) {
        throw std::logic_error(
            "Combat action actor is not the live registered Creature");
    }
    bool actorQueueAssociated = std::find(
                                    attacker->actions().begin(),
                                    attacker->actions().end(),
                                    action) != attacker->actions().end();

    // If attacker has already started a combat round, return it.
    if (CombatRound *round = findRoundForAction(action, *attacker)) {
        return *round;
    }

    // Find an existing round where target and attacker roles are reversed, and
    // append the action to this round.
    std::shared_ptr<Object> target = getTarget(*action);
    if (target) {
        if (CombatRound *round = tryAppendAction(
                action, attacker, target, actorQueueAssociated)) {
            recordCombatAction(actor, target, *action);
            configureSpellPair(*round, action, *attacker);
            debug(str(boost::format("Append attack: %s -> %s") % actor.tag() % target->tag()), LogChannel::Combat);
            return *round;
        }
    }

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
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        creature->activateCombat();
    }
    if (target && isa<Creature>(target)) {
        cast<Creature>(target)->activateCombat(2);
    }

    return newRound;
}

void CombatDispatchAction::execute(std::shared_ptr<Action>, Object &actor, float) {
    if (!_game.combat().hasScheduled(actor)) complete();
}

bool CombatDispatchAction::cancel(std::shared_ptr<Action>, Object &actor) {
    complete();
    if (auto *creature = dyn_cast<Creature>(&actor)) _game.combat().transferEquipment(*creature);
    _game.combat().discardEquipment(actor);
    return true;
}

std::optional<SavedActionRecord> CombatDispatchAction::saveFacingState() const {
    SavedActionRecord result = originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 63;
    result.declaredParameterCount = 1;
    result.parameters = {{1, int32_t {_mode}}};
    return result;
}

bool Combat::hasScheduled(const Object &actor) const {
    auto found = _scheduled.find(actor.id());
    return found != _scheduled.end() && found->second->actor.resolve().get() == &actor &&
        !found->second->actions.empty();
}

void Combat::ensureDispatcher(Creature &actor) {
    for (const auto &node : actor.actions().nodes)
        if (node->actionId == 63 && node->action && !node->action->isCompleted() && !node->action->isCancelled()) return;
    actor.addActionOnTop(_game.newAction<CombatDispatchAction>());
}

static bool scheduledCommandMatches(const SavedScheduledAction &saved) {
    if (!saved.command) return saved.type == 3;
    const auto id = saved.command->actionId;
    switch (saved.type) {
    case 1: case 11: return id == 12;
    case 6: return id == 8;
    case 7: return id == 11;
    case 9: case 10: return id == 15;
    case 12: return id == 1 || id == 17;
    default: return false;
    }
}


bool Combat::scheduleEquipment(Object &object, const OrdinaryActionQueue::Node &node) {
    auto actor = std::dynamic_pointer_cast<Creature>(_game.getObjectById(object.id()));
    if (!actor || !node || !node->action || node->action->originalSavedAction()) return false;
    const auto id = node->action->serializedActionId();
    if (id != 8 && id != 11) return false;
    auto command = node->action->saveFacingState();
    if (!command || command->parameters.size() != 3) return false;
    auto target = std::get_if<SavedObjectReference>(&command->parameters[0].payload);
    auto flags = std::get_if<int32_t>(&command->parameters[2].payload);
    if (!target || !flags) return false;
    const uint32_t slot = id == 8 ? static_cast<uint32_t>(std::get<int32_t>(command->parameters[1].payload)) : 0;
    if (!actor->isCommandable() || (id == 8 && actor->isInCombat() &&
        actor->combatActivationType() == 1 && slot == equipmentSlotMask(InventorySlots::body))) {
        node->action->markCancelled();
        return true;
    }
    command->bindObjectReferences(_game);
    auto &state = _scheduled[actor->id()];
    if (!state || state->actor.resolve() != actor) {
        state = std::make_shared<ScheduledOwner>();
        state->actor = actor;
        for (const auto &round : _rounds) {
            if (round->state == CombatRound::Finished) continue;
            for (const auto &entry : round->actions)
                if (!entry.retired && entry.attacker.resolve() == actor) state->time = round->time;
        }
    }
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
    }
    if (*flags != 0 || !actor->isInCombat() || actor->combatActivationType() != 1) return false;
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

bool Combat::blocksOwnerActions(const Creature &actor) const {
    if (!actor.actions().nodes.empty()) {
        const auto &head = actor.actions().nodes.front()->action;
        if (head && head->isScheduledCommand() && !head->isCompleted() && !head->isCancelled()) return false;
    }
    const auto found = _scheduled.find(actor.id());
    if (found == _scheduled.end() || found->second->actor.resolve().get() != &actor || found->second->actions.empty()) return false;
    const auto &head = found->second->actions.front();
    return head->saved.applied || head->saved.timer <= static_cast<int>(found->second->time * 1000.0f);
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
        if (!actor->actions().nodes.empty()) {
            const auto &head = actor->actions().nodes.front()->action;
            if (head && head->isScheduledCommand() && !head->isCompleted() && !head->isCancelled()) continue;
        }
        float externalPause = 0.0f;
        for (const auto &round : _rounds) {
            if (round->state == CombatRound::Finished || round->pauseRemaining <= 0.0f) continue;
            if (std::any_of(round->actions.begin(), round->actions.end(), [&](const auto &entry) {
                    return !entry.retired && entry.attacker.resolve() == actor;
                })) externalPause = std::max(externalPause, round->pauseRemaining);
        }
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
                if (entry->saved.type == 3) {
                    entry->saved.applied = true;
                    entry->saved.remainingPause = std::max(0, entry->saved.animationTime) / 1000.0f;
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
                    if (!action->runtimeDependenciesLive() || !actor->permitsAction(*action)) {
                        if (!entry->refusalReported) {
                            warn("Scheduled action is not currently executable", LogChannel::Combat);
                            entry->refusalReported = true;
                        }
                        _equipmentTime[actor->id()] += frame;
                        frame = 0.0f;
                        break;
                    }
                    if (!entry->saved.isEquipment()) {
                        owner->actions.pop_front();
                        action->setScheduledCommand(true);
                        actor->requeueActionNode(entry->node);
                        frame = 0.0f;
                        break;
                    }
                    entry->saved.applied = true;
                    entry->saved.remainingPause = std::max(0, entry->saved.animationTime) / 1000.0f;
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
            action->cancel(action, actor);
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
                saved.command = std::move(command);
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
    actor->activateCombat();
}

void Combat::cancelActions(Object &actor) {
    discardEquipment(actor);
    if (auto *creature = dyn_cast<Creature>(&actor)) releasePartner(*creature);
    std::vector<std::shared_ptr<Action>> actions;
    for (const auto &round : _rounds) {
        for (const auto &entry : round->actions) {
            if (entry.attacker.resolve().get() == &actor &&
                std::find(actions.begin(), actions.end(), entry.action) == actions.end())
                actions.push_back(entry.action);
        }
    }
    for (const auto &action : actions) {
        if (action->isCompleted() || action->isCancelled()) continue;
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
    actor.setMovementRestricted(false);
    actor.setMovementType(Creature::MovementType::None);
    const auto leader = _game.party().getLeader();
    if (leader.get() != &actor) actor.setClientCombatMode(false);
    if (runEndRound && !suppressScript && actor.currentSerializedActionId() != 1 &&
        leader.get() != &actor && !actor.isDead() && (!actor.isPC() || actor.currentHitPoints() > 0))
        actor.runEndRoundScript();
}

void Combat::endRound(Creature &actor, int runEndRound) {
    std::vector<CombatRound::RoundAction> finished;
    bool suppressed = false;
    actor.refreshBodyFuel();
    for (const auto &round : _rounds) {
        for (auto &entry : round->actions) {
            if (entry.attacker.resolve().get() != &actor || entry.retired) continue;
            finished.push_back(entry);
            suppressed |= entry.action->combatAction().suppressesEndRoundScript();
            entry.retired = true;
            entry.action->retireCombatRound();
            entry.target.reset();
        }
    }
    releasePartner(actor);
    transferEquipment(actor);
    finishOwner(actor, runEndRound, suppressed);
    if (runEndRound) for (const auto &entry : finished) continueOwner(entry);
}

void Combat::update(float dt) {
    pruneInvalidRounds();
    updateEquipment(dt);
    const RoundQueue rounds = _rounds;
    for (const auto &round : rounds) {
        if (std::find(_rounds.begin(), _rounds.end(), round) == _rounds.end()) continue;
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

void Combat::cancelRound(CombatRound &round) {
    const std::vector<CombatRound::RoundAction> actions(round.actions.begin(), round.actions.end());
    for (auto &entry : round.actions) entry.retired = true;
    for (const auto &entry : actions) {
        if (entry.retired) continue;
        auto actor = entry.attacker.resolve();
        entry.action->retireCombatRound();
        if (actor) {
            releasePartner(*actor);
            if (!entry.action->isCompleted() && !entry.action->isCancelled())
                entry.action->cancel(entry.action, *actor);
        }
        entry.action->markCancelled();
        entry.action->complete();
    }
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
            if (auto actor = entry.attacker.resolve()) {
                releasePartner(*actor);
                if (!entry.action->isCancelled()) entry.action->cancel(entry.action, *actor);
            }
            entry.action->markCancelled();
            entry.action->complete();
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
    float advance = std::max(0.0f, dt);
    if (round.pauseRemaining > 0.0f) {
        const float used = std::min(advance, round.pauseRemaining);
        round.pauseRemaining -= used;
        advance -= used;
        if (round.pauseRemaining > 0.0f) return;
        round.pauseOwner.reset();
    }
    round.time += advance;
    if (round.state == CombatRound::Pending) round.state = CombatRound::FirstAction;
    if (round.state == CombatRound::FirstAction && round.time >= 0.5f * round.duration)
        round.state = CombatRound::SecondAction;
    if (round.state != CombatRound::SecondAction || round.time < round.duration) return;
    for (const auto &entry : round.actions) {
        auto actor = entry.attacker.resolve();
        if (!entry.retired && actor && (blocksOwnerActions(*actor) ||
            (!entry.action->isCancelled() && entry.action->holdsCombatRound()))) return;
    }
    round.state = CombatRound::Finished;
    finishRound(round);
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
bool Combat::isSpellEngaged(const Creature &caster, const Creature &target) const {
    if (target.isDebilitated()) return false;
    const auto attack = target.getAttackTarget();
    const auto spell = target.attemptedSpellTarget();
    bool engaged = !attack || attack.get() == &caster || spell.get() == &caster;
    if (_game.party().getLeader().get() == &target) {
        if (!attack && !spell) return false;
        if ((attack && attack.get() != &caster) || (spell && spell.get() != &caster)) return false;
        const auto attempted = target.getAttemptedAttackTarget();
        if (attempted != script::kObjectInvalid && attempted != caster.id()) return false;
    }
    return engaged;
}
void Combat::configureSpellPair(CombatRound &round, const std::shared_ptr<Action> &action, Creature &caster) {
    if (!isa<CastSpellAtObjectAction>(&action->combatAction())) return;
    auto target = std::dynamic_pointer_cast<Creature>(getTarget(*action));
    const bool engaged = target && isSpellEngaged(caster, *target);
    bool master = false;
    if (engaged) {
        const auto targetAttack = target->getAttackTarget();
        master = caster.isPC() || !targetAttack || (targetAttack.get() == &caster && !isRoundMaster(*target));
    }
    round.master = engaged ? (master ? _game.getObjectById(caster.id()) : std::static_pointer_cast<Object>(target)) : nullptr;
    round.engaged = engaged ? target : nullptr;
    if (engaged && round.state == CombatRound::Pending && round.actions.size() == 2) {
        auto masterObject = round.master.resolve();
        for (auto &entry : round.actions) entry.slot = entry.attacker.resolve() == masterObject ? 0 : 1;
    }
    // When the second command arrives while still pending, its master owns slot zero.
}
void Combat::beginCast(const std::shared_ptr<Action> &action, Creature &caster, float seconds) {
    auto *owner = findRoundForAction(action, caster);
    if (!owner || owner->state == CombatRound::Finished) return;
    // Preserve the role selected when the pair was admitted. Selecting it again
    // would count this pair's own master as an unrelated engaged round.
    if (owner->master.empty()) configureSpellPair(*owner, action, caster);
    auto target = std::dynamic_pointer_cast<Creature>(owner->engaged.resolve());
    const bool engaged = static_cast<bool>(target);
    const int pause = std::max(0, static_cast<int>(seconds * 1000.0f));
    for (auto &candidate : _rounds) {
        auto &round = *candidate;
        if (round.state == CombatRound::Finished) continue;
        bool partner = engaged && std::any_of(round.actions.begin(), round.actions.end(), [&](const auto &entry) {
            return !entry.retired && entry.attacker.resolve() == target;
        });
        if (&round != owner && !partner) continue;
        rescaleRound(round, pause, false);
        round.pauseRemaining += pause / 1000.0f;
        round.pauseOwner = _game.getObjectById(caster.id());
        if (partner && &round != owner) {
            round.master = owner->master;
            round.engaged = _game.getObjectById(caster.id());
        }
    }
}

void Combat::finishCast(Creature &caster) {
    for (auto &round : _rounds) {
        if (round->pauseOwner.resolve().get() == &caster) {
            round->pauseRemaining = 0.0f;
            round->pauseOwner = RuntimeObjectRef<Object> {};
        }
    }
}

std::optional<SavedRoundClock> Combat::saveRound(const Action &action) const {
    for (const auto &round : _rounds) for (const auto &entry : round->actions) {
        if (entry.action.get() != &action || entry.retired) continue;
        SavedRoundClock result;
        result.id = round->id; result.slot = entry.slot; result.state = round->state;
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
    actor->activateCombat();
    round->actions.back().slot = saved.slot;
    round->duel = round->actions.size() == 2;
    if (round->duel && round->actions[0].slot > round->actions[1].slot)
        std::swap(round->actions[0], round->actions[1]);
    if (auto *cast = dyn_cast<CastSpellAtObjectAction>(&action->combatAction()))
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
            // Keep the entry associated with the finished round until the
            // queued action observes that state and completes its cleanup.
            entry.action->retireCombatRound();
        }
    }
    owners.clear();
    for (const auto &entry : entries) {
        auto actor = entry.attacker.resolve();
        if (!entry.retired && actor && owners.insert(actor.get()).second) {
            releasePartner(*actor);
            finishOwner(*actor, 1, entry.action->combatAction().suppressesEndRoundScript());
            if (actor->isRuntimeLive()) actor->deactivateCombat(kDeactivateDelay);
        }
        if (auto target = std::dynamic_pointer_cast<Creature>(entry.target.resolve()))
            target->deactivateCombat(kDeactivateDelay);
    }
    for (const auto &entry : entries) if (!entry.retired) continueOwner(entry);
}

void Combat::continueOwner(const CombatRound::RoundAction &entry) {
    auto actor = entry.attacker.resolve();
    auto leader = _game.party().getLeader();
    if (!actor || entry.action->isCancelled() ||
        isUnavailableAttackTarget(*actor) || actor->stateControlsActions() || entry.action->combatAction().isCutsceneAttack()) return;
    auto *castAction = dyn_cast<CastSpellAtObjectAction>(&entry.action->combatAction());
    const bool spell = castAction && castAction->spell()->hostile;
    if (!spell && actor != leader) return;
    if (!spell && !isa<AttackObjectAction>(&entry.action->combatAction()) &&
        !isa<UseFeatAction>(&entry.action->combatAction())) return;
    bool otherCommand = false;
    for (const auto &node : actor->actions().nodes)
        if (!node->action || (node->action != entry.action && !node->action->isCompleted() && !node->action->isCancelled()))
            otherCommand = true;
    if (otherCommand) return;
    auto module = _game.module();
    std::shared_ptr<Object> target = spell ? actor->attemptedSpellTarget() : entry.target.resolve();
    auto targetCreature = std::dynamic_pointer_cast<Creature>(target);
    const bool keepObject = spell && target && !targetCreature;
    const bool keepCreature = targetCreature && !isUnavailableAttackTarget(*targetCreature) &&
        isHostileForContinuation(*actor, *targetCreature) &&
        (!spell || glm::distance(actor->position(), targetCreature->position()) <= actor->maxCleaveRange(targetCreature.get()));
    if (keepObject || keepCreature) {
        actor->addAction(_game.newAction<AttackObjectAction>(target));
        return;
    }
    std::shared_ptr<Creature> enemy;
    auto area = module ? module->area() : nullptr;
    if (area) {
        const float searchRange = spell ? actor->maxCleaveRange(actor.get()) : getAttackTargetSearchRange(*actor);
        if (targetCreature) {
            enemy = findNearestEnemy(*area, *actor, *targetCreature, searchRange);
        }
        if (!enemy) {
            enemy = findNearestEnemy(*area, *actor, *actor, searchRange);
        }
    }
    if (enemy) {
        if (actor == leader) _game.setLastTarget(enemy->id());
        actor->addAction(_game.newAction<AttackObjectAction>(enemy));
    } else actor->deactivateCombat(0.0f);
}

} // namespace game

} // namespace reone
