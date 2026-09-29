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

#include "reone/game/attack.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

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
#include "../forcerules.h"
#include "../d20/itemattributes.h"
#include "../object.h"
#include "../menupresentation.h"
#include "../runtimeref.h"
#include "../pathfinder.h"

#include "item.h"

namespace reone {

namespace resource {
class Gff;
class IGffs;
}

namespace game {

// AC effect categories and selectors are distinct from damage-packet flags.
struct ArmorClassEffectData {
    int category {0}; // dodge, natural, armour, shield, deflection
    int amount {0};
    int race {7};
    int lawChaos {0};
    int goodEvil {0};
    int selector {0x4007};
    bool decrease {false};

    bool validCategory() const { return category >= 0 && category < 5; }
    bool unqualified() const { return race == 7 && lawChaos == 0 && goodEvil == 0; }
    bool cached(bool tsl) const {
        if (!validCategory() || !unqualified()) return false;
        return !tsl || decrease || category != 0 || selector == 0x2007 || selector == 0x4007;
    }
    bool conditional(int attackerRace, int attackerAlignment, int damageFlags) const {
        // The skip test ignores lawChaos. PHYSICAL is an exact selector, not a bitmask
        // intersection; neither 0x2007 nor DAMAGE_TYPE_ALL is a wildcard.
        if (race == 7 && goodEvil == 0 && selector == 0x4007) return false;
        if ((race != 7 && race != attackerRace) ||
            (goodEvil != 0 && goodEvil != attackerAlignment)) return false;
        return selector == 0x4007 || (selector == 1 && damageFlags == 0) ||
               (selector == 2 && damageFlags == 1) || (selector == 4 && damageFlags == 2);
    }
};

constexpr int armorClassByte(std::int64_t value) {
    auto byte = static_cast<std::uint8_t>(value);
    return byte < 128 ? byte : static_cast<int>(byte) - 256;
}
constexpr int armorClassShort(std::int64_t value) {
    auto word = static_cast<std::uint16_t>(value);
    return word < 32768 ? word : static_cast<int>(word) - 65536;
}

// The cursor changes only when an AC increase exists and retains its previous
// offset when the last increase disappears. Queries and removals both start
// here, including decrease-only scans.
struct ArmorClassCursor {
    std::uint16_t position {0};

    template<class ReadType>
    void update(std::size_t count, ReadType type) {
        for (std::size_t i = 0; i < count; ++i) {
            if (type(i) == 48) {
                position = static_cast<std::uint16_t>(i);
                return;
            }
        }
    }
};

template<class ReadType, class ReadEffect, class IsRemoved>
int remainingArmorClassMaximum(const ArmorClassCursor &cursor, std::size_t count,
                             const ArmorClassEffectData &removed, ReadType type,
                             ReadEffect effect, IsRemoved isRemoved) {
    int maximum = 0;
    for (std::size_t i = cursor.position; i < count; ++i) {
        // In particular, decrease removal does not skip an increase prefix.
        if (type(i) != (removed.decrease ? 49 : 48)) break;
        if (isRemoved(i)) continue;
        const auto candidate = effect(i);
        // Decrease removal checks race only; increase removal also checks the
        // two alignment fields. Neither recomputation checks the selector.
        if (candidate.category == removed.category && candidate.race == 7 &&
            (removed.decrease || candidate.unqualified()))
            maximum = std::max(maximum, candidate.amount);
    }
    return maximum;
}

struct ArmorClassCache {
    // Cache values use signed-byte arithmetic. Preserve update history: computing
    // a fresh maximum at query time would lose overflow and removal effects.
    std::array<int, 5> increases {};
    std::array<int, 5> decreases {};

    void add(const ArmorClassEffectData &effect, bool tsl) {
        if (!effect.cached(tsl)) return;
        auto &value = (effect.decrease ? decreases : increases)[effect.category];
        if (effect.category == 0) value = armorClassByte(std::int64_t(value) + effect.amount);
        else if (effect.amount > value) value = armorClassByte(effect.amount);
    }
    void remove(const ArmorClassEffectData &effect, bool tsl, int remainingMaximum) {
        if (!effect.cached(tsl)) return;
        auto &value = (effect.decrease ? decreases : increases)[effect.category];
        if (effect.category == 0) value = armorClassByte(std::int64_t(value) - effect.amount);
        else value = armorClassByte(remainingMaximum);
    }
    int dodge() const {
        int difference = increases[0] - decreases[0];
        return difference > 10 ? 10 : armorClassByte(difference);
    }
};

struct ConditionalArmorClass {
    std::int64_t dodge {0};
    int deflection {0};
    int shieldDecrease {0};

    explicit ConditionalArmorClass(const ArmorClassCache &cache) : deflection(cache.increases[4]) {}
    void add(const ArmorClassEffectData &effect, int race, int alignment, int damageFlags) {
        if (!effect.conditional(race, alignment, damageFlags)) return;
        if (effect.category == 0) dodge += effect.decrease ? -std::int64_t(effect.amount) : effect.amount;
        if (!effect.decrease && effect.category == 4)
            deflection = std::max(deflection, effect.amount);
        if (effect.decrease && effect.category == 3)
            shieldDecrease = std::max(shieldDecrease, effect.amount);
        // The versus AC result does not use conditional natural, armour, or shield
        // increases. Shield decreases add to the residual sum; deflection decreases
        // do not contribute to it.
    }
};

struct ArmorClassParts {
    int natural {0};
    int armour {0};
    int shield {0};
    std::int64_t dodgeAndDeflection {0};
};

inline ArmorClassParts armorClassParts(const ArmorClassCache &cache,
                                  const ConditionalArmorClass &conditional,
                                  int natural, int armour, int shield,
                                  bool versus, bool attackerSeen = true, bool touch = false) {
    ArmorClassParts result;
    if (touch) {
        // A touch attack ignores natural, worn armour and shield AC and their
        // decreases, but keeps the armour and shield increases.
        result.armour = cache.increases[2];
        result.shield = cache.increases[3];
    } else {
        result.natural = armorClassByte(std::int64_t(natural) + cache.increases[1] - cache.decreases[1]);
        result.armour = armorClassByte(std::int64_t(armour) + cache.increases[2] - cache.decreases[2]);
        result.shield = armorClassByte(std::int64_t(shield) + cache.increases[3] - cache.decreases[3]);
    }
    if (versus) {
        result.dodgeAndDeflection = (attackerSeen ? cache.dodge() : 0) + conditional.dodge +
                                   conditional.deflection +
                                   (touch ? 0 : std::int64_t(conditional.shieldDecrease));
    } else {
        // The general AC query includes both deflection increases and decreases.
        result.dodgeAndDeflection = cache.dodge() + armorClassByte(cache.increases[4] - cache.decreases[4]);
    }
    return result;
}

struct PowerMenuStatus;
struct ContextAction;

class CreatureClass;
class Talent;

class DamagePacket;
class Trigger;
class Encounter;
struct Spell;
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

/**
 * Where the clip a creature shows came from: its animation ID, -1 when
 * unknown, and whether it is a dialog animation.
 */
struct AnimationSource {
    int id {-1};
    bool dialog {false};
};

/**
 * The attack a swing shows: its target, the result its round's record holds,
 * whether it is ranged, the kind of attack the record holds (it picks the
 * weapon heard at the hit) and whether that attack is the killing blow.
 */
struct SwingAttack {
    uint32_t targetId {script::kObjectInvalid};
    AttackResultType result {AttackResultType::Invalid};
    bool ranged {false};
    PhysicalAttackKind weaponKind {PhysicalAttackKind::None};
    bool killingBlow {false};
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

    struct Perception {
        float sightRange {0.0f};
        float hearingRange {0.0f};
        std::map<uint32_t, RuntimeObjectRef<Object>> seen;
        std::map<uint32_t, RuntimeObjectRef<Object>> heard;
        /** Perceived creatures that are invisible to this one; kept while so. */
        std::map<uint32_t, RuntimeObjectRef<Object>> invisible;

        bool sees(uint32_t id) const {
            auto found = seen.find(id);
            return found != seen.end() && found->second.resolve() != nullptr;
        }
        bool hears(uint32_t id) const {
            auto found = heard.find(id);
            return found != heard.end() && found->second.resolve() != nullptr;
        }
        bool isInvisible(uint32_t id) const {
            auto found = invisible.find(id);
            return found != invisible.end() && found->second.resolve() != nullptr;
        }
        bool has(uint32_t id) const { return sees(id) || hears(id) || isInvisible(id); }
        void clear() {
            seen.clear();
            heard.clear();
            invisible.clear();
        }
    };

    /**
     * Detection rolls, kept between checks: the hider's stealth rolls and the
     * observer's awareness rolls, refreshed at most every 20 s of play.
     */
    struct PerceptionRolls {
        uint8_t hide {0};
        uint8_t moveSilently {0};
        uint8_t spot {0};
        uint8_t listen {0};
        float age {0.0f};
    };

    struct AutoBalanceContext {
        uint8_t multiplierSet {0};
        uint8_t playerLevelAtSpawn {0};
    };

    struct CombatState {
        bool active {false};
        CombatActivation activationType {CombatActivation::None};
        bool expiryHeld {false};
        RuntimeObjectRef<Object> attackTarget;
        // The target of the round most recently started, other than the creature itself.
        RuntimeObjectRef<Object> roundTarget;
        RuntimeObjectRef<Object> attemptedAttackTarget;
        RuntimeObjectRef<Object> attemptedSpellTarget;
        ActionType attackAction {ActionType::QueueEmpty};
        FeatType combatFeat {FeatType::Invalid};
        // The power a cast took the current round with, 0 for an item use.
        std::optional<int> roundSpell;
        // The current round has told the creature that its ranged weapon
        // cannot hurt its target. It is saved with the round.
        bool weaponIneffectiveReported {false};
        Timer expiryTimer;
        float decisionScriptTime {3.0f};
    };

    Creature(
        uint32_t id,
        std::string sceneName,
        Game &game,
        ServicesView &services);

    static bool classof(const Object *from) {
        return from->type() == ObjectType::Creature;
    }

    /**
     * The creature template for resRef. A template that does not exist is
     * replaced by the substitute creature template, and resRef then names it;
     * null when neither exists.
     */
    static std::shared_ptr<resource::Gff> findTemplate(resource::IGffs &gffs, std::string &resRef);

    // False, leaving the creature unloaded, when findTemplate finds nothing.
    bool loadFromBlueprint(const std::string &resRef);
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

    void clearAllActions(bool force = false, bool evenUncommandable = false) override;
    void teardownActions() override;
    void damage(
        int amount,
        const std::shared_ptr<Object> &damager) override;

    void applyDamageEffect(
        int amount,
        const std::shared_ptr<Object> &damager,
        std::optional<DamageReaction> reaction) override;

    void giveXP(int amount);
    void setXP(int xp);

    /**
     * Plays a sound-set sound, replacing the creature's last one. A dead
     * creature, or a party member at zero vitality, only plays its death
     * sound.
     */
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
    void onInternalStateRemoved(EffectId id, bool setsPose);
    void recomputeAIStateEffects(int pendingMask = 0xffff, uint64_t removingOrder = 0);
    struct EffectStackCounts { int positive {0}; int negative {0}; };
    EffectStackCounts effectStackCounts() const;
    void addEffectIcon(int icon);
    void removeEffectIcon(int icon);
    void resolveDamageShields(Creature &attacker);

    void damageForcePoints(int amount);
    void healForcePoints(int amount);
    /** Adds to the ordinary Force pool without capping it at the maximum. */
    void addCurrentForcePoints(int amount);
    int maxForcePoints() const;
    void setBodyFuel(bool active) { _bodyFuel = active; }
    void setThrowParryBlocked(bool blocked) { _throwParryBlocked = blocked; }
    bool throwParryBlocked() const { return _throwParryBlocked; }
    void refreshBodyFuel();
    int adjustedSpellForcePointCost(const Spell &spell, bool partyModifiers) const;
    PowerMenuStatus powerMenuStatus(const Spell &spell, const Object *target = nullptr) const;
    /**
     * The entries of the creature's power menu against \p target, or of its
     * own menu without one: the powers offered, each with its status.
     */
    std::vector<ContextAction> powerMenuActions(const Object *target, bool hostileMenu) const;
    /** The entries of the creature's form menu: every form it knows, all ready. */
    std::vector<ContextAction> formMenuActions() const;
    /**
     * The entries of the creature's behaviour menu, all ready. A combat style
     * outside the party behaviours first becomes the aggressive one.
     */
    std::vector<ContextAction> behaviorMenuActions();
    /** The implant mode the implant-switching feat has selected (1-4). */
    int implantMode() const;
    /** Switch to implant mode \p mode, remembering the previous one, and run the switch script. */
    void switchImplantMode(int mode);
    bool canPaySpellForcePointCost(const Spell &spell) const;
    /**
     * A class of the creature knows the power, and its Force point cost is
     * within the creature's reach as a query of the power counts it.
     */
    bool hasAffordableSpell(const Spell &spell) const;
    bool commitSpellForcePointCost(const Spell &spell, int &cost);
    uint32_t forceItemMask() const;
    int forceBodyLevel() const;
    int spellCasterLevel(const Spell &spell, bool itemOrCheat = false) const;
    int spellCastingClass(const Spell &spell) const;
    int spellLikeAbilityCasterLevel(int spellId) const;
    /** The spell is one of the creature's spell-like abilities, ready or not. */
    bool knowsSpellLikeAbility(SpellType spell) const;
    int adjustedClassLevel(int classIndex) const;
    bool readySpellLikeAbility(SpellType spell, int &casterLevel) const;
    void consumeSpellLikeAbility(SpellType spell, int casterLevel);
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
    /** Enough experience for the next level, below the title's maximum, neither dead nor downed. */
    bool isLevelUpPending() const;
    /** The level the creature's experience reaches, up to the title's maximum. */
    int potentialLevel() const;

    glm::vec3 getSelectablePosition() const override;
    int getNeededXP() const;

    Gender gender() const { return _gender; }
    const std::string &firstName() const { return _firstName.str(); }
    const std::string &lastName() const { return _lastName.str(); }
    ModelType modelType() const { return _modelType; }
    int appearance() const { return _appearance; }
    CreaturePresentation presentation() const;
    void setPresentation(const CreaturePresentation &presentation);
    uint16_t portraitId() const { return _portraitId; }
    std::shared_ptr<graphics::Texture> portrait() const { return _portrait; }
    float walkSpeed() const;
    /** The speed the player drives this creature at, and its appearance's limit. */
    float driveSpeed() const { return _driveSpeed; }
    float driveMaxSpeed() const { return _driveMaxSpeed; }
    void setDriveSpeed(float speed) { _driveSpeed = speed; }
    float runSpeed() const;
    float personalSpace() const { return _personalSpace; }
    float creaturePersonalSpace() const { return _creaturePersonalSpace; }
    /** How far above its feet the creature's walking space reaches. */
    float collisionHeight() const { return _collisionHeight; }
    CreatureSize size() const { return _size; }
    CreatureAttributes &attributes() { return _attributes; }
    const CreatureAttributes &attributes() const { return _attributes; }
    ItemAttributes &itemAttributes() { return _itemAttributes; }
    const ItemAttributes &itemAttributes() const { return _itemAttributes; }
    Faction faction() const { return _faction; }
    int xp() const { return _xp; }
    /** Experience a roster companion brought with it beyond its share of the party pool. */
    int joiningXP() const { return _joiningXP; }
    void setJoiningXP(int value) { _joiningXP = value; }
    float challengeRating() const { return _challengeRating; }
    int getReputationToward(const Creature &target) const;
    /** How an object of a faction outside the party regards this creature. */
    int getReputationFrom(Faction sourceFaction) const;

    /** The encounter that spawned this creature, if any. */
    uint32_t encounterId() const { return _encounterId; }
    bool isEncounterCreature() const { return _encounterId != script::kObjectInvalid; }
    void joinEncounter(const Encounter &encounter);
    /** The creature no longer counts against its encounter's wave. */
    void leaveEncounter();
    /**
     * The areas of effect the creature carries join the area it enters,
     * centred on it.
     */
    void placeCarriedAreasOfEffect(Area &area);
    bool hasLeftEncounter() const { return _leftEncounter; }

    /**
     * Keep turning toward an object while standing (kObjectInvalid releases
     * the lock). A dead or downed creature keeps its lock unless forced.
     * Releasing a lock turns the creature once toward what it was locked on.
     */
    void setOrientationLock(uint32_t objectId, bool force = false);
    uint32_t orientationLock() const { return _orientationLock; }
    /**
     * The creature's last swing was in an engaged exchange. While it is, the
     * controlled leader at rest is not driven to its rest pose.
     */
    void setEngagedExchange(bool engaged) { _engagedExchange = engaged; }

    // Facing

    /**
     * Faces at once, the model with it: placement, loading, conversations and
     * player input.
     */
    void setFacing(float facing) override;
    /**
     * Turns the facing at once and lets the model follow: the facing wanted
     * and the movement facing become it, and the model takes one step.
     */
    void turnTo(float facing);
    void turnToward(const glm::vec3 &point);
    void turnToward(const Object &object);
    void turnAwayFrom(const Object &object);
    /**
     * A step taken turns the facing along it; walking or running, the model
     * first steps toward its previous way.
     */
    void turnAlongStep(const glm::vec2 &step);
    /** The facing the model turns toward standing still; the facing stays. */
    void setDesiredFacing(float facing);
    /** Wants to face a point, unless it stands on it. */
    void setDesiredFacingToward(const glm::vec3 &point);

    // END Facing

    void setIsInConversation(bool isInConversation) override;
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

    /**
     * The pool maximum an ability effect moves: vitality for Constitution,
     * Force points for Wisdom and Charisma. Read before the effect is
     * admitted or removed.
     */
    std::optional<int> abilityPoolMaximum(const EffectInstance &effect) const;

    /**
     * An ability effect was admitted or removed. The current pool moves with
     * its maximum; a Constitution change can kill.
     */
    void followAbilityPool(const EffectInstance &effect, int previousMaximum);

    Alignment alignment() const;
    RacialType racialType() const { return _race; }
    void setRacialType(RacialType race) { _race = race; }
    Subrace subrace() const { return _subrace; }
    NPCAIStyle aiStyle() const { return _aiStyle; }
    /** The creature scripts name for this one to heal. It is not saved. */
    uint32_t healTarget() const { return _healTarget; }
    void setHealTarget(uint32_t target) { _healTarget = target; }
    int walkmeshMaterial() const { return _walkmeshMaterial; }
    bool isPC() const { return _isPC; }
    /** The character was made by the player (TSL item limitation). */
    bool isPlayerCreated() const { return _playerCreated; }
    bool isPartyInteract() const { return _partyInteract; }
    /** Runs the heartbeat script now and restarts the heartbeat interval. */
    void forceHeartbeat();
    /** Puts the party's clothing on a member whose body armour was refused on load. */
    void forceEquipClothing();
    void setPlayerCreated(bool value) { _playerCreated = value; }
    /**
     * The player-character flag a creature record carries as IsPC. A creature
     * whose record does not carry it is a player character.
     */
    void setPC(bool value) { _isPC = value; }
    /** The appearance's race label, which picks droid discharges. */
    const std::string &appearanceRace() const { return _appearanceRace; }
    uint8_t goodEvil() const { return _goodEvil; }
    void setGoodEvil(uint8_t value) { _goodEvil = value; }
    int assignedPuppet() const { return _assignedPuppet; }
    bool isPuppet() const { return _puppet; }
    CombatForm currentForm() const { return _currentForm; }
    CombatStance combatStance() const { return _combatStance; }
    void setCombatStance(CombatStance stance);
    bool isInTotalDefense() const;
    /**
     * The calendar day and time at which Total Defense last started the
     * creature's round, 0/0 when unset. Set, it also marks the stance as
     * already begun. It is not saved.
     */
    bool hasTotalDefenseStart() const { return _totalDefenseDay != 0 || _totalDefenseTime != 0; }
    void markTotalDefenseStart(uint32_t day, uint32_t time) {
        _totalDefenseDay = day;
        _totalDefenseTime = time;
    }
    void clearTotalDefenseStart() { markTotalDefenseStart(0, 0); }
    uint32_t totalDefenseDay() const { return _totalDefenseDay; }
    uint32_t totalDefenseTime() const { return _totalDefenseTime; }
    /**
     * Combat mode: 1 parry, 2 power attack, 3 improved power attack,
     * 4 counterspell, 5 flurry, 6 rapid shot. Only the forced setter exists:
     * no command produces a non-zero mode, so play keeps it at 0.
     */
    uint8_t combatMode() const { return _combatMode; }
    bool setCombatMode(uint8_t mode);
    /** Releases the ready pose and combat mode and strips special attacks from the round. */
    void cancelAllCombatModes();
    /** Record the kind of the round action most recently dispatched for this creature. */
    void setRoundActionKind(uint8_t kind) { _roundActionKind = kind; }
    uint8_t roundActionKind() const { return _roundActionKind; }
    /** Whether the current round has told this creature that its ranged weapon is ineffective. */
    bool weaponIneffectiveReported() const { return _combatState.weaponIneffectiveReported; }
    void setWeaponIneffectiveReported(bool reported) { _combatState.weaponIneffectiveReported = reported; }
    bool addStanceActions(CombatStance stance, const std::shared_ptr<Object> &target,
                          bool clearActions, bool toFront, bool fromRound);
    bool addSwitchWeaponsAction(bool immediate);
    /**
     * A player's weapon-swap request: refused in restrict mode and within a
     * second of the last. The second is real time, so it also passes while the
     * game is paused or a menu is open.
     */
    bool requestSwitchWeapons(bool immediate);
    /** An item used in combat holds back the next item use for three seconds of game time. */
    bool isItemUseCoolingDown() const { return _itemUseCooldown > 0.0f; }
    void startItemUseCooldown();
    const AutoBalanceContext &autoBalanceContext() const {
        return _autoBalanceContext;
    }

    void setGender(Gender gender) { _gender = gender; }
    /** The first and last name; the name is the two joined by a space. */
    void setFirstAndLastName(resource::LocString firstName, resource::LocString lastName);
    void setAppearance(int appearance) { _appearance = appearance; }
    void setMovementType(MovementType type);
    void setFaction(Faction faction) { _faction = faction; }
    /** Set the serialized base maximum and preserve the existing damage. */
    void setMaxHitPoints(int baseHitPoints) override;
    int maxHitPoints() const override;
    void setCurrentHitPoints(int hitPoints) override;

    /**
     * Take up a changed base Constitution: current vitality moves with the
     * maximum.
     */
    void recalculatePermanentVitality();

    /**
     * Add to a base ability score. A Constitution change moves current
     * vitality with the maximum.
     */
    void adjustBaseAbilityScore(Ability ability, int amount);

    /** Initialize a newly generated creature at full derived vitality. */
    void initializeGeneratedVitality();

    /** Current vitality translated back to the serialized base-HP axis. */
    int serializedCurrentHitPoints() const;

    /** Current Force points translated back to the serialized base axis. */
    int serializedCurrentForce() const;
    void setMovementRestricted(bool restricted) { _movementRestricted = restricted; }
    void setImmortal(bool immortal) { _immortal = immortal; }
    void setAIStyle(NPCAIStyle style) { _aiStyle = style; }
    void setAssignedPuppet(int puppet) { _assignedPuppet = puppet; }
    void setPuppet(bool puppet) { _puppet = puppet; }
    void setWalkmeshMaterial(int material) { _walkmeshMaterial = material; }
    void setCurrentForm(CombatForm form) { _currentForm = form; }
    bool isStealthed() const { return _stealthMode; }
    void setStealthMode(bool enabled);
    /**
     * The latest of the creature's own shells, the stealth exit's flash or
     * else the stealth field, with the effect application order current when
     * it began; null without one.
     */
    graphics::Texture *latestOwnShell(uint64_t &order) const;
    /** Whether the creature may use its Stealth skill at all. */
    bool canUseStealthSkill() const;
    /** Whether the player can put this creature into stealth. */
    bool isStealthCapable() const;
    /** The stealth skill: leave stealth, or enter it out of combat and conversation. */
    void toggleStealth();
    /** Using the Stealth skill: a creature without it is told so, others toggle stealth. */
    void useStealthSkill();
    /** Stealth holds the creature to a sneaking walk, unless TSL's Stealth Run frees it. */
    bool movesStealthily() const;
    /**
     * Break off the activities an engaging creature cannot keep: stealth ends
     * and the creature leaves any conversation it takes part in. A creature
     * holding a paused conversation stays in it; keepStealth preserves stealth.
     */
    void interruptActivities(bool keepStealth = false);
    /** A creature that paused its conversation keeps its place in it until it resumes. */
    void setConversationPaused(bool paused) { _conversationPaused = paused; }
    void beginSpellActivity(int spellId, bool itemCast, bool hostile, bool interrupts = true);
    void updateMindTrickPerception(const Creature &target, bool heard, bool seen);
    /**
     * Excite the creature for the duration of an excitedduration row, unless
     * it is already excited for longer. The creature the player controls
     * starts battle music when an enemy is near.
     */
    void setExcitedState(uint8_t row);
    bool isExcited() const { return _excitedMilliseconds != 0; }
    void setAutoBalanceContext(AutoBalanceContext context) {
        _autoBalanceContext = context;
        _autoBalancePlayerLevelAtSpawnSet = true;
    }

    // Animation

    /**
     * Turn the head toward an object no farther than maxDistance, for
     * creatures whose appearance tracks with the head; none ends the look.
     * Nothing changes while in combat state, while suspended, or for the
     * object already looked at. True when a new look begins.
     */
    bool lookAt(const std::shared_ptr<Object> &target, float maxDistance);
    /** The appearance's horizontal head-turn arc in degrees. */
    float headTurnHorizontal() const;
    /**
     * Height of the head above the creature's position: the posed head node of
     * the body model, or the appearance height when that node is missing, at
     * zero height or below the creature. Zero without a model.
     */
    float headHeight() const;
    /** Where free-look sees from: the free-look hook, the camera hook, or 2 m above the creature. */
    glm::vec3 freeLookPoint() const;
    /** The video effect shown while looking through the creature's eyes in free-look, -1 for none. */
    int freeLookVideoEffect() const;
    /**
     * Height the camera is hooked at: the camera hook's posed height relative
     * to its parent node, or the head height without a hook.
     */
    float cameraHookHeight() const;
    /**
     * A creature that played a world-space cut clip cannot turn its head
     * until a dialog animation change or the end of the conversation lifts it.
     */
    void setHeadLookSuspended(bool suspended) { _headLookSuspended = suspended; }

    /** A PlayAnimation constant as this creature plays it. */
    struct ScriptAnimation {
        std::string clip; // empty without one
        bool loop {false};
        float length {0.0f}; // seconds at natural speed, 0 when the model lacks the clip
        AnimationSource source;
    };
    ScriptAnimation getScriptAnimation(int constant) const;
    void playScriptAnimation(const ScriptAnimation &animation, float speed);
    int currentAnimationId() const { return _animationSource.id; }
    bool isPlayingDialogAnimation() const { return _animationSource.dialog; }

    void playAnimation(AnimationType type, scene::AnimationProperties properties = scene::AnimationProperties()) override;
    void playAnimation(AnimationType type, scene::AnimationProperties properties, AnimationSource source);

    void playAnimation(const std::string &name, scene::AnimationProperties properties = scene::AnimationProperties(),
                       AnimationSource source = AnimationSource());
    bool playAnimation(const std::shared_ptr<graphics::Animation> &anim, scene::AnimationProperties properties = scene::AnimationProperties());
    // Plays a conversation clip: a loop until replaced, a one-shot until it ends in the pause.
    bool playExternalAnimation(const std::shared_ptr<graphics::Animation> &anim, scene::AnimationProperties properties = scene::AnimationProperties());
    void resumeStateDrivenAnimation();
    /**
     * Shows the pause or combat-ready loop. At the end of a round pause an
     * injured creature out of combat state is given the injured pause.
     */
    void showPauseReadyAnimation(bool pauseEnd);
    /** Chooses the idle loop again, as when its pose is sent anew. */
    void refreshPauseAnimation();
    /**
     * The flinch of damage other than a weapon hit: once until another
     * animation is chosen, queued as a layer that plays at its natural speed
     * once the creature stands still in a loop.
     */
    void playDamageFlinch();
    /**
     * A cast or an item use stands in for the pose until a pose is shown
     * again. Its loop, if any, is the idle meanwhile; without one the pose
     * stays underneath. A running one-shot finishes first.
     */
    void holdCastAnimation(std::string loop = std::string());
    /** A module transition refills each Force shield's pool. */
    void refillForceShieldPools();
    void releaseCastAnimation();
    bool holdsCastAnimation() const { return _castAnimation.has_value(); }
    /** The conjure and cast visuals the creature's spells show on it. */
    SpellCastVisuals &spellCastVisuals() { return _spellCastVisuals; }
    /**
     * Another animation is chosen for the creature: the flinch it was held to
     * and a cast's or an item use's hold end.
     */
    void markAnimationChosen();
    /**
     * A one-shot the creature is given: queued to last its length at the
     * speed given, unless it is the one showing.
     */
    void playFireForgetAnimation(const std::string &clip, AnimationSource source, float speed = 1.0f);
    /**
     * A dialog animation: a loop plays at once; a one-shot makes the pause
     * the loop first and is queued.
     */
    void playDialogAnimation(AnimationType type, AnimationSource source);
    /** A weapon newly in hand is drawn; while play is paused the draw waits. */
    void playWeaponDraw();
    /** The flourish animation: a layer while the player drives, else queued. */
    void playWeaponFlourish();
    /**
     * Flourish the weapons in hand, as the player's flourish command
     * (\p playerCommand) or a script does.
     */
    void flourishWeapons(bool playerCommand);
    /** Below a fifth of its maximum vitality, unless its appearance disables the injured animations. */
    bool isInjured() const;
    /**
     * Stands in its plain pause or ready loop: not moving, not in a one-shot,
     * a script loop, a talk, sleep or death pose, and not given the injured
     * pause at a round pause end.
     */
    bool showsPauseReadyAnimation() const;
    /**
     * A one-shot is running: a clip started from the fire-and-forget queue
     * whose time has not run out, or a directly played clip still playing.
     */
    bool isPlayingOneShotAnimation() const;

    /**
     * Queues a one-shot lasting lengthMilliseconds at rate behind the ones
     * already queued. Each frame, while the
     * creature stands still and its current clip loops, the first queued clip
     * starts. A clip that waited is sped up to end when it would have ended
     * had it started at once, and runs for at least 50 ms. A clip the
     * animations table marks as an overlay, or any clip queued as layered,
     * plays over the loop at its natural speed instead; the time then only
     * decides when the next clip may start and how long a new loop waits, and
     * an overlay row is taken off when it runs out. When the time runs out the
     * creature returns to its stored loop. A different loop arriving empties
     * the queue; a walk or run leaves the queue waiting until it stands again.
     * A swing carries its attack, which the hit of the swing shows.
     */
    void addFireForgetAnimation(const std::string &clip, int lengthMilliseconds, float rate, bool layered,
                                AnimationSource source = AnimationSource(),
                                std::optional<SwingAttack> attack = std::nullopt);
    /**
     * Queues a clip of the creature's model at the speed given, lasting its
     * length at that speed; a clip the model lacks runs the shortest time and
     * shows nothing. False without a model.
     */
    bool addFireForgetAnimation(const std::string &clip, bool layered = false, AnimationSource source = AnimationSource(),
                                float speed = 1.0f);
    /**
     * Ends the running one-shot and returns to the stored loop; the queue is
     * kept. clearLayers also takes every layer off the body model.
     */
    void abortFireForgetAnimation(bool clearLayers = false);
    /** Empties the fire-and-forget queue. The running one-shot goes on. */
    void flushFireForgetQueue();
    /** The pause becomes the loop, unless it shows already. */
    void showDialogPause();
    /**
     * The creature stands still and shows a loop: no one-shot runs and the
     * clip under any layers loops.
     */
    bool currentClipLoops() const;

    /**
     * Play an animation as a layer over whatever the creature is already doing,
     * including while it is walking or running. Unlike the other playAnimation
     * overloads this neither waits for the creature to stand still nor takes
     * over its state-driven animation, so locomotion carries on underneath and
     * the layer disappears on its own once it has run.
     */
    void playOverlayAnimation(AnimationType type);
    void playOverlayAnimation(const std::string &clip);
    int selectMeleeAttackVariant(bool cinematic);

    void updateModelAnimation();
    /**
     * Each frame a standing creature's loop follows its state: the controlled
     * leader acting freely and following party members are held to their
     * pose, everyone else folds between pause, ready and injured pause.
     */
    void updateIdleLoop(const std::string &active);
    /**
     * Once per frame: counts the running one-shot down and returns to the
     * stored loop when its time runs out, updates the model animation, then
     * starts the first queued clip if the current clip loops.
     */
    void animateFireAndForget(float dt);

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
    /**
     * Remove the equipment ownership edge for immediate transfer or retirement.
     * Without the presentation update the model keeps showing the item.
     */
    std::shared_ptr<Item> takeEquippedItem(const std::shared_ptr<Item> &item, bool updatePresentation = true);
    /** Unequip an Item directly into an explicit owning inventory. */
    bool moveEquippedItemTo(
        const std::shared_ptr<Item> &item,
        Object &receiver);

    bool isSlotEquipped(int slot) const;

    std::shared_ptr<Item> getEquippedItem(int slot) const;
    CreatureWieldType getWieldType() const;
    bool hasAssassinateWeaponPresentation() const;

    const std::map<int, std::shared_ptr<Item>> &equipment() const { return _equipment; }
    /**
     * Runs \p evaluate while the creature answers as though it wore
     * \p equipment, then restores its own. Nothing is equipped or unequipped:
     * effects, ownership and presentation stay as they are.
     */
    void evaluateWithEquipment(
        std::map<int, std::shared_ptr<Item>> equipment,
        const std::function<void()> &evaluate);
    std::vector<std::shared_ptr<Object>> ownedRuntimeObjects() const override;

    // END Equipment

    // Body bag

    int bodyBagRow() const { return _bodyBagId; }
    /** The items a body leaves in its bag: droppable equipment of the first sixteen slots, then droppable inventory. */
    std::vector<std::shared_ptr<Item>> dropableItems() const;
    /** The body shows no weapons in its hands; its equipment is unchanged. */
    void stripHandWeapons();
    /** The bag made when this creature was destroyed, which its kept corpse passes picks to. */
    uint32_t spawnedBodyBag() const { return _spawnedBodyBag; }
    void setSpawnedBodyBag(uint32_t id) { _spawnedBodyBag = id; }

    // END Body bag

    // Pathfinding
    bool navigateTo(const glm::vec3 &dest, bool run, float distance, float dt);
    /** The last navigateTo found no way to its destination. */
    bool navigationFailed() const { return _navigationFailed; }

    /** Where the creature goes to use an object, and how near that point it must come. */
    struct UseRange {
        glm::vec3 point {0.0f};
        float range {0.0f};
    };

    /**
     * The point and range a walk toward a door or placeable heads for. Until
     * it is fixed, the walk takes the creature's current use point and range
     * once when either differs from what it holds (the point by more than a
     * millimetre), and from then on keeps them.
     */
    struct UseApproach {
        uint32_t target {script::kObjectInvalid};
        UseRange use;
        bool fixed {false};

        void follow(const UseRange &current);
    };

    /**
     * Where this creature uses \p target from. The distance starts at the
     * creature's personal space, at the target's position. A creature is used
     * from 1.4 metres in TSL, and in KotOR from the two creature personal
     * spaces and 0.3 metres. A trigger that is not an area transition is used
     * from its point nearest the creature, half a metre beyond the personal
     * space. A placeable is used from its nearer use point, three quarters of
     * a metre beyond the personal space, or from a tenth of a metre when it
     * wants precise use, that is not ignored, and the creature fits at the
     * point; five metres more for a corpse. A door is used from its nearest
     * action point, dropped onto the ground, three quarters of a metre beyond
     * the personal space, or from a tenth of a metre when it is locked and
     * wants precise use, that is not ignored, and the creature fits there.
     */
    UseRange useRange(const Object &target, bool ignorePreciseUse = false) const;
    /**
     * Whether \p target is within use range, lengthened by \p extra and a
     * tenth of a metre, of this creature, both in the same area, with a clear
     * line a metre and a half above the ground from the creature to the use
     * point. An object in no area is never within use range.
     * An area transition trigger is within use range while the creature
     * stands inside it.
     */
    bool isInUseRange(const Object &target, float extra) const;
    /**
     * Run, or walk unless \p run, toward the use point of \p target until
     * within its use range lengthened by \p extra. True once \p target is
     * within use range, so lengthened. A walk to a door or placeable sets out
     * for the use point and range of that moment and keeps them until it
     * arrives, unless \p followsPoint lets it take them anew once.
     */
    bool navigateToUse(const Object &target, float extra, float dt, bool run = true, bool followsPoint = false);
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
    void setObjectInvisible(const std::shared_ptr<Object> &object, bool invisible);
    void forgetPerceived(uint32_t id);
    /** Record a perception change as the last one and run the notice script. */
    void runOnNotice(const std::shared_ptr<Object> &object, PerceptionEvent event);
    void refreshVisibilityPerception();
    /**
     * Forget everything perceived, without notices, and look around at once;
     * a dead creature other than a player character does not look.
     */
    void perceiveAfresh();
    /** Take over another creature's perception, as a new party leader does. */
    void takePerception(Creature &other);
    /** Restore saved perception bits once saved references are bound. */
    void restoreSavedPerception(const std::vector<uint8_t> &data);

    uint32_t lastPerceivedId() const;
    bool lastPerception(PerceptionEvent event) const { return _lastPerception == event; }

    /** Spot and listen ranges; creatures other than the controlled one widen them in combat. */
    float spotRange() const;
    float listenRange() const;
    /** Take the perception ranges of a ranges row when the creature next looks around. */
    void setPerceptionRangeRow(int row);
    void resolvePerceptionRanges();

    /** Refresh the detection rolls when 20 s of play have passed since the last refresh. */
    void refreshPerceptionRolls();
    /** Whether this creature spots a creature now, reporting a spotted hider. */
    bool detectsBySight(const Creature &target, bool invisible) const;
    /** Whether this creature hears a creature now, through what lies between them. */
    bool detectsBySound(const Creature &target, bool invisible) const;
    /** Whether a creature this one attacks is one it has detected. */
    bool hasDetectedTarget(const Object &target) const;
    bool isBlind() const;

    /**
     * Update priority 0-4. Only priority 0 changes behaviour: it slows the
     * creature's full perception pass.
     */
    int aiLevel() const { return _aiLevel; }
    /** Script priority: remembered for resetting, out-of-range values give 3. */
    void setScriptAILevel(int level);
    void resetScriptAILevel();
    /** Whether a priority-0 creature is still skipping its full perception pass. */
    bool throttlesPerceptionPass();

    float perceptionPassTime() const { return _perceptionPassTime; }
    void setPerceptionPassTime(float time) { _perceptionPassTime = time; }
    bool perceptionStarted() const { return _perceptionStarted; }
    void setPerceptionStarted() { _perceptionStarted = true; }

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

    void setCombatState(bool active, CombatActivation activationType = CombatActivation::Direct, bool holdExpiry = false);
    CombatActivation combatActivationType() const { return _combatState.activationType; }
    bool clientCombatMode() const { return _clientCombatMode; }
    void setClientCombatMode(bool active);
    void broadcastCombatState(uint32_t opponent);
    void removeCombatInvisibilityEffects();
    void removeMindTrickEffects();
    uint32_t lastWeaponUsed() const { return _lastWeaponUsed; }

    bool isInCombat() const { return _combatState.active; }
    /**
     * Held helpless: by a state (in TSL any but the confusions), by a push, or
     * as a party member at zero vitality. TSL also counts meditation when
     * \p meditating asks for it.
     */
    bool isDebilitated(bool meditating = false) const;
    bool isTemporarilyDead() const;
    /** The first force shield effect on the creature names a shield. */
    bool hasForceShield() const;
    /**
     * Shows no reaction to an attack: unable to act, a party member at zero
     * vitality, or meditating.
     */
    bool isUnableToReact() const;
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
    /** The weapon of the off hand: a double-bladed right-hand weapon, else the left-hand weapon. */
    std::shared_ptr<Item> getEquippedOffhandWeapon() const;

    /** The creature lies under a knockdown effect. */
    bool isKnockedDown() const { return hasEffect(EffectType::Knockdown); }
    /** Show the fall of a knockdown under the given animation ID. */
    void showKnockdownFall(int animationId);

    int forcePoints() const { return _forcePoints; }
    int currentForce() const { return _currentForce + _temporaryForcePoints; }
    int currentForceWithoutTemporary() const { return _currentForce; }
    void regenerateForcePoints(int amount);
    void applyLevelUp(CreatureAttributes attributes, CreatureClass &clazz);

    uint32_t getAttemptedAttackTarget() const {
        auto target = _combatState.attemptedAttackTarget.resolve();
        return target ? target->id() : script::kObjectInvalid;
    }
    std::shared_ptr<Object> getAttackTarget() const {
        return _combatState.attackTarget.resolve();
    }
    std::shared_ptr<Object> getRoundTarget() const {
        return _combatState.roundTarget.resolve();
    }
    void setRoundTarget(const std::shared_ptr<Object> &target) { _combatState.roundTarget = target; }
    void clearRoundTarget() { _combatState.roundTarget.reset(); }
    uint32_t getLastHostileTarget() const {
        auto target = _lastHostileTarget.resolve();
        return target ? target->id() : script::kObjectInvalid;
    }
    ActionType getLastAttackAction() const { return _lastAttackAction; }
    /** The power the last spell round ended with: 0 after an item use, -1 when none since leaving combat. */
    int lastForcePowerUsed() const { return _lastForcePowerUsed; }
    FeatType getLastCombatFeat() const { return _lastCombatFeat; }
    AttackResultType getLastAttackResult() const { return _lastAttackResult; }
    int modifiedAttacks() const { return _modifiedAttacks; }
    bool hasAssuredHit() const { return _assuredHit; }
    void addPartyLeadershipTerms(AttackBonusBreakdown &result, const Creature *target) const;
    AttackBonusBreakdown getAttackBonusBreakdown(
        const Creature *target,
        const Item *weapon,
        bool offHand) const;
    int getAttackBonus(bool offHand = false) const;
    /** The touch-attack bonus: none when the right hand holds the other kind of weapon. */
    int getTouchAttackBonus(bool ranged) const;
    /**
     * The untargeted attack bonus of the equipped hand for a melee or ranged
     * attack. A melee attack uses the left-hand weapon for the off hand unless
     * \p doubleBladed, and has none while that weapon is ranged; a ranged attack
     * has none while the right hand holds a melee weapon. \p meleeTwoWeapon
     * keeps the two-weapon terms of a melee attack.
     */
    AttackBonusBreakdown getHandAttackBonusBreakdown(
        bool ranged,
        bool offHand,
        bool doubleBladed,
        bool meleeTwoWeapon) const;
    bool hasEffectImmunity(
        ImmunityType immunityType,
        const Creature *creator = nullptr) const;
    int getAbilityEffectModifier(Ability ability) const;
    int getEffectiveAbilityScore(Ability ability) const;
    int getEffectiveAbilityModifier(Ability ability) const;
    bool hasEffectiveFeat(FeatType feat) const;
    /** The d6 a sneak attack adds: the rank of the highest Sneak Attack feat held, 0 without one. */
    int sneakAttackDice() const;
    DefenseBreakdown getDefenseBreakdown(const Creature *attacker, int damageFlags, bool touch = false) const;
    int getDefense(const Creature *attacker, int damageFlags) const;
    int getDefense() const;
    // Ordinary ranged defense consumes the same permission as active saber throws.
    bool canParryRangedWeapon(const Creature &shooter, int damageFlags, bool &canReturn) const;
    AttackResultType resolveRangedDefense(const Creature &shooter, int damageFlags, int attackTotal) const;
    /**
     * Let \p target deflect this creature's touch attack. Only a ranged round
     * record allows it, opposed by that record's last roll and modifier; a
     * deflection overwrites the record's result.
     */
    AttackResultType resolveTouchDeflection(const Creature &target);
    // \p weapon is the shooter's weapon in the discharging hand, if any.
    AttackResultType resolveRangedMiss(const Creature &shooter, const Item *weapon) const;
    SavingThrowBreakdown getSavingThrowBreakdown(
        SavingThrow save, SavingThrowType type = SavingThrowType::All,
        const Object *versus = nullptr) const;
    int getSavingThrow(SavingThrow save) const;
    /** Add \p delta to the creature's own bonus to \p save, a signed byte that wraps. */
    void modifySavingThrowBonus(SavingThrow save, int delta);
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
    /**
     * Adds the item and effect damage modifiers of one hit and returns the
     * miscellaneous amount added to its base damage. \p dice chooses whether
     * the modifier dice are rolled or taken at a fixed face; the lowest face
     * applies to hand-specific dice only.
     */
    int addPhysicalDamageModifiers(
        DamagePacket &damage,
        DamageBreakdown &breakdown,
        const Creature *target,
        const Item *weapon,
        bool offHand,
        int criticalMultiplier,
        DamageDiceRoll dice = DamageDiceRoll::Rolled) const;
    /**
     * Whether the weapon in the given hand would damage \p versus: its highest
     * damage after the target's immunity, resistance and reduction, plus its
     * highest elemental damage bonus, is positive. No pool is consumed.
     */
    bool isWeaponEffective(Object *versus, bool offHand) const;
    /**
     * The highest damage one hit of the given hand's weapon deals to
     * \p versus: its highest dice, Strength and feat bonuses and the
     * miscellaneous modifiers, at least one point, then the target's immunity,
     * resistance and reduction without consuming a pool. Without a target
     * nothing is mitigated.
     */
    int getMaximumWeaponDamage(Object *versus, bool offHand) const;
    /**
     * The highest elemental damage bonus of one hit against \p versus, after
     * its immunity and resistance.
     */
    int getMaximumElementalDamageBonus(Object &versus) const;
    /**
     * The weapon's size relative to the creature's, from two smaller to one
     * larger; -10 outside that span.
     */
    int getRelativeWeaponSize(const Item &weapon) const;
    /**
     * The damage range of one hit with \p weapon (none: unarmed) in the given
     * hand, as shown to the player: the base dice, the Strength term, weapon
     * specialization and the damage feats, then, with \p includeModifiers, the
     * untargeted item and effect damage modifiers of the equipped hand. Each
     * bound is at least one.
     */
    void getDamageRange(const Item *weapon, bool offHand, bool includeModifiers, int &minimum, int &maximum) const;

    void setAttemptedSpellTarget(uint32_t id);
    std::shared_ptr<Object> attemptedSpellTarget() const { return _combatState.attemptedSpellTarget.resolve(); }
    float maxCleaveRange(const Creature *target) const;
    std::string getWeaponModelName(int slot) const;
    void setAttemptedAttackTarget(uint32_t target);
    void beginCombatAttack(Object &target, FeatType feat);
    // Attack records are stored in action-owned buffers, not in the creature.
    void setCurrentCombatAction(const std::shared_ptr<Action> &action) { _currentCombatAction = action; }
    std::shared_ptr<Action> currentCombatAction() const { return _currentCombatAction.lock(); }
    uint16_t currentCombatAttackType() const;
    AttackResultType currentCombatAttackResult() const;
    void finishCombatRound();
    void cancelCombat(int runEndRound = 0);
    /**
     * Gives up the fight: the creature and every creature within 250 m it
     * regards as hostile leave combat and lose their effects (with
     * retainOwnEffects, the effects each cast on itself stay); the creature
     * then joins the neutral faction.
     */
    void surrenderToEnemies(bool retainOwnEffects);
    /**
     * Every creature of the area that targets this one, or was last attacked
     * by it, leaves combat and drops its hostile actions against it.
     */
    void pacify();
    void recordQueuedAttack(Object &target);
    uint32_t getGoingToBeAttackedBy() const { return _incomingAttacker.id; }
    uint32_t getFirstAttacker();
    uint32_t getNextAttacker() { return _attackerList.next(); }
    int getLastAttackType() const { return _receivedAttack.scriptType(isInCombat()); }
    int getLastAttackMode() const { return _receivedAttack.scriptMode(isInCombat()); }
    void receiveAttackEvent(const AttackHistory *history, uint32_t attackerId,
                            const AttackEventFields *fields = nullptr);
    void receiveSpellCastAt(uint32_t casterId, int spell, int harmful);
    /** Record the weapon of a delivered attack as this attacker's last weapon. */
    void recordAttackWeapon(uint8_t weaponAttackType);
    /**
     * A party member enters client combat mode from its own live attempted
     * target and leaves it once nothing keeps it there. The stored target is
     * the member's party-slot target; the leader also holds while a dead target
     * is held. Returns true when the member left combat mode.
     */
    bool updateClientCombatMode(const Object *storedTarget, bool holdDeadTarget);
    void clearCurrentAttackTarget() { _combatState.attackTarget.reset(); }
    /**
     * An attacker that cannot see its target remembers where it stood. True
     * when it had already failed to see from this very spot.
     */
    bool noteBlockedAttackSight() {
        const bool again = _blockedAttackSight == _position;
        _blockedAttackSight = _position;
        return again;
    }
    void clearBlockedAttackSight() { _blockedAttackSight.reset(); }
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

    // Script-driven talent queries. Selection does not spend uses or resources.
    bool hasTalent(TalentType type, int id) const;
    bool hasSpellUsesLeft(const Spell &spell, size_t classIndex) const;
    std::shared_ptr<Talent> selectTalent(int category, int crMax, int inclusion,
                                       int excludeType, int excludeId) const;
    int getUnopposedSkillRank(SkillType skill, bool baseOnly = false) const;
    /** Skill rank against a creature: skill effects limited to its race or alignment count. */
    int getSkillRankVersus(SkillType skill, const Creature &versus) const;
    /**
     * The farthest this creature attacks \p target from. A ranged weapon
     * reaches its range, 30 metres for none, lengthened by Awareness in TSL,
     * or 20 metres when looking for another enemy to attack. In melee it is
     * half a metre beyond the desired distance; in a choreographed exchange in
     * TSL, 1.5 metres.
     */
    float maxAttackRange(const Object &target, bool choreographed = false, bool enemySearch = false) const;
    /**
     * The distance this creature closes to in melee with \p target: the two
     * hit radii and 1.6 metres between them for a creature; in a
     * choreographed exchange 1.4 metres in TSL, and in KotOR the two creature
     * personal spaces and 0.7 metres; twice the creature's personal space for a door,
     * which an attack closes on as the target of its own path; the use range
     * for a placeable; 1.5 metres for anything else.
     */
    float desiredAttackRange(const Object &target, bool choreographed = false) const;
    /** Whether the creature's appearance cannot run at all. */
    bool isImmobile() const;
    int featRemainingUses(FeatType feat) const;
    void spendFeatUse(FeatType feat);
    /**
     * The attack a request to use \p feat makes: nothing for a feat the
     * creature lacks, otherwise the highest rank of its chain the creature
     * holds, when that rank is an attack feat. Invalid means no attack.
     */
    FeatType attackFeatToUse(FeatType feat) const;
    std::optional<std::pair<std::shared_ptr<Item>, size_t>> itemForPower(int spellId) const;
    const Object *itemRepositoryOwner(bool useParty = true) const;

    // Scripts

    void runSpawnScript();
    void runBlockedScript(uint32_t blockingDoorId);
    void runEndRoundScript();
    void refreshCombatDecisionTimer();
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
    void runEndDialogScript();
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
    void beginForcePush(const glm::vec3 &destination);
    bool isForcePushed() const { return _forcedMove && _forcedMove->pushed; }
    /**
     * Carries the creature straight over the ground to destination in a third
     * of a second, as a Force jump does, whatever it is doing meanwhile.
     */
    void beginLeap(const glm::vec3 &destination);
    /** Ends a push or leap carrying the creature; it stays where it has got to. */
    void endForcedMove();

    int furyDamageBonus() const { return _furyDamageBonus; }
    int furySpellState() const { return _furySpellState; }
    void applyFuryState(int spellId);
    void clearFuryState();
    void incrementFuryDamageBonus();

protected:
    bool canExecuteActions() const override;
    glm::quat presentedOrientation() const override;

private:
    // Which per-frame animation rules a creature follows: the controlled
    // leader acting freely is driven, a party member following the leader
    // follows, everyone else takes the default rules.
    enum class AnimationUpdater {
        Default,
        Drive,
        Follow
    };

    /** The area whose music this creature drives: its own, when the player controls it. */
    std::shared_ptr<Area> battleMusicArea() const;
    /** The weapon swap and the flourish share one second of real time between uses. */
    bool isSwitchWeaponsCoolingDown() const;
    void startSwitchWeaponsCooldown();
    void updateExcitement();
    void updateForcedMove(float dt);
    AnimationUpdater animationUpdater() const;
    /** The party follow slot, 1 or 2, or -1 outside them. */
    int followSlot() const;
    void updateTurning();
    void rotateStep(float target);
    bool isHostileCombatTurn() const;
    void playTurnAnimation(float sign);
    void updateFidget();
    void updateStateHeartbeat(float dt);
    void updateRegeneration(float dt);
    void updateMineCheck(float dt);
    void reportMineDetection(const Trigger &trap, int roll, int rank, int dc);
    void updateK1HitPointRegeneration(float dt);
    void updateHitPointRegeneration(float dt);
    void updateForcePointRegeneration(float dt);
    void updateK1ForcePointRegeneration(float dt);
    bool talentPowerAffordable(const Spell &spell) const;
    struct ItemPowerSource {
        std::shared_ptr<Item> item;
        size_t property;
        bool equipped;
    };
    std::vector<ItemPowerSource> talentItemPowers() const;
    // Fractional regeneration is runtime state, not a load-time resource grant.
    float _hitPointAccumulator {0.0f};
    float _forcePointAccumulator {0.0f};
    Timer _stateSupportTimer;
    // A move that carries the creature bodily, a Force push or a leap: one at
    // a time, the latest replacing any other. Only a push holds the creature.
    struct ForcedMove {
        glm::vec3 destination;
        float speed;
        bool pushed;
    };
    std::optional<ForcedMove> _forcedMove;
    int getSavingThrowBase(SavingThrow save) const;
    int getSavingThrowEffectBonus(SavingThrow save, SavingThrowType type,
                                 const Object *versus) const;

    // Head look-at: the object looked at, and whether the look still runs.
    uint32_t _lookAtId {script::kObjectInvalid};
    RuntimeObjectRef<Object> _lookAtTarget;
    float _lookAtDistance {0.0f};
    bool _lookAtRunning {false};
    bool _headLookSuspended {false};

    void updateLookAt();
    std::vector<scene::ModelSceneNode *> lookAtModels() const;

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
    bool _isPC {true};
    bool _playerCreated {false};
    // Body armour this creature could not wear when it was loaded.
    bool _bodyRefusedOnLoad {false};
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
    // LvlStatList: the hit die and base Force point grant of each individual
    // level. The player character's maxima are derived from it.
    struct LevelStats {
        uint8_t hitDie {0};
        uint8_t forcePoints {0};
    };
    std::vector<LevelStats> _levelStats;
    int16_t _currentForce {0};
    int32_t _temporaryHitPoints {0};
    int32_t _temporaryForcePoints {0};
    int32_t _bonusForcePoints {0};
    int8_t _furyDamageBonus {-1};
    int32_t _furySpellState {0};
    bool _temporaryHitPointsRestored {false};
    bool _temporaryForcePointsRestored {false};
    // The Constitution modifier vitality reads. It is taken up on load, on
    // base Constitution and level changes and when a Constitution effect is
    // admitted or removed.
    int _vitalityConstitutionModifier {0};
    int16_t _refBonus {0};
    int16_t _willBonus {0};
    int16_t _fortBonus {0};
    uint8_t _goodEvil {0};
    float _challengeRating {0};
    uint32_t _encounterId {script::kObjectInvalid};
    bool _leftEncounter {false};
    uint32_t _orientationLock {script::kObjectInvalid};
    bool _engagedExchange {false};
    uint32_t _xp {0};
    int _joiningXP {0};
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
    // Signed armor-adjustment caches. These start at zero and have no nonzero producer
    // connected here.
    std::array<int8_t, 2> _armorSkillAdjustments {{0, 0}};
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
    // A row of 0 takes the appearance's bag row.
    uint8_t _bodyBagId {0};
    // The perception range row of a creature's record: by default the row its
    // appearance names.
    uint8_t _perceptionId {11};

    CreatureAttributes _attributes;
    struct SpellLikeAbility {
        int spell {0};
        // Keep the full encoded value: some consumers require exactly one,
        // while others accept any nonzero readiness value.
        int flags {0};
        int casterLevel {0};
    };
    std::vector<SpellLikeAbility> _spellLikeAbilities;
    std::map<int, std::shared_ptr<Item>> _equipment;
    // END Serializable

    ModelType _modelType {ModelType::Creature};
    std::shared_ptr<graphics::Texture> _portrait;

    // The facing the model shows turns toward the facing wanted standing
    // still and toward the movement facing while moving.
    float _presentationFacing {0.0f};
    float _desiredFacing {0.0f};
    float _movementFacing {0.0f};
    int _frameMilliseconds {0};

    // Current path that the creature is following, its velocity and position at
    // the previous frame.
    std::optional<Path> _path;
    bool _navigationFailed {false};
    // The walk of navigateToUse under way.
    std::optional<UseApproach> _useApproach;
    glm::vec3 _pathVelocity;
    glm::vec3 _previousPosition;
    // When there is no progress on the path, apply _stuckForce to steer the
    // creature in a random direction until the timer runs out.
    Timer _stuckTimer;
    glm::vec3 _stuckForce;

    float _walkSpeed {0.0f};
    float _driveSpeed {0.0f};
    float _driveMaxSpeed {0.0f};
    float _runSpeed {0.0f};
    float _personalSpace {0.6f};
    float _creaturePersonalSpace {0.6f};
    float _collisionHeight {0.5f};
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
    std::weak_ptr<Action> _currentCombatAction;
    bool _clientCombatMode {false};
    uint8_t _combatMode {0};
    float _mineCheckTime {0.0f};
    uint32_t _lastWeaponUsed {script::kObjectInvalid};
    SavedObjectReference _incomingAttacker;
    // Where the creature last stood when it could not see its attack target.
    std::optional<glm::vec3> _blockedAttackSight;
    AttackerList _attackerList;
    AttackHistory _receivedAttack;
    CombatStance _combatStance {CombatStance::None};
    uint32_t _totalDefenseDay {0};
    uint32_t _totalDefenseTime {0};
    uint8_t _roundActionKind {0};
    bool _stealthMode {false};
    bool _conversationPaused {false};
    /** Milliseconds of excitement left, counted down by the frame time; 0 when not excited. */
    uint32_t _excitedMilliseconds {0};
    /** System clock time, in microseconds, at which the weapon swap is ready again. */
    uint64_t _switchWeaponsReadyAt {0};
    float _itemUseCooldown {0.0f};
    int _lastMeleeAttackVariant {-1};
    RuntimeObjectRef<Object> _lastHostileTarget;
    ActionType _lastAttackAction {ActionType::QueueEmpty};
    FeatType _lastCombatFeat {FeatType::Invalid};
    int _lastForcePowerUsed {-1};
    AttackResultType _lastAttackResult {AttackResultType::Invalid};
    int _modifiedAttacks {0};
    bool _assuredHit {false};
    bool _assuredDeflection {false};
    bool _assuredReturn {false};
    bool _immortal {false};
    std::shared_ptr<resource::SoundSet> _soundSet;
    uint32_t _spawnedBodyBag {script::kObjectInvalid};
    Perception _perception;
    PerceptionRolls _perceptionRolls;
    float _perceptionPassTime {0.0f};
    bool _perceptionStarted {false};
    std::optional<PerceptionEvent> _lastPerception;
    float _blindSpot {120.0f};
    // The stealth field's shell, and the stealth exit's one-second shell,
    // each with the effect application order current when it began.
    std::shared_ptr<graphics::Texture> _stealthShell;
    uint64_t _stealthShellOrder {0};
    std::shared_ptr<graphics::Texture> _cloakRemovalShell;
    uint64_t _cloakRemovalShellOrder {0};
    float _cloakRemovalTime {0.0f};
    int _aiLevel {1};
    int _savedAILevel {-1};
    int _perceptionThrottle {0};
    // Heartbeat interval (ms) and the due updates a level-0 creature waits.
    int _heartbeatInterval {3000};
    int _heartbeatThrottle {0};
    void updateAILevel();
    /** Draws the detection rolls and the perception, heartbeat and fidget counters a new creature starts with. */
    void startUnsavedCounters();
    /** Starts the heartbeat clock of a creature whose heartbeat has not been stamped. */
    void startUnstampedHeartbeat();
    void presentStealth(bool enabled, bool announce);
    void presentStealthField(bool enabled, bool announce);
    // TSL Dampen Sound on an equipped item.
    bool hasSilentMove() const;
    int _pendingPerceptionRangeRow {-1};
    bool _pendingPerceptionRangeByAppearance {false};
    uint8_t _visibilityCounterBits {0};
    bool _trueSeeingUltravisionQuirk {false};
    NPCAIStyle _aiStyle {NPCAIStyle::DefaultAttack};
    uint32_t _healTarget {script::kObjectInvalid};

    uint32_t _footstepType {0};
    int _walkmeshMaterial {-1};
    int _gold {0}; /**< aka credits */
    std::string _envmap;
    // The appearance's death visual and the body node it goes off at.
    std::optional<int> _deathVisual;
    std::string _deathVisualNode;
    // Read from the appearance with the rest of its properties.
    float _hitRadius {0.0f};
    int _soundAppType {0};
    std::string _appearanceRace;
    bool _isListening {false};

    std::shared_ptr<audio::AudioSource> _audioSourceVoice;
    std::shared_ptr<audio::AudioSource> _audioSourceFootstep;
    bool _lightsaberIdlePowerDownPending {false};
    Timer _lightsaberIdlePowerDownTimer;

    // Animation

    bool _animDirty {true};
    bool _animFireForget {false};
    bool _injuredPause {false}; // given the injured pause at a round pause end
    bool _injuredIdle {false};  // the idle pause turned into the injured pause
    bool _damageFlinchHeld {false}; // flinched since another animation was chosen
    std::optional<std::string> _castAnimation; // held by a cast or an item use
    bool _equipmentHidden {false}; // an animation that hides equipped items put the hand weapons away
    SpellCastVisuals _spellCastVisuals;
    bool _disableInjuredAnim {false};

    struct FireForgetEntry {
        std::string clip;
        uint32_t startDay {0};  // calendar day it was queued at
        uint32_t startTime {0}; // and the time of that day
        int lengthMilliseconds {0};
        float rate {1.0f};
        bool layered {false};
        AnimationSource source;
        std::optional<SwingAttack> attack;
    };
    struct RunningOneShot {
        std::string clip;
        int id {-1}; // its animation ID
        bool layered {false};
        bool overlayRow {false}; // taken off the model when its time runs out
        float remainingMilliseconds {0.0f};
    };
    // The loop a one-shot returns to: a state-driven pose goes back through
    // the pose path, any other loop plays again with its properties.
    struct StoredLoop {
        std::string clip;
        scene::AnimationProperties properties;
        bool stateDriven {true};
        AnimationSource source;
    };
    std::deque<FireForgetEntry> _fireForgetQueue;
    std::optional<RunningOneShot> _oneShot;
    std::optional<StoredLoop> _storedLoop;
    AnimationSource _animationSource; // the clip shown
    std::optional<SwingAttack> _swingAttack; // the attack of the last swing started
    bool _weaponDrawPending {false};         // a draw waiting for play to resume
    // Idle fidgets: milliseconds spent in the plain pause, and when the next
    // fidget is due.
    int _fidgetTime {0};
    int _fidgetDelay {0};
    std::shared_ptr<graphics::LipAnimation> _lipAnimation;
    // The pose a creature lies in once dead, chosen when it first dies: a
    // dead pose reached through its die clip, the second dead pose reached
    // through the second die clip, or the third dead pose reached through its
    // own lead-in.
    enum class DeathPose {
        Dead,
        Dead1,
        Dead3
    };
    std::optional<DeathPose> _deathPose;

    // END Animation

    // Ground tilt

    // The appearance follows the ground while alive.
    bool _groundTiltAppearance {false};
    // The tilt last aimed for and the tilt shown, applied beneath the facing.
    glm::quat _groundTiltTarget {1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat _groundTilt {1.0f, 0.0f, 0.0f, 0.0f};

    void updateGroundTilt();
    void updateTransform() override;

    // END Ground tilt

    // Scripts

    // END Scripts

    void loadTransformFromGIT(const resource::generated::GIT_Creature_List &git);

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
    /**
     * Records a loop arriving; a loop other than the stored one empties the
     * queue. False while a one-shot runs: the loop waits for it.
     */
    bool storeLoop(const std::string &clip, const scene::AnimationProperties &properties, bool stateDriven,
                   AnimationSource source);
    void returnToStoredLoop();
    /**
     * The body shows \p clip: one that hides equipped items puts the hand
     * weapons away; the next one that does not, outside a conversation, brings
     * them back and takes away the creature's thrown lightsaber still in flight.
     */
    void presentEquippedItems(const std::string &clip);
    /** A new clip is shown; any but the plain pause restarts the fidget time. */
    void setAnimationSource(AnimationSource source);
    void playTopFireForgetAnimation();
    /**
     * The hit of a melee swing sounds the weapon against the target, makes a
     * creature-model target that can react flinch at once, as a layer outside
     * its queue, and may draw a pain grunt from a creature target. A parried
     * or missed swing against a parrying target sounds the weapon's parry.
     */
    void presentSwingHit();
    /** The weapon of the swing's attack heard against the target, or its parry. */
    void playHitSound(const Object &target, bool parried);
    /** The item whose weapon sound the swing's attack makes, none for an unarmed or creature attack. */
    std::shared_ptr<Item> weaponForHitSound(PhysicalAttackKind kind) const;
    /** The material a weapon sounds against on the target; empty when it has none. */
    std::string hitSoundMaterial(const Object &target) const;
    /** A swing, clash or parry sound of the weapon the creature swings, one of \p variants. */
    void playSwingSound(const std::string &name, int variants);
    /** Plays a weaponsounds.2da sound at the creature. */
    void playWeaponSound(int weaponSoundRow, const std::string &column);
    /** The shown clip is a parry. */
    bool isShowingParry() const;
    /** The leader is in the creature's area within 30 m, where it hears the creature's voice. */
    bool isHeardByLeader() const;
    void queueLoopTransition(int newId, int oldId, float speed);
    void queueTransitionPair(const std::string &clip, int clipId, const std::string &follow, int followId);
    void switchOffOverlay(const RunningOneShot &oneShot);
    /** The draw played as a layer over movement, or empty without one. */
    std::string getWeaponDrawOverlayAnimation() const;

    std::string getAnimationName(AnimationType anim) const override;
    std::string getAnimationName(CombatAnimation anim, CreatureWieldType wield, int variant) const;
    /** The clip under any layers, or empty without one. */
    std::string getActiveAnimationName() const override;

    std::string getDeadAnimation() const;
    std::string getDieAnimation() const;
    bool isInFallenLoop() const;
    /** The clips that lead a dying creature into its dead pose. */
    void playDieTransition(bool prone);
    /** The appearance's death visual, shown at its node on the body. */
    void presentDeathVisual();
    std::string getHeadTalkAnimation() const;
    std::string getPauseAnimation(bool injured) const;
    std::string getReadyAnimation() const;
    std::string getDamageFlinchAnimation() const;
    std::string getRunAnimation() const;
    std::string getStealthWalkAnimation() const;
    std::string getTalkNormalAnimation() const;
    std::string getWalkAnimation() const;

    /**
     * @return creatureAnim if model type is creature, elseAnim otherwise
     */
    inline std::string getFirstIfCreatureModel(std::string creatureAnim, std::string elseAnim) const;

    bool getWeaponInfo(WeaponType &type, WeaponWield &wield) const;
    int getReadyWeaponClass() const;
    /**
     * The attack penalty of fighting with two weapons. A light off-hand weapon
     * eases the main hand's penalty; \p rangedSheet judges it, as the ranged
     * attack value of the equip screen does, by the right-hand weapon's size.
     */
    int getTwoWeaponAttackPenalty(
        const Item *weapon,
        bool offHand,
        int *smallOffhandBonus = nullptr,
        bool rangedSheet = false) const;
    int getDuelingBonus() const;

    // END Animation

    // Blueprint
    void deserializeAll(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    void deserializeName(const resource::Gff &gff);
    void deserializeSoundSet(const resource::Gff &gff);
    void deserializeAttributes(const resource::Gff &gff);
    void deserializeClass(const resource::Gff &gff);
    void deserializePerception(const resource::Gff &gff);
    int skillRank(SkillType skill, bool baseOnly, const Creature *versus) const;
    bool isStationaryForDetection() const;
    void reportStealthDetection(const Creature &target, int spotRoll, int hideRoll, int aware, int rank,
                                int observerStill, int targetStill, int distance, int facing, int run, int combat) const;
    std::optional<int> admitLoadedEquipment(
        const Item &item, uint32_t slotMask,
        const std::map<int, std::shared_ptr<Item>> &loaded, bool &bodyRefused);
    void deserializeOwnedItemsAndEquipment(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    /** Credits, chemicals, components and pazaak cards read into the inventory are acquired as counts. */
    void countLoadedItems();
    void appendEquippedItemEffects(
        std::deque<EffectInstance> &effects,
        int slot,
        const std::shared_ptr<Item> &item, bool onlyDeferredEffects = false) const;
    std::deque<EffectInstance> effectsWithoutEquippedSource(
        const Item *source) const;
    std::deque<EffectInstance> rebuildEquippedItemEffects(
        const std::map<int, std::shared_ptr<Item>> &equipment) const;
    void refreshVitalityConstitution();
    void followMaximumHitPoints(int previousMaximum);
    bool hitPointsAtDeathThreshold() const;
    void restoreSerializedVitality();
    void applyHitPointDamage(int amount, const std::shared_ptr<Object> &damager,
                             std::optional<DamageReaction> reaction = std::nullopt);
    int consumeTemporaryHitPoints(int amount);
    void updateDeathFromCurrentHitPoints();
    // END Blueprint
};

} // namespace game

} // namespace reone
