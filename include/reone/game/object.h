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

#include "reone/scene/animproperties.h"
#include "reone/scene/graph.h"
#include "reone/scene/node.h"
#include "reone/scene/user.h"
#include "reone/script/types.h"
#include "reone/system/cast.h"
#include "reone/system/timer.h"

#include "action.h"
#include "actionqueue.h"
#include "castspell.h"
#include "action/playanimation.h"
#include "effect.h"
#include "runtimeref.h"
#include "saveprovenance.h"
#include "savedruntime.h"
#include "types.h"

namespace reone {

namespace resource {
class Gff;
}

namespace game {

struct ServicesView;

class Action;
class Area;
class Game;
class Item;
class ModuleSnapshotBuilder;
class Room;

class Object : public scene::IUser, boost::noncopyable {
    friend class Area;
public:
    enum class RuntimeState {
        Constructing,
        Live,
        Retired,
        Presentation,
    };

    virtual ~Object() = default;

    static bool classof(Object *from) {
        return true;
    }

    void deserialize(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);

    virtual void update(float dt);
    virtual void damage(
        int amount,
        const std::shared_ptr<Object> &damager);
    virtual void applyDamageEffect(
        int amount,
        const std::shared_ptr<Object> &damager);
    void heal(int amount);

    void face(const Object &other);
    void face(const glm::vec3 &point);
    void faceAwayFrom(const Object &other);

    bool contains(const glm::vec3 &point) const;

    virtual bool isSelectable() const;
    bool isOpen() const { return _open; }

    bool isMinOneHP() const { return _minOneHP; }
    bool isDead() const { return _dead; }
    bool isDestroyable() const { return _destroyable; }
    bool isRaiseable() const { return _raiseable; }
    bool isSelectableWhenDead() const { return _selectableWhenDead; }
    void setRaiseable(bool value) { _raiseable = value; }
    void setDestroyability(bool destroyable, bool raiseable, bool selectableWhenDead);

    bool isCommandable() const { return _commandable; }
    bool isInConversation() const { return _isInConversation; }

    float getDistanceTo(const glm::vec2 &point) const;
    float getSquareDistanceTo(const glm::vec2 &point) const;
    float getDistanceTo(const glm::vec3 &point) const;
    float getSquareDistanceTo(const glm::vec3 &point) const;
    float getDistanceTo(const Object &other) const;
    float getSquareDistanceTo(const Object &other) const;

    virtual glm::vec3 getSelectablePosition() const;
    float getFacing() const { return glm::eulerAngles(_orientation).z; }

    uint32_t id() const { return _id; }
    virtual std::string getOnSpellCastAt() const { return {}; }
    void setFeedbackText(std::string text, float duration);
    const std::string &feedbackText() const { return _feedbackText; }
    Game &game() const { return _game; }
    ServicesView &services() const { return _services; }
    bool isRuntimeLive() const { return _runtimeState == RuntimeState::Live; }
    bool isPresentationOnly() const {
        return _runtimeState == RuntimeState::Presentation;
    }
    uint64_t runtimeIncarnation() const { return _runtimeIncarnation; }
    const std::string &tag() const { return _tag; }
    ObjectType type() const { return _type; }
    const std::string &blueprintResRef() const { return _blueprintResRef; }
    const std::string &name() const { return _name; }
    const std::string &conversation() const { return _conversation; }
    bool plotFlag() const { return _plot; }
    SpellType spellCast() const { return _spellCast; }
    void setSpellCast(SpellType spell) { _spellCast = spell; }
    uint32_t effectSpellId() const { return _effectSpellId; }
    void setEffectSpellId(uint32_t spellId) { _effectSpellId = spellId; }
    const SpellCastContext &spellCastContext() const { return _spellCastContext; }
    void setSpellCastContext(SpellCastContext context) { _spellCastContext = context; }

    Room *room() const { return _room; }
    const glm::vec3 &position() const { return _position; }
    const glm::mat4 &transform() const { return _transform; }
    bool visible() const { return _visible; }
    std::shared_ptr<scene::SceneNode> sceneNode() const { return _sceneNode; }

    void setTag(std::string tag) { _tag = std::move(tag); }
    void setConversation(std::string conversation) { _conversation = std::move(conversation); }
    void setName(std::string name) { _name = std::move(name); }
    void setPlotFlag(bool plot) { _plot = plot; }
    void setCommandable(bool commandable) { _commandable = commandable; }
    void setIsInConversation(bool isInConversation) { _isInConversation = isInConversation; }

    void setRoom(Room *room);
    void setPosition(const glm::vec3 &position);
    void setFacing(float facing);
    void setVisible(bool visible);

    // Animation

    virtual void playAnimation(AnimationType type, scene::AnimationProperties properties = scene::AnimationProperties());

    virtual std::string getAnimationName(AnimationType type) const { return ""; }
    virtual std::string getActiveAnimationName() const { return ""; };

    // END Animation

    // Inventory

    std::shared_ptr<Item> addItem(const std::string &resRef, int stackSize = 1, bool dropable = true);
    void addItem(const std::shared_ptr<Item> &item);
    bool removeItem(const std::shared_ptr<Item> &item, bool &last);
    bool removeItemStack(const std::shared_ptr<Item> &item);
    void moveDropableItemsTo(Object &other);

    std::shared_ptr<Item> getFirstItem();
    std::shared_ptr<Item> getNextItem();
    std::shared_ptr<Item> getItemByTag(const std::string &tag);

    const std::vector<std::shared_ptr<Item>> &items() const { return _items; }

    // Runtime children whose semantic lifetime is owned by this object. New
    // nested game-object types participate in registry finalization by
    // extending this list; Game does not need to know their concrete type.
    virtual std::vector<std::shared_ptr<Object>> ownedRuntimeObjects() const;

    // END Inventory

    // Effects

    void clearAllEffects();
    void removeEffect(const std::shared_ptr<Effect> &effect);
    void queueScriptEffectRemoval(ScriptEffectRemovalMatch match, const EffectInstance &value);
    bool applyEffect(const std::shared_ptr<Effect> &effect, DurationType durationType, float duration = 0.0f);
    bool applyEffect(EffectInstance effect);
    bool restoreEffect(EffectInstance effect);
    bool isClearingEffects() const { return _clearingEffects; }
    size_t removeEffectsById(EffectId id);
    bool removeEffectApplication(uint64_t applicationOrder);
    EffectPackageApplicationResult applyEffectPackage(const std::vector<EffectInstance> &members);

    const std::deque<EffectInstance> &effects() const { return _effects; }
    /** Find the canonical applied record for an exact executable payload. */
    EffectInstance *findEffectInstance(const Effect &effect);
    EffectInstance *findEffectApplication(uint64_t applicationOrder);
    std::vector<EffectInstance> saveEffectSnapshot() const;
    bool hasEffect(EffectType type) const;
    std::shared_ptr<Effect> getFirstEffect();
    std::shared_ptr<Effect> getNextEffect();

    // END Effects

    // Stunt mode

    bool isStuntMode() const { return _stunt; }

    /**
     * Places this object into the stunt mode. Objects in this mode have their
     * position and orientation fixed to the world origin. Subsequent changes to
     * position and orientation will be buffered and applied when
     * stopStuntMode is called.
     */
    void startStuntMode();

    void stopStuntMode();

    // END Stunt mode

    // Hit Points

    // Base maximum hit points, not considering any bonuses.
    int hitPoints() const { return _hitPoints; }

    // Maximum hit points, after considering all bonuses and penalties.
    int maxHitPoints() const { return _maxHitPoints; }

    // Current runtime hit points.
    virtual int currentHitPoints() const { return static_cast<int16_t>(_currentHitPoints); }
    int currentHitPointsWithoutTemporary() const { return static_cast<int16_t>(_currentHitPoints); }

    void setMinOneHP(bool minOneHP) { _minOneHP = minOneHP; }
    virtual void setMaxHitPoints(int maxHitPoints) { _maxHitPoints = maxHitPoints; }
    virtual void setCurrentHitPoints(int hitPoints) {
        _currentHitPoints = _minOneHP && hitPoints <= 0 ? 1 : hitPoints;
    }

    // END Hit Points

    // Actions

    virtual void clearAllActions(bool force = false);

    void clearCommandActions();
    void discardHostileActionGroups();
    void addAction(std::shared_ptr<Action> action,
                   uint16_t group = OrdinaryActionQueue::kNewGroup);
    void addActionOnTop(std::shared_ptr<Action> action,
                        uint16_t group = OrdinaryActionQueue::kNewGroup);
    bool addActionBefore(const Action &parent, std::shared_ptr<Action> action);
    bool hasOrdinaryActionsPending() const { return !_actions.nodes.empty(); }
    uint32_t currentSerializedActionId() const {
        return _actions.nodes.empty() ? 0xffff : _actions.nodes.front()->actionId;
    }
    void delayAction(std::shared_ptr<Action> action, float seconds);

    bool hasUserActionsPending(const Action *excluded = nullptr) const;

    std::shared_ptr<Action> getCurrentAction() const;

    const OrdinaryActionQueue &actions() const { return _actions; }
    uint16_t allocateActionGroup(uint16_t request) { return _actions.allocateGroup(request); }
    std::vector<SavedActionRecord> saveActionSnapshot() const;
    void requeueActionNode(const OrdinaryActionQueue::Node &node);

    /** Drop live execution and object bindings after their Area was captured. */
    void retireAreaRuntimeState(
        const std::set<const Object *> &retainedObjects);

    // END Actions

    // Combat

    uint32_t getLastHostileActor() const;

    void setLastHostileActor(uint32_t actor);
    int forceAlwaysUpdate() const { return _forceAlwaysUpdate; }
    void setForceAlwaysUpdate(int value) { _forceAlwaysUpdate = value; }

    uint32_t getLastDamager() const;
    void setLastDamager(const std::shared_ptr<Object> &damager);
    int getLastDamageAmountByType(int damageTypeFlag) const;
    int getTotalDamageDealt() const;
    void setLastDamageAmounts(const std::array<int, 15> &amounts) {
        _lastDamageAmounts = amounts;
    }

    // END Combat

    // Local variables

    bool getLocalBoolean(int index) const;
    int getLocalNumber(int index) const;

    const std::map<int, bool> &localBooleans() const { return _localBooleans; }
    const std::map<int, int> &localNumbers() const { return _localNumbers; }
    void deserializeRuntimeState(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    void bindSavedRuntimeState();
    void publishSavedRuntimeState();
    const std::vector<EffectInstance> &savedEffects() const { return _savedEffects; }
    const SavedActionQueue &savedActionQueue() const { return _savedActionQueue; }
    bool hasPublishedSavedRuntimeState() const { return _savedRuntimePublished; }
    bool isRestoringSavedRuntime() const { return _savedRuntimeParsed && !_savedRuntimePublished; }

    void captureSaveRecord(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext,
        SaveRecordOrigin origin = {});
    const std::optional<SaveRecordProvenance> &saveRecordProvenance() const {
        return _saveRecordProvenance;
    }
    std::optional<SerializedObjectIdentity> serializedObjectIdentity() const {
        return _saveRecordProvenance
                   ? _saveRecordProvenance->identity
                   : std::nullopt;
    }
    void assignSerializedObjectIdentity(
        const SerializedObjectIdentity &identity);


    void resolveSavedReferences(
        const std::function<std::shared_ptr<Object>(uint32_t)> &resolver);
    std::shared_ptr<Object> savedReference(std::string_view field) const;



    void setLocalBoolean(int index, bool value);
    void setLocalNumber(int index, int value);

    // END Local variables

    // Scripts

    const std::string &getOnHeartbeat() const { return _onHeartbeat; }
    const std::string &getOnUserDefined() const { return _onUserDefined; }

    /**
     * Drop this object's OnHeartbeat script, leaving its other event scripts
     * alone. Area heartbeat dispatch skips objects without one, so the object
     * stops receiving heartbeats. Used by the KotOR II RemoveHeartbeat routine
     * once a heartbeat script has done its one-off work.
     */
    void clearOnHeartbeat() { _onHeartbeat.clear(); }

    // END Scripts

protected:
    friend class Game;
    friend class ModuleSnapshotBuilder;
    friend class TestGameModule;

    // Add one privately constructed Item to a candidate owned graph using the
    // same stacking rules as runtime inventory insertion. Authoritative saved
    // identities can be preserved as distinct records during restoration.
    std::shared_ptr<Item> appendOwnedItemCandidate(
        std::vector<std::shared_ptr<Item>> &items,
        const std::shared_ptr<Item> &item,
        bool preserveSerializedIdentities);
    /** Atomically replace canonical effect state and run exact lifecycle hooks. */
    void replaceEffectState(std::deque<EffectInstance> replacement) noexcept;
    struct DelayedAction {
        std::shared_ptr<Action> action;
        std::unique_ptr<Timer> timer;
    };

    uint32_t _id;
    RuntimeState _runtimeState {RuntimeState::Constructing};
    uint64_t _runtimeIncarnation {0};
    ObjectType _type;
    std::string _sceneName;
    Game &_game;
    ServicesView &_services;

    // Serializable
    std::string _tag;
    std::string _blueprintResRef;
    std::string _conversation;
    std::string _onHeartbeat;
    std::string _onUserDefined;
    bool _minOneHP {false};
    bool _plot {false};
    bool _commandable {true};
    bool _interruptable {false};
    int16_t _hitPoints {0};
    int16_t _maxHitPoints {0};
    int32_t _currentHitPoints {0}; // Writes use 32 bits; getters and GFF read a signed word.
    glm::vec3 _position {0.0f};
    glm::quat _orientation {1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<std::shared_ptr<Item>> _items; // FIXME: deserialize
    // END Serializable

    std::string _name;
    SpellType _spellCast {SpellType::All};
    SpellCastContext _spellCastContext;
    uint32_t _effectSpellId {0xffffffffu}; // Only while running a spell-impact script.
    bool _dead {false};
    bool _destroyable {true};
    bool _raiseable {false};
    bool _selectableWhenDead {false};
    bool _isInConversation {false};
    glm::mat4 _transform {1.0f};
    bool _visible {true};
    // Non-owning; Area detachment/destruction clears this before releasing ownership.
    Area *_spatialArea {nullptr};
    Room *_room {nullptr};
    std::deque<EffectInstance> _effects;
    uint64_t _nextEffectApplicationOrder {1};
    bool _clearingEffects {false};
    std::vector<uint64_t> _removingEffectApplications;
    bool _open {false};
    bool _stunt {false};
    std::string _activeAnimName;
    std::string _feedbackText;
    float _feedbackTextRemaining {0.0f};

    std::shared_ptr<scene::SceneNode> _sceneNode;

    int _itemIndex {0};
    int _effectIndex {0};

    // Actions

    OrdinaryActionQueue _actions;
    std::vector<DelayedAction> _delayed;
    std::weak_ptr<ActionQueueNode> _executingActionNode;

    void addOpaqueAction(SavedActionRecord record);

    // END Actions

    RuntimeObjectRef<Object> _lastHostileActor;
    RuntimeObjectRef<Object> _lastDamager;
    std::array<int, 15> _lastDamageAmounts;
    std::optional<uint32_t> _savedLastDamagerId;

    // Local variables
    std::map<std::string, uint32_t> _savedReferenceIds;
    std::map<std::string, RuntimeObjectRef<Object>> _savedReferences;
    std::vector<EffectInstance> _savedEffects;
    int _forceAlwaysUpdate {0};
    SavedActionQueue _savedActionQueue;
    std::vector<SavedScheduledAction> _savedScheduledActions;
    std::vector<bool> _savedScheduledReferencesBound;
    float _savedScheduledTime {0.0f};
    SerializedIdentityContext _savedRuntimeIdentityContext;
    std::vector<bool> _savedEffectReferencesBound;
    std::vector<bool> _savedActionReferencesBound;
    bool _savedRuntimeParsed {false};
    bool _savedRuntimePublished {false};
    std::optional<SaveRecordProvenance> _saveRecordProvenance;


    std::map<int, bool> _localBooleans;
    std::map<int, int> _localNumbers;

    // END Local variables

    Object(uint32_t id, ObjectType type, std::string sceneName,
           Game &game, ServicesView &services);

    virtual void updateTransform();
    virtual bool canExecuteActions() const { return true; }

    void deserializeOwnedItems(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext,
        SaveRecordOriginKind originKind,
        bool forceDropable = false,
        std::string originOwner = {});

    // Actions

    void updateActions(float dt);
    void removeCompletedActions();
    void updateDelayedActions(float dt);

    void executeActions(float dt);

    // END Actions

    // Effects

    bool admitEffect(EffectInstance effect);
    void updateEffects(float dt);
    virtual void onEffectsCleared() {}
    virtual void onEffectsRestored() {}

    int applyDamageToHitPoints(int amount, int currentHitPoints);

    // END Effects
};

} // namespace game

} // namespace reone
