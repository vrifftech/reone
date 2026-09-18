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

#include "reone/game/attackhistory.h"

#include "reone/audio/clip.h"
#include "reone/audio/source.h"
#include "reone/graphics/lipanimation.h"
#include "reone/resource/format/2dareader.h"
#include "reone/resource/format/gffreader.h"
#include "reone/resource/parser/gff/git.h"
#include "reone/resource/strings.h"
#include "reone/resource/types.h"
#include "reone/scene/animeventlistener.h"
#include "reone/scene/node/model.h"
#include "reone/script/types.h"
#include "reone/system/timer.h"

#include "../d20/attributes.h"
#include "../forceresistancerules.h"
#include "../forcerules.h"
#include "../armorclassrules.h"
#include "../d20/itemattributes.h"
#include "../object.h"
#include "../menupresentation.h"
#include "../runtimeref.h"
#include "../pathfinder.h"

#include "item.h"

namespace reone {

namespace resource {
class Gff;
}

namespace game {

constexpr float kDefaultAttackRange = 2.0f;

class DamagePacket;
class Spell;
class ModuleSnapshotBuilder;
struct AttackBonusBreakdown;
struct DefenseBreakdown;
struct DamageBreakdown;
struct PhysicalDamageBonus;
struct DamageResolution;
struct SavingThrowBreakdown {
    int base {0};
    int modifier {0};
    int total() const { return base + modifier; }
};

class Creature : public Object, public scene::IAnimationEventListener {
public:
    std::string getOnSpellCastAt() const override { return _onSpellAt; }
    enum class ModelType {
        Creature,
        Droid,
        Character
    };

    enum class MovementType {
        None,
        Walk,
        Run
    };

    struct BodyBag {
        std::string name;
        int appearance {0}; /**< index into placeables.2da */
        bool corpse {false};
    };

    struct Perception {
        float sightRange {0.0f};
        float hearingRange {0.0f};
        std::map<uint32_t, RuntimeObjectRef<Object>> seen;
        std::map<uint32_t, RuntimeObjectRef<Object>> heard;

        bool sees(uint32_t id) const {
            auto found = seen.find(id);
            return found != seen.end() && found->second.resolve() != nullptr;
        }
        bool hears(uint32_t id) const {
            auto found = heard.find(id);
            return found != heard.end() && found->second.resolve() != nullptr;
        }
    };

    struct AutoBalanceContext {
        uint8_t multiplierSet {0};
        uint8_t playerLevelAtSpawn {0};
    };

    struct CombatState {
        bool active {false};
        uint8_t activationType {0};
        bool shouldDeactivate {false};
        bool debilitated {false};
        RuntimeObjectRef<Object> attackTarget;
        RuntimeObjectRef<Object> attemptedAttackTarget;
        RuntimeObjectRef<Object> attemptedSpellTarget;
        ActionType attackAction {ActionType::QueueEmpty};
        FeatType combatFeat {FeatType::Invalid};
        Timer deactivationTimer;
    };

    Creature(
        uint32_t id,
        std::string sceneName,
        Game &game,
        ServicesView &services);

    static bool classof(const Object *from) {
        return from->type() == ObjectType::Creature;
    }

    void loadFromBlueprint(const std::string &resRef);
    void loadAppearance();
    void applyDisguiseAppearance(int appearance);
    void removeDisguiseAppearance();

    void deserialize(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);

    /**
     * Invalidate state that only exists while this retained creature inhabits
     * one Area. The owning Area supplies its still-live Pathfinder so path
     * handles are released before that owner is destroyed.
     */
    void retireAreaRuntime(
        Pathfinder &pathfinder,
        const std::set<const Object *> &retainedObjects);

    void update(float dt) override;

    void clearAllActions(bool force = false) override;
    void damage(
        int amount,
        const std::shared_ptr<Object> &damager) override;

    void applyDamageEffect(
        int amount,
        const std::shared_ptr<Object> &damager) override;

    void giveXP(int amount);
    void setXP(int xp);

    void playSound(resource::SoundSetEntry entry, bool positional = true);

    void startTalking(const std::shared_ptr<graphics::LipAnimation> &animation);
    void stopTalking();

    bool isSelectable() const override;
    int effectState() const { return _effectState; }
    int effectAIStateMask() const { return _effectAIStateMask; }
    int effectAmbientState() const { return _effectAmbientState; }
    EffectId activeStateRootId() const { return _activeStateRootId; }
    void onStateRootApplied(const EffectInstance &pending);
    bool isHasted() const { return _hasted; }
    bool isSlowed() const { return _slowed; }
    void setHasted(bool value) { _hasted = value; }
    void setSlowed(bool value) { _slowed = value; }
    void rebuildStateEffects(const EffectInstance *pending = nullptr, uint64_t removingOrder = 0);
    void beginStateImmobilization();
    void restoreMovementAfterState();
    void setInternalStateEffect(int state, int ambientState, EffectId id);
    void clearInternalStateEffect(EffectId id);
    void onInternalStateRemoved(EffectId id);
    void recomputeAIStateEffects(int pendingMask = 0xffff, uint64_t removingOrder = 0);
    struct EffectStackCounts { int positive {0}; int negative {0}; };
    EffectStackCounts effectStackCounts() const;
    void addEffectIcon(int icon);
    void removeEffectIcon(int icon);
    void resolveDamageShields(Creature &attacker);

    void damageForcePoints(int amount);
    void healForcePoints(int amount);
    int maxForcePoints() const;
    void setBodyFuel(bool active) { _bodyFuel = active; }
    void setThrowParryBlocked(bool blocked) { _throwParryBlocked = blocked; }
    bool throwParryBlocked() const { return _throwParryBlocked; }
    void refreshBodyFuel();
    int adjustedSpellForcePointCost(const Spell &spell) const;
    bool canPaySpellForcePointCost(const Spell &spell) const;
    bool commitSpellForcePointCost(const Spell &spell, int &cost);
    uint32_t forceItemMask() const;
    int forceBodyLevel() const;
    int spellCasterLevel(const Spell &spell, bool itemOrCheat = false) const;
    bool readySpellLikeAbility(SpellType spell, int &casterLevel) const;
    bool consumeSpellLikeAbility(SpellType spell, int &casterLevel);
    void addTemporaryHitPoints(int amount, bool restoring = false);
    void removeTemporaryHitPoints(int amount);
    void addTemporaryForcePoints(int amount, bool restoring = false);
    void removeTemporaryForcePoints(int amount);
    int bonusForcePoints() const { return _bonusForcePoints; }
    void setBonusForcePoints(int amount);
    int currentHitPoints() const override {
        return narrowSignedResource(static_cast<int64_t>(_currentHitPoints) + _temporaryHitPoints);
    }

    void multiplyMovementRate(float multiplier);
    void recomputeMovementRate(uint64_t removingId = 0);
    float movementRate(bool applyMobility = false) const;
    bool isRunLimited() const;
    void setRunLimited(bool limited) { _runLimited = limited; }
    MovementType movementType() const { return _movementType; }

    bool canCastSpells() const { return canExecuteActions(); }
    bool canMove() const { return canExecuteActions() && (_effectAIStateMask & 0x02) != 0; }
    bool canAttack() const { return canExecuteActions() && (_effectAIStateMask & 0x84) == 0x84; }
    bool isMovementRestricted() const { return _movementRestricted || !canMove(); }
    bool movementLockedByAction() const { return _movementRestricted; }
    bool permitsAction(const Action &action) const;
    bool isLevelUpPending() const;

    glm::vec3 getSelectablePosition() const override;
    float getAttackRange() const;
    int getNeededXP() const;

    Gender gender() const { return _gender; }
    ModelType modelType() const { return _modelType; }
    int appearance() const { return _appearance; }
    CreaturePresentation presentation() const;
    void setPresentation(const CreaturePresentation &presentation);
    uint16_t portraitId() const { return _portraitId; }
    std::shared_ptr<graphics::Texture> portrait() const { return _portrait; }
    float walkSpeed() const { return _walkSpeed * movementRate(true); }
    float runSpeed() const { return _runSpeed * movementRate(true); }
    float creaturePersonalSpace() const { return _creaturePersonalSpace; }
    CreatureSize size() const { return _size; }
    CreatureAttributes &attributes() { return _attributes; }
    const CreatureAttributes &attributes() const { return _attributes; }
    ItemAttributes &itemAttributes() { return _itemAttributes; }
    const ItemAttributes &itemAttributes() const { return _itemAttributes; }
    Faction faction() const { return _faction; }
    int xp() const { return _xp; }
    float challengeRating() const { return _challengeRating; }
    int getReputationToward(const Creature &target) const;
    void onDestroyabilityChanged();
    bool applyDeathEffect(const std::shared_ptr<Object> &damager, bool noFadeAway,
                          const EffectInstance *operation = nullptr);
    bool applyResurrectionEffect(int hpPercent);
    void applyHealingEffect(int amount, const std::shared_ptr<Object> &creator, bool quiet);
    void removeEffectsOnDeath();

    void addArmorClassEffect(const EffectInstance &effect);
    void removeArmorClassEffect(const EffectInstance &effect);
    void updateArmorClassEffectCursor();
    void clearArmorClassEffectCache() { _armorClassCache = {}; _armorClassCursor = {}; }

    Alignment alignment() const;
    RacialType racialType() const { return _race; }
    Subrace subrace() const { return _subrace; }
    NPCAIStyle aiStyle() const { return _aiStyle; }
    int walkmeshMaterial() const { return _walkmeshMaterial; }
    bool isPC() const { return _isPC; }
    int assignedPuppet() const { return _assignedPuppet; }
    bool isPuppet() const { return _puppet; }
    CombatForm currentForm() const { return _currentForm; }
    CombatStance combatStance() const { return _combatStance; }
    const AutoBalanceContext &autoBalanceContext() const {
        return _autoBalanceContext;
    }

    void setGender(Gender gender) { _gender = gender; }
    void setAppearance(int appearance) { _appearance = appearance; }
    void setMovementType(MovementType type);
    void setFaction(Faction faction) { _faction = faction; }
    /** Set the serialized base maximum and preserve the existing damage. */
    void setMaxHitPoints(int baseHitPoints) override;
    void setCurrentHitPoints(int hitPoints) override;

    /**
     * Recalculate permanent vitality after attributes, levels or feats change,
     * preserving the exact amount of damage already sustained.
     */
    void recalculatePermanentVitality();

    /** Initialize a newly generated creature at full derived vitality. */
    void initializeGeneratedVitality();

    /** Current vitality translated back to the serialized base-HP axis. */
    int serializedCurrentHitPoints() const;
    void setMovementRestricted(bool restricted) { _movementRestricted = restricted; }
    void setImmortal(bool immortal) { _immortal = immortal; }
    void setAIStyle(NPCAIStyle style) { _aiStyle = style; }
    void setAssignedPuppet(int puppet) { _assignedPuppet = puppet; }
    void setPuppet(bool puppet) { _puppet = puppet; }
    void setWalkmeshMaterial(int material) { _walkmeshMaterial = material; }
    void setCurrentForm(CombatForm form) { _currentForm = form; }
    bool isStealthed() const { return _stealthMode; }
    void setStealthMode(bool enabled);
    void beginSpellActivity(int spellId, bool itemCast);
    void updateMindTrickPerception(const Creature &target, bool heard, bool seen);
    void resolveInitiative() { _initiative = !isPC(); }
    bool hasInitiative() const { return _initiative; }
    void setExcitedState(uint8_t row);
    bool isExcited() const { return _excitedTime > 0.0f; }
    void setAutoBalanceContext(AutoBalanceContext context) {
        _autoBalanceContext = context;
        _autoBalancePlayerLevelAtSpawnSet = true;
    }

    // Animation

    void playAnimation(AnimationType type, scene::AnimationProperties properties = scene::AnimationProperties()) override;

    void playAnimation(CombatAnimation anim, CreatureWieldType wield, int variant = 1);
    void playAnimation(const std::string &name, scene::AnimationProperties properties = scene::AnimationProperties());
    bool playAnimation(const std::shared_ptr<graphics::Animation> &anim, scene::AnimationProperties properties = scene::AnimationProperties());
    // Holds an externally sourced animation until resumeStateDrivenAnimation is called.
    bool playExternalAnimation(const std::shared_ptr<graphics::Animation> &anim, scene::AnimationProperties properties = scene::AnimationProperties());
    void resumeStateDrivenAnimation();

    /**
     * Play an animation as a layer over whatever the creature is already doing,
     * including while it is walking or running. Unlike the other playAnimation
     * overloads this neither waits for the creature to stand still nor takes
     * over its state-driven animation, so locomotion carries on underneath and
     * the layer disappears on its own once it has run.
     */
    void playOverlayAnimation(AnimationType type);
    int selectMeleeAttackVariant(bool cinematic);

    void updateModelAnimation();

    // END Animation

    // Equipment

    bool equip(const std::string &resRef);
    /** Validate an equipment commit without changing ownership. */
    bool canEquip(int slot, const std::shared_ptr<Item> &item) const;
    bool equip(int slot, const std::shared_ptr<Item> &item);
    /** Validate an equipment replacement without changing ownership. */
    bool canReplaceEquipment(
        int slot,
        const std::shared_ptr<Item> &item,
        const Object &displacedReceiver) const;
    bool replaceEquipment(
        int slot,
        const std::shared_ptr<Item> &item,
        Object &displacedReceiver);
    /** Remove the equipment ownership edge for immediate transfer or retirement. */
    std::shared_ptr<Item> takeEquippedItem(const std::shared_ptr<Item> &item);
    /** Unequip an Item directly into an explicit owning inventory. */
    bool moveEquippedItemTo(
        const std::shared_ptr<Item> &item,
        Object &receiver);

    bool isSlotEquipped(int slot) const;

    std::shared_ptr<Item> getEquippedItem(int slot) const;
    CreatureWieldType getWieldType() const;

    const std::map<int, std::shared_ptr<Item>> &equipment() const { return _equipment; }
    std::vector<std::shared_ptr<Object>> ownedRuntimeObjects() const override;

    // END Equipment

    // Pathfinding
    bool navigateTo(const glm::vec3 &dest, bool run, float distance, float dt);
    void clearPath();
    void advanceOnPath(const glm::vec3 &dest, const glm::vec3 &dir, bool run, float distance, float dt);
    glm::vec3 computeSteeringForce(const Uniwalk &uni, const glm::vec3 &next, float dt);
    // END Pathfinding

    // Blocking doors

    /**
     * Remember the door that obstructed the last attempted step. Written by the
     * collision layer for every mover, including the directly controlled player.
     *
     * This lives only to carry the obstruction from the collision test to the
     * blocked event raised after the step. It is not what scripts read:
     * GetBlockingDoor answers from the argument captured when the event was
     * raised, so it stays fixed for that run while this keeps changing.
     */
    void setBlockingDoor(uint32_t doorId) { _blockingDoorId = doorId; }

    void clearBlockingDoor() { _blockingDoorId = script::kObjectInvalid; }

    uint32_t blockingDoorId() const { return _blockingDoorId; }

    /**
     * Edge-trigger ScriptOnBlocked for the door currently obstructing this
     * creature. Called by navigation after each attempted step, so it only
     * applies to AI, script and action driven movement. A continuous
     * obstruction by the same door reports once; an unobstructed step re-arms.
     */
    void dispatchBlockedEvent();

    // END Blocking doors

    // Perception

    void onObjectSeen(const std::shared_ptr<Object> &object);
    void onObjectVanished(const std::shared_ptr<Object> &object);
    void onObjectHeard(const std::shared_ptr<Object> &object);
    void onObjectInaudible(const std::shared_ptr<Object> &object);

    void setObjectSeen(const std::shared_ptr<Object> &object, bool seen);
    void setObjectHeard(const std::shared_ptr<Object> &object, bool heard);
    void runOnNotice(const Object &object, bool heard, bool seen);
    void refreshVisibilityPerception();

    static constexpr uint8_t kSeeInvisibleCounter = 0x01;
    static constexpr uint8_t kUltravisionCounter = 0x02;
    static constexpr uint8_t kTrueSeeingCounter = 0x04;

    uint8_t visibilityCounterBits() const { return _visibilityCounterBits; }
    void setVisibilityCounter(uint8_t bit);
    void restoreBlindnessCounter(int mask, uint64_t removedApplication);
    void restoreVisibilityCounter(
        EffectType type,
        uint8_t bit,
        EffectId removedEffect,
        bool trueSeeingRemovalQuirk = false);
    bool hasVisibilityCounter(uint8_t bits) const;

    const Perception &perception() const { return _perception; }

    // END Perception

    // Combat

    void activateCombat(uint8_t activationType = 1);
    uint8_t combatActivationType() const { return _combatState.activationType; }
    bool clientCombatMode() const { return _clientCombatMode; }
    void setClientCombatMode(bool active);
    void broadcastCombatState(uint32_t opponent);
    void removeCombatInvisibilityEffects();
    void removeMindTrickEffects();
    uint32_t lastWeaponUsed() const { return _lastWeaponUsed; }
    void deactivateCombat(float delay);

    bool isInCombat() const { return _combatState.active; }
    bool isDebilitated() const;
    bool isTemporarilyDead() const;
    EffectId activePoisonEffectId() const { return _activePoisonEffectId; }
    void setActivePoisonEffectId(EffectId id) { _activePoisonEffectId = id; }
    void clearActivePoisonEffectId(EffectId id) {
        if (_activePoisonEffectId == id) _activePoisonEffectId = kUnassignedEffectId;
    }
    bool isPartyMember() const;
    bool isInvisibleTo(const Creature &observer) const;
    void clearHostileActionsAgainst(const Object &object);
    bool isEffectLinkImmune(const Effect &effect) const;
    bool isTwoWeaponFighting() const;
    std::shared_ptr<Item> getOffhandAttackWeapon() const;

    bool stateControlsActions() const;

    int forcePoints() const { return _forcePoints; }
    int currentForce() const { return _currentForce + _temporaryForcePoints; }
    int currentForceWithoutTemporary() const { return _currentForce; }
    void regenerateForcePoints(int amount);

    uint32_t getAttemptedAttackTarget() const {
        auto target = _combatState.attemptedAttackTarget.resolve();
        return target ? target->id() : script::kObjectInvalid;
    }
    std::shared_ptr<Object> getAttackTarget() const {
        return _combatState.attackTarget.resolve();
    }
    uint32_t getLastHostileTarget() const {
        auto target = _lastHostileTarget.resolve();
        return target ? target->id() : script::kObjectInvalid;
    }
    ActionType getLastAttackAction() const { return _lastAttackAction; }
    FeatType getLastCombatFeat() const { return _lastCombatFeat; }
    AttackResultType getLastAttackResult() const { return _lastAttackResult; }
    int modifiedAttacks() const { return _modifiedAttacks; }
    bool hasAssuredHit() const { return _assuredHit; }
    AttackBonusBreakdown getAttackBonusBreakdown(
        const Creature *target,
        const Item *weapon,
        bool offHand) const;
    int getAttackBonus(bool offHand = false) const;
    bool hasEffectImmunity(
        ImmunityType immunityType,
        const Creature *creator = nullptr) const;
    int getAbilityEffectModifier(Ability ability) const;
    int getEffectiveAbilityScore(Ability ability) const;
    int getEffectiveAbilityModifier(Ability ability) const;
    bool hasEffectiveFeat(FeatType feat) const;
    DefenseBreakdown getDefenseBreakdown(const Creature *attacker, int damageFlags) const;
    int getDefense(const Creature *attacker, int damageFlags) const;
    int getDefense() const;
    // Ordinary ranged defense consumes the same permission as active saber throws.
    bool canParryRangedWeapon(const Creature &shooter, int damageFlags, bool &canReturn) const;
    AttackResultType resolveRangedDefense(const Creature &shooter, int damageFlags, int attackTotal) const;
    AttackResultType resolveRangedMiss(const Creature &shooter, const Item &weapon) const;
    SavingThrowBreakdown getSavingThrowBreakdown(
        SavingThrow save, SavingThrowType type = SavingThrowType::All,
        const Object *versus = nullptr) const;
    int getSavingThrow(SavingThrow save) const;
    SavingThrowResult rollSavingThrow(
        SavingThrow save, int difficultyClass,
        SavingThrowType type = SavingThrowType::All,
        const Object *versus = nullptr) const;
    SavingThrowResult getSavingThrowResult(
        int total, int difficultyClass, SavingThrowType type,
        const Object *versus = nullptr) const;
    PhysicalDamageBonus getPhysicalDamageBonus(
        const Item *weapon,
        bool offHand) const;
    int getPhysicalDamageAutoBalanceFactor() const;
    int getSpellLevel(bool applyNegativeLevels = true) const;
    ForceResistanceState &forceResistance() { return _forceResistance; }
    const ForceResistanceState &forceResistance() const { return _forceResistance; }
    int getMassiveCriticalDamage(const Item *weapon, bool criticalHit) const;
    void getDamageResistanceFeatBonuses(
        int damage,
        DamageResolution &resolution) const;
    DamagePower calculateDamagePower(
        const Creature *target,
        const Item *weapon,
        bool offHand) const;
    void addPhysicalDamageModifiers(
        DamagePacket &damage,
        DamageBreakdown &breakdown,
        const Creature *target,
        const Item *weapon,
        bool offHand,
        int criticalMultiplier) const;
    void getMainHandDamage(int &min, int &max) const;
    void getOffhandDamage(int &min, int &max) const;

    void setAttemptedSpellTarget(uint32_t id);
    std::shared_ptr<Object> attemptedSpellTarget() const { return _combatState.attemptedSpellTarget.resolve(); }
    float maxCleaveRange(const Creature *target) const;
    std::string getWeaponModelName(int slot) const;
    void setAttemptedAttackTarget(uint32_t target);
    void beginCombatAttack(Object &target, FeatType feat);
    void finishCombatRound();
    void cancelCombat(int runEndRound = 0);
    void recordQueuedAttack(Creature &target);
    uint32_t getGoingToBeAttackedBy() const { return _incomingAttacker.id; }
    uint32_t getFirstAttacker();
    uint32_t getNextAttacker() { return _attackerList.next(); }
    int getLastAttackType() const { return _receivedAttack.scriptType(isInCombat()); }
    int getLastAttackMode() const { return _receivedAttack.scriptMode(isInCombat()); }
    void receiveAttackEvent(const AttackHistory *history, uint32_t attackerId,
                            const AttackEventFields *fields = nullptr);
    void clearCurrentAttackTarget() { _combatState.attackTarget.reset(); }
    void setLastAttackResult(AttackResultType result) { _lastAttackResult = result; }
    void adjustModifiedAttacks(int amount);
    bool applyAssuredHit();
    void removeAssuredHit() { _assuredHit = false; }
    bool applyAssuredDeflection(int returnDamage);
    void removeAssuredDeflection() { _assuredDeflection = _assuredReturn = false; }

    // END Combat

    // Gold

    void giveGold(int amount);
    void takeGold(int amount);

    int gold() const { return _gold; }

    // END Gold

    // Scripts

    void runSpawnScript();
    void runBlockedScript(uint32_t blockingDoorId);
    void runEndRoundScript();
    void runDialogueScript(uint32_t speakerId, int32_t listenNumber);
    void runAttackedScript(uint32_t attackerId);

    bool spawnScriptFired() const { return _spawnScriptFired; }

    void setOnHeartbeat(std::string onHeartbeat) { _onHeartbeat = onHeartbeat; }
    void setOnSpawn(std::string onSpawn) { _onSpawn = onSpawn; }
    void setOnDeath(std::string onDeath) { _onDeath = onDeath; }
    void setOnNotice(std::string onNotice) { _onNotice = onNotice; }
    void setOnEndRound(std::string onEndRound) { _onEndRound = onEndRound; }
    void setOnSpellAt(std::string onSpellAt) { _onSpellAt = onSpellAt; }
    void setOnAttacked(std::string onAttacked) { _onAttacked = onAttacked; }
    void setOnDamaged(std::string onDamaged) { _onDamaged = onDamaged; }
    void setOnDisturbed(std::string onDisturbed) { _onDisturbed = onDisturbed; }
    void setOnEndDialogue(std::string onEndDialogue) { _onEndDialogue = onEndDialogue; }
    void setOnBlocked(std::string onBlocked) { _onBlocked = onBlocked; }
    void setOnDialogue(std::string onDialogue) { _onDialogue = onDialogue; }

    // END Scripts

    // IAnimationEventListener

    void onEventSignalled(const std::string &name) override;

    // END IAnimationEventListener

    // Listeners

    bool isListening() { return _isListening; }
    void setIsListening(bool value) { _isListening = value; }

    // END Listeners

    int getSpellSaveDC(int spellId) const;
    void playForceResistedAnimation();
    void beginForcePush(const glm::vec3 &destination, float facing);
    void endForcePush();
    bool isForcePushed() const { return _forcePushDestination.has_value(); }

    int furyDamageBonus() const { return _furyDamageBonus; }
    int furySpellState() const { return _furySpellState; }
    void applyFuryState(int spellId);
    void clearFuryState();
    void incrementFuryDamageBonus();

protected:
    bool canExecuteActions() const override;

private:
    void updateForcePush(float dt);
    void updateStateHeartbeat(float dt);
    Timer _stateSupportTimer;
    std::optional<glm::vec3> _forcePushDestination;
    float _forcePushFacing {0.0f};
    int getSavingThrowBase(SavingThrow save) const;
    int getSavingThrowEffectBonus(SavingThrow save, SavingThrowType type,
                                 const Object *versus) const;

    friend class ModuleSnapshotBuilder;
    friend class TestGameModule;
    // Serializable
    RacialType _race {RacialType::Unknown};
    Subrace _subrace {Subrace::None};
    uint32_t _appearance {0};
    uint32_t _appearanceBeforeDisguise {0};
    bool _disguised {false};
    Gender _gender {Gender::Male};
    uint16_t _portraitId {0};
    bool _isPC {false};
    int32_t _assignedPuppet {-1};
    bool _puppet {false};
    Faction _faction {Faction::Invalid};
    bool _disarmable {false};
    bool _noPermDeath {false};
    std::optional<std::string> _livingName;
    EffectId _activePoisonEffectId {kUnassignedEffectId};
    bool _notReorienting {false};
    uint8_t _bodyVariation {0};
    std::optional<CreaturePresentation> _presentation;
    uint8_t _textureVar {0};
    bool _partyInteract {false};
    int32_t _walkRate {0};
    uint8_t _naturalAC {0};
    ArmorClassCache _armorClassCache;
    ArmorClassCursor _armorClassCursor;
    int16_t _forcePoints {0};
    // LvlStatList/LvlStatForce: the base grant for each individual level.
    std::vector<uint8_t> _levelForcePoints;
    int16_t _currentForce {0};
    int32_t _temporaryHitPoints {0};
    int32_t _temporaryForcePoints {0};
    int32_t _bonusForcePoints {0};
    int8_t _furyDamageBonus {-1};
    int32_t _furySpellState {0};
    bool _temporaryHitPointsRestored {false};
    bool _temporaryForcePointsRestored {false};
    int16_t _refBonus {0};
    int16_t _willBonus {0};
    int16_t _fortBonus {0};
    uint8_t _goodEvil {0};
    float _challengeRating {0};
    uint32_t _xp {0};
    CombatForm _currentForm {CombatForm::None};
    AutoBalanceContext _autoBalanceContext;
    ForceResistanceState _forceResistance;
    bool _autoBalancePlayerLevelAtSpawnSet {false};

    std::string _onNotice;
    std::string _onSpellAt;
    std::string _onAttacked;
    std::string _onDamaged;
    std::string _onDisturbed;
    std::string _onEndRound;
    std::string _onEndDialogue;
    std::string _onDialogue;
    std::string _onSpawn;
    std::string _onDeath;
    std::string _onBlocked;

    // CreatnScrptFird tracks whether this creature has run its creation script.
    // It persists across area attachments and saves.
    bool _spawnScriptFired {false};

    // Door currently obstructing this creature, and the door the blocked event
    // was last reported for. Object ids rather than pointers, so a door that is
    // destroyed while remembered simply resolves to no object.
    uint32_t _blockingDoorId {script::kObjectInvalid};
    uint32_t _blockedEventDoorId {script::kObjectInvalid};

    resource::LocString _firstName;
    resource::LocString _lastName;

    uint16_t _soundSetId {0xFFFF};
    uint8_t _bodyBagId {0xFF};
    uint8_t _perceptionId {0xFF};

    CreatureAttributes _attributes;
    struct SpellLikeAbility {
        uint16_t spell {0};
        uint8_t flags {0};
        uint8_t casterLevel {0};
    };
    std::vector<SpellLikeAbility> _spellLikeAbilities;
    std::map<int, std::shared_ptr<Item>> _equipment;
    // END Serializable

    ModelType _modelType {ModelType::Creature};
    std::shared_ptr<graphics::Texture> _portrait;

    // Current path that the creature is following, its velocity and position at
    // the previous frame.
    std::optional<Path> _path;
    glm::vec3 _pathVelocity;
    glm::vec3 _previousPosition;
    // When there is no progress on the path, apply _stuckForce to steer the
    // creature in a random direction until the timer runs out.
    Timer _stuckTimer;
    glm::vec3 _stuckForce;

    float _walkSpeed {0.0f};
    float _runSpeed {0.0f};
    float _creaturePersonalSpace {0.6f};
    CreatureSize _size {CreatureSize::Invalid};
    MovementType _movementType {MovementType::None};
    bool _talking {false};

    ItemAttributes _itemAttributes;

    bool _movementRestricted {false};
    float _movementRate {1.0f};
    bool _hasted {false};
    bool _slowed {false};
    bool _runLimited {false};
    std::optional<MovementType> _movementTypeBeforeStateImmobilization;
    int _effectState {0};
    int _effectAmbientState {0};
    int _effectAIStateMask {0xffff};
    bool _bodyFuel {false};
    bool _throwParryBlocked {false};
    bool projectileDefenseEligible(const Creature &shooter, int damageFlags,
        const Item *weapon, bool allowShield, bool &shieldHit, bool &canReturn) const;
    EffectId _internalStateEffectId {0};
    EffectId _activeStateRootId {0};
    std::map<int, int> _effectIconCounts;
    CombatState _combatState;
    bool _clientCombatMode {false};
    uint32_t _lastWeaponUsed {script::kObjectInvalid};
    SavedObjectReference _incomingAttacker;
    AttackerList _attackerList;
    AttackHistory _receivedAttack;
    CombatStance _combatStance {CombatStance::None};
    bool _stealthMode {false};
    bool _initiative {false};
    float _excitedTime {0.0f};
    int _lastMeleeAttackVariant {-1};
    RuntimeObjectRef<Object> _lastHostileTarget;
    ActionType _lastAttackAction {ActionType::QueueEmpty};
    FeatType _lastCombatFeat {FeatType::Invalid};
    AttackResultType _lastAttackResult {AttackResultType::Invalid};
    int _modifiedAttacks {0};
    bool _assuredHit {false};
    bool _assuredDeflection {false};
    bool _assuredReturn {false};
    bool _immortal {false};
    std::shared_ptr<resource::SoundSet> _soundSet;
    BodyBag _bodyBag;
    Perception _perception;
    uint8_t _visibilityCounterBits {0};
    bool _trueSeeingUltravisionQuirk {false};
    NPCAIStyle _aiStyle {NPCAIStyle::DefaultAttack};

    uint32_t _footstepType {0};
    int _walkmeshMaterial {-1};
    int _gold {0}; /**< aka credits */
    std::string _envmap;
    bool _isListening {false};

    std::shared_ptr<audio::AudioSource> _audioSourceVoice;
    std::shared_ptr<audio::AudioSource> _audioSourceFootstep;
    bool _lightsaberIdlePowerDownPending {false};
    Timer _lightsaberIdlePowerDownTimer;

    // Animation

    bool _animDirty {true};
    bool _animFireForget {false};
    std::shared_ptr<graphics::LipAnimation> _lipAnimation;

    // END Animation

    // Scripts

    // END Scripts

    void loadTransformFromGIT(const resource::generated::GIT_Creature_List &git);

    void onEffectsCleared() override;
    void onEffectsRestored() override;
    void updateModel();

    // Refresh appearance-derived state (model type, size, speeds, footstep, envmap,
    // portrait) for the current _appearance, without building a scene node.
    void loadAppearanceProperties();

    // Presentation-only equipment snapshots retain their visual-only override.
    // Live equipment uses the Disguise effect provider and restores the original
    // appearance when none remains. Updates _appearance only; callers rebuild the model.
    void updateDisguise();
    void updateEquipmentPresentation();
    void updateCombat(float dt);
    void setLightsabersPowered(bool powered, bool animate);
    void updateLightsaberSoundPositions();

    void runDeathScript();
    void runDamagedScript();

    ModelType parseModelType(const std::string &s) const;

    // Appearance

    std::shared_ptr<scene::ModelSceneNode> buildModel();
    void finalizeModel(scene::ModelSceneNode &body);

    std::string getBodyModelName() const;
    std::string getBodyTextureName() const;
    std::string getHeadModelName() const;
    std::string getMaskModelName() const;

    // END Appearance

    // Animation

    bool doPlayAnimation(bool fireForget, const std::function<void()> &callback);

    std::string getAnimationName(AnimationType anim) const override;
    std::string getAnimationName(CombatAnimation anim, CreatureWieldType wield, int variant) const;
    std::string getActiveAnimationName() const override;

    std::string getDeadAnimation() const;
    std::string getDieAnimation() const;
    std::string getHeadTalkAnimation() const;
    std::string getPauseAnimation() const;
    std::string getRunAnimation() const;
    std::string getTalkNormalAnimation() const;
    std::string getWalkAnimation() const;

    /**
     * @return creatureAnim if model type is creature, elseAnim otherwise
     */
    inline std::string getFirstIfCreatureModel(std::string creatureAnim, std::string elseAnim) const;

    bool getWeaponInfo(WeaponType &type, WeaponWield &wield) const;
    int getWeaponWieldNumber(WeaponWield wield) const;
    int getRelativeWeaponSize(const Item &weapon) const;
    int getTwoWeaponAttackPenalty(
        const Item *weapon,
        bool offHand,
        int *smallOffhandBonus = nullptr) const;
    int getDuelingBonus() const;
    void getWeaponDamage(const Item *weapon, int &min, int &max) const;

    // END Animation

    // Blueprint
    void deserializeAll(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    void deserializeName(const resource::Gff &gff);
    void deserializeSoundSet(const resource::Gff &gff);
    void deserializeBodyBag(const resource::Gff &gff);
    void deserializeAttributes(const resource::Gff &gff);
    void deserializeClass(const resource::Gff &gff);
    void deserializePerception(const resource::Gff &gff);
    void deserializeOwnedItemsAndEquipment(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    void appendEquippedItemEffects(
        std::deque<EffectInstance> &effects,
        int slot,
        const std::shared_ptr<Item> &item, bool onlyDeferredEffects = false) const;
    std::deque<EffectInstance> effectsWithoutEquippedSource(
        const Item *source) const;
    std::deque<EffectInstance> rebuildEquippedItemEffects(
        const std::map<int, std::shared_ptr<Item>> &equipment) const;
    int derivePermanentMaxHitPoints() const;
    void restoreSerializedVitality();
    void applyHitPointDamage(int amount, const std::shared_ptr<Object> &damager);
    int consumeTemporaryHitPoints(int amount);
    void updateDeathFromCurrentHitPoints();
    // END Blueprint
};

} // namespace game

} // namespace reone
