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

#include "reone/game/object.h"
#include "reone/game/actionremoval.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/effect/scriptrules.h"
#include "reone/game/effectexpiry.h"
#include "effectcollection.h"
#include "reone/game/effect/damage.h"

#include <exception>
#include <sstream>
#include <typeinfo>

#include "reone/game/action/startconversation.h"
#include "reone/game/di/services.h"
#include "reone/game/equipmentrules.h"
#include "reone/game/game.h"
#include "reone/game/object/item.h"
#include "reone/game/object/area.h"
#include "reone/game/script/savedsituation.h"
#include "reone/resource/provider/scripts.h"
#include "reone/game/room.h"
#include "reone/resource/gff.h"
#include "reone/system/exception/validation.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace reone {

namespace game {

static constexpr float kKeepPathDuration = 1000.0f;
static constexpr float kDefaultMaxObjectDistance = 2.0f;
static constexpr float kMaxConversationDistance = 4.0f;
static constexpr float kDistanceWalk = 4.0f;

void Object::deserialize(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    if (gff.readString(_tag, "Tag")) {
        boost::to_lower(_tag);
    }

    // FIXME: not all of these properties are shared by all object subclasses.
    gff.readResRef(_blueprintResRef, "TemplateResRef");
    gff.readResRef(_conversation, "Conversation");
    gff.readResRef(_onHeartbeat, "ScriptHeartbeat");
    gff.readResRef(_onUserDefined, "ScriptUserDefine");
    gff.readBool(_minOneHP, "Min1HP");
    gff.readBool(_plot, "Plot");
    gff.readBool(_commandable, "Commandable");
    gff.readBool(_interruptable, "Interruptable");
    gff.readShort(_hitPoints, "HitPoints");
    gff.readShort(_maxHitPoints, "MaxHitPoints");
    int16_t currentHitPoints = static_cast<int16_t>(_currentHitPoints);
    if (gff.readShort(currentHitPoints, "CurrentHitPoints")) _currentHitPoints = currentHitPoints;

    gff.readFloat(_position[0], "X");
    gff.readFloat(_position[0], "XPosition");
    gff.readFloat(_position[1], "Y");
    gff.readFloat(_position[1], "YPosition");
    gff.readFloat(_position[2], "Z");
    gff.readFloat(_position[2], "ZPosition");

    {
        float cosine, sine;
        if (gff.readFloat(cosine, "XOrientation") && gff.readFloat(sine, "YOrientation")) {
            _orientation = glm::quat(glm::vec3(0.0f, 0.0f, -glm::atan(cosine, sine)));
        }

        float bearing;
        if (gff.readFloat(bearing, "Bearing")) {
            _orientation = glm::quat(glm::vec3(0.0f, 0.0f, bearing));
        }
    }

    if (_type != ObjectType::Placeable && _type != ObjectType::Store &&
        _type != ObjectType::Item && dynamic_cast<Creature *>(this) == nullptr) {
        deserializeOwnedItems(
            gff, identityContext, SaveRecordOriginKind::ContainedItem);
    }
    deserializeRuntimeState(gff, identityContext);
}

std::vector<std::shared_ptr<Object>> Object::ownedRuntimeObjects() const {
    return {_items.begin(), _items.end()};
}

std::shared_ptr<Item> Object::appendOwnedItemCandidate(
    std::vector<std::shared_ptr<Item>> &items,
    const std::shared_ptr<Item> &item,
    bool preserveSerializedIdentities) {
    if (!item) {
        throw ValidationException("Cannot stage a null owned Item");
    }
    for (const auto &existing : items) {
        if (!existing || existing.get() == item.get()) {
            continue;
        }
        if (preserveSerializedIdentities &&
            (existing->serializedObjectIdentity() ||
             item->serializedObjectIdentity())) {
            continue;
        }
        if (!existing->isStackCompatibleWith(*item)) {
            continue;
        }
        if (existing->mergeStackFrom(*item)) {
            item->setOwner(0);
            _game.discardStagedRuntimeObjects({item});
            return existing;
        }
    }
    items.push_back(item);
    return item;
}

void Object::deserializeOwnedItems(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext,
    SaveRecordOriginKind originKind,
    bool forceDropable,
    std::string originOwner) {
    if (!identityContext.isSerializedState()) {
        if (!gff.has("ItemList")) return;
        std::vector<std::shared_ptr<Object>> obsolete(
            _items.begin(), _items.end());
        std::vector<std::shared_ptr<Item>> replacement;
        ItemAttributes replacementAttributes;
        auto creature = dynamic_cast<Creature *>(this);
        _game.replaceRuntimeObjectGraph(
            obsolete,
            [&]() {
                for (const auto &itemGff : gff.getList("ItemList")) {
                    auto item = _game.newOwnedItem(*itemGff, identityContext);
                    if (forceDropable) item->setDropable(true);
                    item->setOwner(_id);
                    appendOwnedItemCandidate(
                        replacement, item, true);
                }
                if (creature) {
                    for (const auto &item : replacement) {
                        replacementAttributes.addItem(item, _services.game);
                    }
                }
            },
            [&]() noexcept {
                _items = std::move(replacement);
                if (creature) {
                    creature->itemAttributes() =
                        std::move(replacementAttributes);
                }
            });
        return;
    }

    std::vector<std::shared_ptr<Object>> obsolete(_items.begin(), _items.end());
    std::vector<std::shared_ptr<Item>> replacement;
    ItemAttributes replacementAttributes;
    auto creature = dynamic_cast<Creature *>(this);
    _game.replaceRuntimeObjectGraph(
        obsolete,
        [&]() {
            for (const auto &itemGff : gff.getList("ItemList")) {
                auto item = _game.newOwnedItem(*itemGff, identityContext);
                item->captureSaveRecord(
                    *itemGff,
                    identityContext,
                    {originKind,
                     originOwner.empty() ? std::to_string(_id) : originOwner});
                if (forceDropable) {
                    item->setDropable(true);
                }
                item->setOwner(_id);
                appendOwnedItemCandidate(
                    replacement, item, true);
            }
            if (creature) {
                for (const auto &item : replacement) {
                    replacementAttributes.addItem(item, _services.game);
                }
            }
        },
        [&]() noexcept {
            _items = std::move(replacement);
            if (creature) {
                creature->itemAttributes() =
                    std::move(replacementAttributes);
            }
        });
}

void Object::update(float dt) {
    if (_feedbackTextRemaining > 0.0f) {
        _feedbackTextRemaining = std::max(0.0f, _feedbackTextRemaining - dt);
        if (_feedbackTextRemaining == 0.0f) _feedbackText.clear();
    }
    if (!isRuntimeLive()) {
        return;
    }
    updateActions(dt);
    updateEffects(dt);
    if (!_dead && canExecuteActions()) {
        executeActions(dt);
    }
    if (_sceneNode && _sceneNode->type() == SceneNodeType::Model) {
        std::static_pointer_cast<ModelSceneNode>(_sceneNode)->setPickable(isSelectable());
    }
}

bool Object::getLocalBoolean(int index) const {
    auto it = _localBooleans.find(index);
    return it != _localBooleans.end() ? it->second : false;
}

int Object::getLocalNumber(int index) const {
    auto it = _localNumbers.find(index);
    return it != _localNumbers.end() ? it->second : 0;
}

void Object::setLocalBoolean(int index, bool value) {
    _localBooleans[index] = value;
}

void Object::setLocalNumber(int index, int value) {
    _localNumbers[index] = value;
}
void Object::deserializeRuntimeState(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {

    _localBooleans.clear();
    _localNumbers.clear();
    if (auto variables = gff.findStruct("SWVarTable")) {
        const auto bits = variables->getList("BitArray");
        for (size_t word = 0; word < std::min<size_t>(bits.size(), 5); ++word) {
            uint32_t value = bits[word]->getUint("Variable");
            for (int bit = 0; bit < 32; ++bit) {
                if ((value & (1u << bit)) != 0) {
                    _localBooleans[static_cast<int>(word * 32 + bit)] = true;
                }
            }
        }

        const auto bytes = variables->getList("ByteArray");
        for (size_t index = 0; index < std::min<size_t>(bytes.size(), 32); ++index) {
            uint8_t value = 0;
            if (bytes[index]->readByte(value, "Variable") && value != 0) {
                _localNumbers[static_cast<int>(index)] = value;
            }
        }
    }

    _savedEffects.clear();
    _savedActionQueue = SavedActionQueue {};
    _savedScheduledActions.clear();
    _savedScheduledReferencesBound.clear();
    _savedScheduledTime = 0.0f;
    _savedRuntimeIdentityContext = identityContext;
    _savedEffectReferencesBound.clear();
    _savedActionReferencesBound.clear();
    _savedRuntimeParsed = gff.has("EffectList") || gff.has("ActionList") || gff.has("CombatRoundData");
    if (auto round = gff.findStruct("CombatRoundData")) {
        _savedScheduledTime = round->getInt("Timer") / 1000.0f;
        for (const auto &entry : round->getList("SchedActionList"))
            _savedScheduledActions.push_back(SavedScheduledAction::fromGff(*entry, identityContext));
    }
    _savedRuntimePublished = false;
    if (_savedRuntimeParsed) {
        for (const auto &effect : gff.getList("EffectList")) {
            _savedEffects.push_back(EffectInstance::fromGff(
                *effect, identityContext));
        }
        _savedActionQueue = SavedActionQueue::fromGff(
            gff, identityContext);
        _effects.clear();
        if (auto *creature = dyn_cast<Creature>(this)) creature->clearArmorClassEffectCache();
        _actions.clear();
        _delayed.clear();
        _executingActionNode.reset();
    }

    _savedReferenceIds.clear();
    _savedReferences.clear();
    _lastDamager.reset();
    _lastDamageAmounts.fill(-1);
    _savedLastDamagerId.reset();
    uint32_t lastDamagerId = 0;
    if (gff.readDword(lastDamagerId, "LastDamager")) {
        _savedLastDamagerId = lastDamagerId;
    }
    static const std::array<std::string_view, 8> referenceFields {
        "AreaId",
        "CreatorId",
        "LastAttacker",
        "LastHostileActor",
        "LastPerceived",
        "MasterID",
        "OwnerId",
        "TargetId",
    };
    for (auto field : referenceFields) {
        uint32_t id = 0;
        if (gff.readDword(id, field)) {
            _savedReferenceIds.emplace(std::string(field), id);
        }
    }

    size_t perceptionIndex = 0;
    for (const auto &perception : gff.getList("PerceptionList")) {
        uint32_t id = 0;
        if (perception->readDword(id, "ObjectId")) {
            _savedReferenceIds.emplace(
                "Perception/" + std::to_string(perceptionIndex), id);
        }
        ++perceptionIndex;
    }
}

void Object::bindSavedRuntimeState() {
    if (!_savedRuntimeParsed) {
        return;
    }
    for (auto &effect : _savedEffects) {
        if (effect.hasStableId()) _game.importEffectId(effect.id);
        _savedEffectReferencesBound.push_back(
            _game.bindEffectCreator(effect));
    }
    for (auto &action : _savedActionQueue.actions) {
        _savedActionReferencesBound.push_back(
            action.bindObjectReferences(_game));
    }
    for (auto &action : _savedScheduledActions)
        _savedScheduledReferencesBound.push_back(action.bindObjectReferences(_game));
}

void Object::publishSavedRuntimeState() {
    if (!_savedRuntimeParsed || _savedRuntimePublished) {
        return;
    }
    SavedScriptSituationImporter importer(
        _game, _services.resource.scripts);

    for (size_t index = 0; index < _savedEffects.size(); ++index) {
        const auto &savedEffect = _savedEffects[index];
        if (index >= _savedEffectReferencesBound.size() ||
            !_savedEffectReferencesBound[index]) {
            continue;
        }
        EffectInstance effect(savedEffect);
        if (effect.durationType() == DurationType::Temporary) {
            auto remaining = _game.remainingEffectDuration(effect);
            if (!remaining || *remaining < 0.0f) {
                continue;
            }
            effect.remainingDuration = *remaining;
        }
        if (effect.hasStableId()) {
            _game.importEffectId(effect.id);
        } else {
            effect.id = _game.allocateEffectId();
        }
        restoreEffect(std::move(effect));
    }

    for (size_t index = 0; index < _savedActionQueue.actions.size(); ++index) {
        const auto &savedAction = _savedActionQueue.actions[index];
        if (index >= _savedActionReferencesBound.size() ||
            !_savedActionReferencesBound[index]) {
            addOpaqueAction(savedAction);
            continue;
        }
        auto action = savedAction.toRuntimeAction(_game, &importer);
        if (action) {
            action->attachSavedAction(savedAction);
            addAction(action);
        } else {
            addOpaqueAction(savedAction);
        }
    }
    onEffectsRestored();
    _savedRuntimePublished = true;
}

void Object::captureSaveRecord(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext,
    SaveRecordOrigin origin) {
    std::optional<SerializedObjectIdentity> identity;
    uint32_t id = 0;
    if (identityContext.isSerializedState() &&
        gff.readDword(id, "ObjectId")) {
        identity = SerializedObjectIdentity {identityContext, id};
    }
    _saveRecordProvenance = SaveRecordProvenance {
        SaveGffShadow::capture(gff), std::move(origin), std::move(identity)};
}

void Object::assignSerializedObjectIdentity(
    const SerializedObjectIdentity &identity) {
    if (_saveRecordProvenance) {
        _saveRecordProvenance->identity = identity;
    } else {
        _saveRecordProvenance = SaveRecordProvenance {
            SaveGffShadow {}, SaveRecordOrigin {}, identity};
    }
}

std::vector<EffectInstance> Object::saveEffectSnapshot() const {
    // Later orchestration calls this at a stable synchronous frame boundary.
    std::vector<EffectInstance> result;
    for (const EffectInstance &effect : _effects) {
        // Equipped effects are derived from the authoritative equipment edge.
        // Persisting them independently would duplicate them on reconstruction.
        if (effect.durationType() != DurationType::Equipped) {
            result.push_back(effect);
        }
    }
    return result;
}

EffectInstance *Object::findEffectInstance(const Effect &effect) {
    auto it = std::find_if(
        _effects.begin(),
        _effects.end(),
        [&effect](const EffectInstance &instance) {
            return instance.effect.get() == &effect;
        });
    return it != _effects.end() ? &*it : nullptr;
}

std::vector<SavedActionRecord> Object::saveActionSnapshot() const {
    // The same ordered nodes drive queries, execution and persistence.
    std::vector<SavedActionRecord> result;
    auto unsupportedAction = [this](const Action &action, size_t queueIndex) {
        std::ostringstream message;
        message << "live queued action has no save-facing representation"
                << ": ownerId=" << id()
                << " ownerType=" << static_cast<int>(type())
                << " ownerTag=\"" << tag() << '"'
                << " ownerBlueprint=\"" << blueprintResRef() << '"'
                << " queueIndex=" << queueIndex
                << " actionType=" << static_cast<int>(action.type())
                << " runtimeClass=" << typeid(action).name()
                << " provenance="
                << (action.originalSavedAction() ? "loaded" : "runtime-created");
        if (action.originalSavedAction()) {
            message << " savedActionId=" << action.originalSavedAction()->actionId
                    << " groupActionId="
                    << action.originalSavedAction()->groupActionId;
        }
        return ValidationException(message.str());
    };
    auto saveAction = [this](const Action &action, size_t queueIndex) {
        try {
            return action.saveFacingState();
        } catch (const std::exception &ex) {
            std::ostringstream message;
            message << ex.what()
                    << "; ownerId=" << id()
                    << " ownerType=" << static_cast<int>(type())
                    << " ownerClass=" << typeid(*this).name()
                    << " ownerTag=\"" << tag() << '\"'
                    << " ownerBlueprint=\"" << blueprintResRef() << '\"'
                    << " queueIndex=" << queueIndex
                    << " actionType=" << static_cast<int>(action.type())
                    << " runtimeClass=" << typeid(action).name()
                    << " actionProvenance="
                    << (action.originalSavedAction() ? "present" : "absent");
            if (action.originalSavedAction()) {
                message << " savedActionId=" << action.originalSavedAction()->actionId
                        << " groupActionId=" << action.originalSavedAction()->groupActionId;
            }
            throw ValidationException(message.str());
        }
    };
    for (size_t index = 0; index < _actions.nodes.size(); ++index) {
        const auto &node = _actions.nodes[index];
        if (!node->action) {
            if (node->opaque) {
                result.push_back(*node->opaque);
                result.back().groupActionId = node->groupId;
            }
            continue;
        }
        const auto &action = node->action;
        if (action->isCompleted() || action->isCancelled()) continue;
        if (auto saved = saveAction(*action, index)) {
            saved->groupActionId = node->groupId;
            saved->round = _game.combat().saveRound(*action);
            saved->scheduled = action->isScheduledCommand();
            result.push_back(std::move(*saved));
        } else {
            throw unsupportedAction(*action, index);
        }
    }
    return result;
}

void Object::retireAreaRuntimeState(
    const std::set<const Object *> &retainedObjects) {
    // The authoritative source snapshot was captured before this boundary.
    // Discard rather than cancel: cancellation callbacks are live gameplay and
    // must not mutate the already-frozen outgoing world.
    for (auto &action : _actions) {
        // Conversation cancellation only retires its presentation admission.
        // Other actions are discarded without invoking gameplay callbacks.
        if (auto conversation = dyn_cast<StartConversationAction>(action)) {
            conversation->cancel(action, *this);
        }
        if (action) action->markCancelled();
    }
    _actions.clear();
    _delayed.clear();
    _executingActionNode.reset();
    _savedActionQueue = SavedActionQueue {};
    _savedScheduledActions.clear();
    _savedScheduledReferencesBound.clear();
    _savedScheduledTime = 0.0f;

    for (auto &effect : _effects) {
        if (effect.effect) {
            effect.effect->retireAreaRuntime(retainedObjects);
        }
        effect.retireAreaRuntimeBindings(retainedObjects);
    }
    _savedEffects.clear();
    _savedRuntimeParsed = false;
    _savedRuntimePublished = false;

    // Rebase object-local bindings exactly as effects are rebased. Master/owner
    // relations between retained session objects remain meaningful; every
    // outgoing Area binding retires. A2 separately owns the saved-graph
    // namespace, translation, and generation.
    std::map<std::string, uint32_t> retainedReferenceIds;
    std::map<std::string, RuntimeObjectRef<Object>> retainedReferences;
    for (const auto &[field, binding] : _savedReferences) {
        auto object = binding.resolve();
        if (!object || retainedObjects.count(object.get()) == 0) continue;
        retainedReferenceIds.emplace(field, object->id());
        retainedReferences.emplace(field, object);
    }
    _savedReferenceIds = std::move(retainedReferenceIds);
    _savedReferences = std::move(retainedReferences);
    _lastHostileActor.reset();
    auto lastDamager = _lastDamager.resolve();
    if (!lastDamager || retainedObjects.count(lastDamager.get()) == 0) {
        _lastDamager.reset();
        _lastDamageAmounts.fill(-1);
        if (_savedLastDamagerId) {
            _savedLastDamagerId = script::kObjectInvalid;
        }
    } else {
        _savedLastDamagerId = lastDamager->id();
    }
}

void Object::resolveSavedReferences(
    const std::function<std::shared_ptr<Object>(uint32_t)> &resolver) {
    _savedReferences.clear();
    for (const auto &[field, id] : _savedReferenceIds) {
        if (auto object = resolver(id)) {
            _savedReferences.emplace(field, object);
        }
    }
    _lastDamager.reset();
    _lastDamageAmounts.fill(-1);
    if (_savedLastDamagerId &&
        *_savedLastDamagerId != script::kObjectInvalid) {
        _lastDamager = resolver(*_savedLastDamagerId);
    }
}

std::shared_ptr<Object> Object::savedReference(std::string_view field) const {
    auto found = _savedReferences.find(std::string(field));
    return found == _savedReferences.end() ? nullptr : found->second.resolve();
}

uint32_t Object::getLastHostileActor() const {
    auto actor = _lastHostileActor.resolve();
    return actor ? actor->id() : script::kObjectInvalid;
}

Object::Object(uint32_t id, ObjectType type, std::string sceneName,
               Game &game, ServicesView &services) :
    _id(id), _type(type), _sceneName(std::move(sceneName)), _game(game), _services(services) {
    _lastDamageAmounts.fill(-1);
    _raiseable = game.isTSL();
}

void Object::setDestroyability(bool destroyable, bool raiseable, bool selectableWhenDead) {
    _destroyable = destroyable;
    _raiseable = raiseable;
    _selectableWhenDead = selectableWhenDead;
    if (auto *creature = dyn_cast<Creature>(this)) creature->onDestroyabilityChanged();
}

void Object::setLastHostileActor(uint32_t actor) {
    if (actor == script::kObjectInvalid) {
        _lastHostileActor.reset();
        return;
    }
    _lastHostileActor = _game.getObjectById(actor);
}

uint32_t Object::getLastDamager() const {
    auto damager = _lastDamager.resolve();
    return damager ? damager->id() : script::kObjectInvalid;
}

void Object::setLastDamager(const std::shared_ptr<Object> &damager) {
    _lastDamager = damager;
    _savedLastDamagerId = damager
                              ? damager->id()
                              : script::kObjectInvalid;
}

void Object::clearAllActions(bool force) {
    if (force) _game.combat().discardEquipment(*this);
    auto &nodes = _actions.nodes;
    auto erase = [&](const OrdinaryActionQueue::Node &node) {
        auto position = std::find(nodes.begin(), nodes.end(), node);
        if (position != nodes.end()) nodes.erase(position);
    };
    auto cancel = [&](const OrdinaryActionQueue::Node &node) {
        if (node->action) {
            auto action = node->action;
            action->cancel(action, *this);
            action->markCancelled();
        }
        erase(node);
    };
    // Preserve the pre-existing forced/UI policy, separate from script clearing.
    if (!force) {
        if (auto executing = _executingActionNode.lock()) {
            while (!nodes.empty() && nodes.front() != executing) {
                auto node = nodes.front();
                if (node->action && node->action->locked()) break;
                cancel(node);
            }
            if (!nodes.empty() && nodes.front() == executing) return;
        }
    }
    while (!nodes.empty()) {
        auto node = nodes.back();
        if (!force && node->action && node->action->locked()) break;
        cancel(node);
    }
}

void Object::clearCommandActions() {
    if (!_commandable) return;
    // K1/K2 ClearAllActions advances the node cursor before ClearAction.
    detail::removeCommandActions(_actions.nodes, [this](const OrdinaryActionQueue::Node &node) {
        if (!node->clearable || !node->action) return false;
        if (auto action = node->action) {
            if (!action->isClearable() || !action->cancel(action, *this)) return false;
            action->markCancelled();
        }
        return true;
    });
}

void Object::discardHostileActionGroups() {
    std::vector<OrdinaryActionQueue::Node> nodes(_actions.nodes.begin(), _actions.nodes.end());
    detail::discardCombatActionGroups(nodes, _game.isTSL(),
        [](const auto &node) { return node->groupId; },
        [](const auto &node) { return node->actionId; },
        [this](const OrdinaryActionQueue::Node &node) {
            // RemoveGroup bypasses commandability, clearability and callbacks.
            if (node->action) node->action->markCancelled();
            auto &queue = _actions.nodes;
            auto position = std::find(queue.begin(), queue.end(), node);
            if (position != queue.end()) queue.erase(position);
        });
}

void Object::addAction(std::shared_ptr<Action> action, uint16_t group) {
    if (!isRuntimeLive() || !action) return;
    auto node = std::make_shared<ActionQueueNode>();
    node->action = action;
    node->actionId = action->serializedActionId();
    if (const auto &saved = action->originalSavedAction();
        saved && group == OrdinaryActionQueue::kNewGroup) group = saved->groupActionId;
    node->groupId = _actions.allocateGroup(group);
    if (_game.combat().scheduleEquipment(*this, node)) return;
    _actions.nodes.push_back(node);
    action->onQueued(*this);
}

void Object::addActionOnTop(std::shared_ptr<Action> action, uint16_t group) {
    if (!isRuntimeLive() || !action) return;
    auto node = std::make_shared<ActionQueueNode>();
    node->action = action;
    node->actionId = action->serializedActionId();
    if (const auto &saved = action->originalSavedAction();
        saved && group == OrdinaryActionQueue::kNewGroup) group = saved->groupActionId;
    node->groupId = _actions.allocateGroup(group);
    if (_game.combat().scheduleEquipment(*this, node)) return;
    _actions.nodes.push_front(node);
    action->onQueued(*this);
}

bool Object::addActionBefore(const Action &parent, std::shared_ptr<Action> action) {
    if (!isRuntimeLive() || !action) return false;
    auto &nodes = _actions.nodes;
    auto executing = _executingActionNode.lock();
    auto position = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
        return executing && executing->action.get() == &parent
                   ? node == executing : node->action.get() == &parent;
    });
    if (position == nodes.end()) return false;
    auto node = std::make_shared<ActionQueueNode>();
    node->action = action;
    node->actionId = action->serializedActionId();
    // Front insertion still interprets an inherited 0xfffe through
    // the allocator (the most recently allocated group), rather than treating
    // that sentinel as a literal even after the counter has wrapped.
    node->groupId = _actions.allocateGroup((*position)->groupId);
    nodes.insert(position, node);
    action->onQueued(*this);
    return true;
}

void Object::requeueActionNode(const OrdinaryActionQueue::Node &node) {
    if (!isRuntimeLive() || !node) return;
    node->groupId = _actions.allocateGroup(OrdinaryActionQueue::kNewGroup);
    _actions.nodes.push_front(node);
}

void Object::addOpaqueAction(SavedActionRecord record) {
    auto node = std::make_shared<ActionQueueNode>();
    node->actionId = record.actionId;
    node->groupId = _actions.allocateGroup(record.groupActionId);
    node->opaque = std::move(record);
    _actions.nodes.push_back(std::move(node));
}

void Object::delayAction(std::shared_ptr<Action> action, float seconds) {
    if (!isRuntimeLive()) return;
    DelayedAction delayed;
    delayed.action = std::move(action);
    delayed.timer = std::make_unique<Timer>(seconds);
    _delayed.push_back(std::move(delayed));
}

void Object::updateActions(float dt) {
    if (isDead()) {
        clearAllActions(/*force=*/true);
        return;
    }
    removeCompletedActions();
    updateDelayedActions(dt);
}

void Object::removeCompletedActions() {
    while (true) {
        std::shared_ptr<Action> action(getCurrentAction());
        if (!action || !action->isCompleted())
            return;

        _actions.nodes.pop_front();
    }
}

void Object::updateDelayedActions(float dt) {
    // Iterate in reverse order, so addActionOnTop keeps the original order.
    for (auto delayed = _delayed.rbegin(), end = _delayed.rend(); delayed != end; ++delayed) {
        delayed->timer->update(dt);
        if (delayed->timer->elapsed()) {
            addActionOnTop(std::move(delayed->action));
        }
    }
    auto delayedToRemove = std::remove_if(
        _delayed.begin(),
        _delayed.end(),
        [](const DelayedAction &delayed) { return delayed.timer->elapsed(); });

    _delayed.erase(delayedToRemove, _delayed.end());
}

void Object::executeActions(float dt) {
    if (_actions.nodes.empty()) return;
    auto node = _actions.nodes.front();
    auto action = node->action;
    // Unsupported records remain in place, never silently overtaken.
    if (!action) {
        if (!node->refusalReported) {
            warn("Cannot execute unsupported action " + std::to_string(node->actionId));
            node->refusalReported = true;
        }
        return;
    }
    const auto *creature = dyn_cast<Creature>(this);
    if (creature && _game.combat().blocksOwnerActions(*creature)) return;
    if (!action->runtimeDependenciesLive() || (creature && !creature->permitsAction(*action))) {
        action->cancel(action, *this);
        action->markCancelled();
        if (!action->isCompleted()) {
            action->complete();
        }
        return;
    }
    _executingActionNode = node;
    try {
        action->execute(action, *this, dt);
    } catch (...) {
        _executingActionNode.reset();
        throw;
    }
    _executingActionNode.reset();
}

bool Object::hasUserActionsPending(const Action *excluded) const {
    // TODO: must only work during combat
    for (const auto &action : _actions) {
        if (action.get() != excluded && action->isUserAction()) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<Action> Object::getCurrentAction() const {
    return _actions.nodes.empty() ? nullptr : _actions.nodes.front()->action;
}

std::shared_ptr<Item> Object::addItem(const std::string &resRef, int stackSize, bool dropable) {
    std::shared_ptr<Item> result;
    std::vector<std::shared_ptr<Object>> noObsolete;
    std::vector<std::shared_ptr<Item>> replacement(_items);
    ItemAttributes replacementAttributes;
    auto creature = dyn_cast<Creature>(this);
    if (creature) replacementAttributes = creature->itemAttributes();
    _game.replaceRuntimeObjectGraph(
        noObsolete,
        [&]() {
            auto candidate = _game.newItemFromBlueprint(resRef);
            candidate->setStackSize(stackSize);
            candidate->setDropable(dropable);
            candidate->setOwner(_id);
            result = appendOwnedItemCandidate(
                replacement, candidate, false);
            if (creature && result.get() == candidate.get()) {
                replacementAttributes.addItem(candidate, _services.game);
            }
        },
        [&]() noexcept {
            _items = std::move(replacement);
            if (creature) {
                creature->itemAttributes() =
                    std::move(replacementAttributes);
            }
        });

    return result;
}

void Object::addItem(const std::shared_ptr<Item> &item) {
    if (!item || (!item->isRuntimeLive() && !item->isPresentationOnly())) {
        throw ValidationException("Cannot own a non-live runtime item");
    }
    if (item->isEquipped()) {
        throw ValidationException(
            "Cannot add an Item while it still has an equipment owner edge");
    }
    if (isActiveAreaOwnedItem(_game, item)) {
        throw ValidationException(
            "Cannot add an Item while it still has an Area ownership edge");
    }
    if (item->owner() != 0 && item->owner() != script::kObjectInvalid &&
        item->owner() != _id) {
        throw ValidationException("Runtime item already has another owner");
    }
    auto alreadyOwned = std::find(_items.begin(), _items.end(), item);
    if (alreadyOwned != _items.end()) {
        // Re-adding an owned stack restores one consumed item.
        if ((*alreadyOwned)->stackSize() < (*alreadyOwned)->maxStackSize()) {
            (*alreadyOwned)->setStackSize((*alreadyOwned)->stackSize() + 1);
        }
        return;
    }
    for (const auto &existing : _items) {
        if (!existing->isStackCompatibleWith(*item)) {
            continue;
        }
        if (existing->mergeStackFrom(*item)) {
            item->setOwner(0);
            _game.destroyRuntimeObjectGraph(item);
            return;
        }
    }
    std::vector<std::shared_ptr<Item>> replacement(_items);
    replacement.push_back(item);
    auto creature = dyn_cast<Creature>(this);
    ItemAttributes replacementAttributes;
    if (creature) {
        replacementAttributes = creature->itemAttributes();
        replacementAttributes.addItem(item, _services.game);
    }
    _items = std::move(replacement);
    item->setOwner(_id);
    if (creature) {
        creature->itemAttributes() = std::move(replacementAttributes);
    }
}

bool Object::removeItem(const std::shared_ptr<Item> &item, bool &last) {
    auto maybeItem = find(_items.begin(), _items.end(), item);
    if (maybeItem == _items.end())
        return false;

    last = false;

    int stackSize = (*maybeItem)->stackSize();
    if (stackSize > 1) {
        (*maybeItem)->setStackSize(stackSize - 1);
    } else {
        last = true;
        _items.erase(maybeItem);
        item->setOwner(0);
        if (Creature *creature = dyn_cast<Creature>(this)) {
            creature->itemAttributes().removeItem(item);
        }
    }

    return true;
}

bool Object::removeItemStack(const std::shared_ptr<Item> &item) {
    auto maybeItem = find(_items.begin(), _items.end(), item);
    if (maybeItem == _items.end()) {
        return false;
    }

    _items.erase(maybeItem);
    if (Creature *creature = dyn_cast<Creature>(this)) {
        creature->itemAttributes().removeItem(item);
    }
    item->setOwner(0);

    return true;
}

float Object::getDistanceTo(const glm::vec2 &point) const {
    return glm::distance(glm::vec2(_position), point);
}

float Object::getSquareDistanceTo(const glm::vec2 &point) const {
    return glm::distance2(glm::vec2(_position), point);
}

float Object::getDistanceTo(const glm::vec3 &point) const {
    return glm::distance(_position, point);
}

float Object::getSquareDistanceTo(const glm::vec3 &point) const {
    return glm::distance2(_position, point);
}

float Object::getDistanceTo(const Object &other) const {
    return glm::distance(_position, other._position);
}

float Object::getSquareDistanceTo(const Object &other) const {
    return glm::distance2(_position, other._position);
}

bool Object::contains(const glm::vec3 &point) const {
    if (!_sceneNode)
        return false;

    const AABB &aabb = _sceneNode->aabb();

    return (aabb * _transform).contains(point);
}

void Object::face(const Object &other) {
    if (_id != other._id) {
        face(other._position);
    }
}

void Object::face(const glm::vec3 &point) {
    if (point == _position)
        return;

    glm::vec2 dir(glm::normalize(point - _position));
    _orientation = glm::quat(glm::vec3(0.0f, 0.0f, -glm::atan(dir.x, dir.y)));
    updateTransform();
}

void Object::faceAwayFrom(const Object &other) {
    if (_id == other._id || _position == other.position())
        return;

    glm::vec2 dir(glm::normalize(_position - other.position()));
    _orientation = glm::quat(glm::vec3(0.0f, 0.0f, -glm::atan(dir.x, dir.y)));
    updateTransform();
}

void Object::moveDropableItemsTo(Object &other) {
    bool otherInParty = _game.party().isMember(other);
    for (auto it = _items.begin(); it != _items.end();) {
        if ((*it)->isDropable()) {
            std::shared_ptr<Item> item(*it);
            it = _items.erase(it);
            if (Creature *creature = dyn_cast<Creature>(this)) {
                creature->itemAttributes().removeItem(item);
            }
            // Credits looted by the party feed the shared gold pool instead of
            // the inventory; the stack size is the credit amount.
            if (otherInParty && item->isCredits()) {
                _game.party().giveGold(item->stackSize());
                item->setOwner(0);
                _game.destroyRuntimeObjectGraph(item);
            } else {
                item->setOwner(0);
                other.addItem(item);
            }
        } else {
            ++it;
        }
    }
}

void Object::heal(int amount) {
    if (auto *creature = dyn_cast<Creature>(this))
        creature->applyHealingEffect(amount, nullptr, false);
}

bool Object::applyEffect(const std::shared_ptr<Effect> &effect,
                         DurationType durationType, float duration) {
    EffectInstance instance = effect->saveFacingInstance();
    if (!std::dynamic_pointer_cast<SavedEffectValue>(effect)) {
        instance.effect = effect;
    }
    instance.setDuration(durationType, duration);
    instance.skipOnLoad = false;
    instance.restoring = false;
    return applyEffect(std::move(instance));
}

bool Object::applyEffect(EffectInstance effect) {
    if (!isRuntimeLive()) return false;
    if (effect.hasStableId()) _game.importEffectId(effect.id);
    else effect.id = _game.allocateEffectId();
    if (!effect.restoring) effect.setDuration(effect.durationType(), effect.duration);
    effect.materialize();
    return admitEffect(std::move(effect));
}

bool Object::restoreEffect(EffectInstance effect) {
    if (!effect.shouldRestoreOnLoad()) return false;
    effect.restoring = true;
    effect.materialize();
    return admitEffect(std::move(effect));
}

bool Object::admitEffect(EffectInstance effect) {
    const uint64_t start = _game.worldTimeMilliseconds();
    effect.skipOnLoad = effect.skipOnLoad || effect.exposed == 0;
    const auto result = effect.effect
        ? effect.effect->onApply(*this, effect)
        : EffectApplicationResult::Retained;
    if (result == EffectApplicationResult::Rejected) return false;
    if (result == EffectApplicationResult::Applied) return true;

    if (effect.durationType() == DurationType::Temporary) {
        if (effect.expiryOrigin == EffectExpiryOrigin::RuntimeCountdown ||
            effect.expiryOrigin == EffectExpiryOrigin::None) {
            const auto expiry = getEffectExpiryMilliseconds(start, effect.duration,
                static_cast<uint32_t>(_game.millisecondsPerWorldDay()));
            effect.expiryDay = static_cast<uint32_t>(expiry / _game.millisecondsPerWorldDay());
            effect.expiryTime = static_cast<uint32_t>(expiry % _game.millisecondsPerWorldDay());
            effect.expiryOrigin = EffectExpiryOrigin::RuntimeAbsoluteGameTime;
        }
        effect.remainingDuration = _game.remainingEffectDuration(effect);
    }
    // Admission may consume a wrapper, change its duration, or insert children.
    // Assign order only after the handler; equal types remain stable.
    effect.applicationOrder = _nextEffectApplicationOrder++;
    effect.restoring = false;
    auto position = std::find_if(_effects.begin(), _effects.end(),
        [&](const EffectInstance &candidate) {
            return candidate.serializedType > effect.serializedType;
        });
    if (auto *creature = dyn_cast<Creature>(this)) creature->addArmorClassEffect(effect);
    // Visibility consumers consult the canonical collection, so counter
    // effects must become visible there before perception (and OnNotice) runs.
    const bool changesVisibility = effect.type() == EffectType::Invisibility ||
                                   effect.type() == EffectType::SeeInvisible ||
                                   effect.type() == EffectType::Ultravision ||
                                   effect.type() == EffectType::TrueSeeing ||
                                   effect.type() == EffectType::Blindness;
    _effects.insert(position, std::move(effect));
    if (auto *creature = dyn_cast<Creature>(this)) creature->updateArmorClassEffectCursor();
    if (changesVisibility) {
        if (auto *creature = dyn_cast<Creature>(this)) creature->refreshVisibilityPerception();
    }
    return true;
}

size_t Object::removeEffectsById(EffectId id) {
    bool changesVisibility = false;
    const auto count = detail::removeEffectPackage(_effects, id, _removingEffectApplications,
        [&](const EffectInstance &value) {
            const auto result = value.effect ? value.effect->onRemove(*this, value) : EffectRemovalResult::Removed;
            if (result == EffectRemovalResult::Removed) {
                if (auto *creature = dyn_cast<Creature>(this)) creature->removeArmorClassEffect(value);
            }
            changesVisibility |= result == EffectRemovalResult::Removed &&
                (value.type() == EffectType::Invisibility || value.type() == EffectType::Blindness);
            return result;
        }, [&]() {
            if (auto *creature = dyn_cast<Creature>(this)) creature->updateArmorClassEffectCursor();
        });
    if (count && changesVisibility) {
        if (auto *creature = dyn_cast<Creature>(this)) creature->refreshVisibilityPerception();
    }
    return count;
}

bool Object::removeEffectApplication(uint64_t order) {
    bool changesVisibility = false;
    const bool removed = detail::removeEffectApplication(_effects, order, _removingEffectApplications,
        [&](const EffectInstance &value) {
            const auto result = value.effect ? value.effect->onRemove(*this, value) : EffectRemovalResult::Removed;
            if (result == EffectRemovalResult::Removed) {
                if (auto *creature = dyn_cast<Creature>(this)) creature->removeArmorClassEffect(value);
            }
            changesVisibility |= result == EffectRemovalResult::Removed &&
                (value.type() == EffectType::Invisibility || value.type() == EffectType::Blindness);
            return result;
        }, [&]() {
            if (auto *creature = dyn_cast<Creature>(this)) creature->updateArmorClassEffectCursor();
        });
    if (removed && changesVisibility) {
        if (auto *creature = dyn_cast<Creature>(this)) creature->refreshVisibilityPerception();
    }
    return removed;
}

EffectPackageApplicationResult Object::applyEffectPackage(const std::vector<EffectInstance> &members) {
    return detail::applyEffectPackageMembers(members, [&](const EffectInstance &member) {
        return applyEffect(member);
    });
}

void Object::updateEffects(float dt) {
    std::vector<uint64_t> applications;
    applications.reserve(_effects.size());
    for (const auto &effect : _effects) applications.push_back(effect.applicationOrder);
    for (uint64_t order : applications) {
        auto *effect = findEffectApplication(order);
        if (!effect) continue;
        // The callback receives a value snapshot; the canonical record remains
        // in the collection and is re-found after any nested application/removal.
        EffectInstance callbackValue(*effect);
        if (callbackValue.effect) callbackValue.effect->onUpdate(*this, callbackValue, dt);
        effect = findEffectApplication(order);
        if (!effect) continue;
        const bool retired = effect->durationType() == DurationType::Equipped &&
                             !effect->hasLiveRuntimeSource();
        const bool temporary = effect->durationType() == DurationType::Temporary;
        if (temporary) effect->remainingDuration = _game.remainingEffectDuration(*effect);
        const auto expiry = static_cast<uint64_t>(effect->expiryDay) * _game.millisecondsPerWorldDay() +
                            effect->expiryTime;
        if (retired || (temporary && _game.worldTimeMilliseconds() > expiry)) {
            const EffectId id = effect->id;
            removeEffectsById(id);
        }
    }
}

void Object::playAnimation(AnimationType animation, AnimationProperties properties) {
}

bool Object::isSelectable() const {
    return false;
}

glm::vec3 Object::getSelectablePosition() const {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    return model ? model->getWorldCenterOfAABB() : _position;
}

void Object::setRoom(Room *room) {
    if (_room) {
        _room->removeTenant(this);
    }
    _room = room;

    if (_room) {
        _room->addTenant(this);
    }
}

void Object::setPosition(const glm::vec3 &position) {
    _position = position;
    if (_spatialArea) {
        _spatialArea->updateObjectSpatialIndex(*this);
    }
    updateTransform();
}

void Object::updateTransform() {
    _transform = glm::translate(glm::mat4(1.0f), _position);
    _transform *= glm::mat4_cast(_orientation);

    if (_sceneNode && !_stunt) {
        _sceneNode->setLocalTransform(_transform);
    }
}

void Object::setFacing(float facing) {
    _orientation = glm::quat(glm::vec3(0.0f, 0.0f, facing));
    updateTransform();
}

void Object::setVisible(bool visible) {
    if (_visible == visible)
        return;

    _visible = visible;

    if (_sceneNode) {
        _sceneNode->setEnabled(visible);
    }
}

std::shared_ptr<Item> Object::getFirstItem() {
    _itemIndex = 0;
    return getNextItem();
}

std::shared_ptr<Item> Object::getNextItem() {
    int itemCount = static_cast<int>(_items.size());
    if (itemCount > _itemIndex) {
        return _items[_itemIndex++];
    }
    return nullptr;
}

std::shared_ptr<Item> Object::getItemByTag(const std::string &tag) {
    for (auto &item : _items) {
        if (item->tag() == tag)
            return item;
    }
    return nullptr;
}

void Object::clearAllEffects() {
    _clearingEffects = true;
    std::vector<EffectId> ids;
    for (const auto &effect : _effects) ids.push_back(effect.id);
    for (auto id : ids) removeEffectsById(id);
    if (_effects.empty()) onEffectsCleared();
    _clearingEffects = false;
}

void Object::queueScriptEffectRemoval(ScriptEffectRemovalMatch match, const EffectInstance &value) {
    for (EffectId id : selectScriptEffectRemovals(_effects, match, value)) {
        _game.queueEffectRemoval(*this, id);
    }
}

void Object::removeEffect(const std::shared_ptr<Effect> &effect) {
    auto at = std::find_if(_effects.begin(), _effects.end(),
        [&](const EffectInstance &record) { return record.effect == effect; });
    if (at != _effects.end()) removeEffectApplication(at->applicationOrder);
}

bool Object::hasEffect(EffectType type) const {
    return std::any_of(
        _effects.begin(),
        _effects.end(),
        [type](const EffectInstance &applied) {
            return applied.hasLiveRuntimeSource() &&
                   applied.type() == type;
        });
}

void Object::replaceEffectState(std::deque<EffectInstance> replacement) noexcept {
    std::vector<uint64_t> departures;
    for (const auto &record : _effects) {
        const bool survives = std::any_of(replacement.begin(), replacement.end(),
            [&](const EffectInstance &candidate) {
                return candidate.applicationOrder == record.applicationOrder;
            });
        if (!survives) departures.push_back(record.applicationOrder);
    }
    for (const auto order : departures) removeEffectApplication(order);
    // The ownership edge was committed by the equipment transaction. Existing
    // canonical records (including callback mutations) stay in place; only the
    // newly materialized equipment entries have no application order yet.
    for (auto &record : replacement) {
        if (record.applicationOrder == 0) admitEffect(std::move(record));
    }
}

int Object::applyDamageToHitPoints(int amount, int currentHitPoints) {
    bool minimumOne = isMinOneHP();
    int minimumHitPoints = minimumOne ? 1 : 0;
    int adjustedAmount = minimumOne
                             ? std::min(amount, std::max(0, currentHitPoints - minimumHitPoints))
                             : amount;
    _currentHitPoints = std::max(minimumHitPoints, currentHitPoints - amount);
    return adjustedAmount;
}

void Object::damage(
    int amount,
    const std::shared_ptr<Object> &damager) {
}

void Object::startStuntMode() {
    if (_sceneNode) {
        _sceneNode->setLocalTransform(glm::mat4(1.0f));
        _sceneNode->setCullingEnabled(false);
    }
    _stunt = true;
}

void Object::stopStuntMode() {
    if (!_stunt)
        return;

    if (_sceneNode) {
        _sceneNode->setLocalTransform(_transform);
        _sceneNode->setCullingEnabled(true);
    }
    _stunt = false;
}

std::shared_ptr<Effect> Object::getFirstEffect() {
    _effectIndex = 0;
    return getNextEffect();
}

std::shared_ptr<Effect> Object::getNextEffect() {
    while (_effectIndex < _effects.size()) {
        const auto &record = _effects[_effectIndex++];
        if (record.isScriptEnumerable()) return std::make_shared<SavedEffectValue>(record);
    }
    return nullptr;
}

void Object::applyDamageEffect(
    int amount,
    const std::shared_ptr<Object> &damager) {

    damage(amount, damager);
}

int Object::getLastDamageAmountByType(int damageTypeFlag) const {
    return getDamageAmountByType(_lastDamageAmounts, damageTypeFlag);
}

int Object::getTotalDamageDealt() const {
    return getTotalDamageAmounts(_lastDamageAmounts);
}

EffectInstance *Object::findEffectApplication(uint64_t applicationOrder) {
    auto it = std::find_if(_effects.begin(), _effects.end(),
        [applicationOrder](const EffectInstance &instance) {
            return instance.applicationOrder == applicationOrder;
        });
    return it == _effects.end() ? nullptr : &*it;
}

void Object::setFeedbackText(std::string text, float duration) {
    _feedbackText = std::move(text);
    _feedbackTextRemaining = std::max(0.0f, duration);
    if (_feedbackTextRemaining == 0.0f) _feedbackText.clear();
}

} // namespace game

} // namespace reone
