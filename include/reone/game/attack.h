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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "reone/game/combatfeedback.h"
#include "reone/game/effect/damage.h"
#include "reone/game/runtimeref.h"
#include "reone/game/types.h"
#include "reone/script/types.h"
#include "reone/system/smallvector.h"

namespace reone {

namespace scene {
class ModelNodeSceneNode;
class ModelSceneNode;
class ISceneGraph;
} // namespace scene

namespace game {

class Action;
struct CombatRound;
class Creature;
class Effect;
class Game;
class IAnimations;
class Item;
class Object;
struct ServicesView;
struct SavedPhysicalAction;

/** The two fields transported by the attacked event, not the actor's action enum. */
struct AttackHistory {
    uint16_t type {0};
    uint8_t mode {0};

    /** The script attack mode of an object not fighting, or not a creature. */
    static constexpr int kScriptModeNone = 0x7f000000;

    int scriptType(bool inCombat) const {
        if (!inCombat) return 0;
        return type == 11 ? 9 : type == 30 ? 10 : 0;
    }
    int scriptMode(bool inCombat) const {
        if (!inCombat) return kScriptModeNone;
        constexpr int values[] = {0, 1, 2, 3, 0, 4, 5};
        return mode < 7 ? values[mode] : 0;
    }
};

/**
 * Non-reference attack-save fields. Together with AttackHistory, ReactObject
 * and AmmoItem, these make up the 25 serialized attack fields. This record
 * transports history; it does not apply damage or generate visual reactions.
 */
struct AttackEventFields {
    uint8_t group {0xff};
    uint16_t animationLength {0};
    uint32_t missedBy {0};
    uint8_t result {0};
    uint16_t reactionDelay {0};
    uint16_t reactionAnimation {10001};
    uint16_t reactionAnimationLength {0};
    uint8_t concealment {0};
    int32_t ranged {0};
    int32_t sneakAttack {0};
    uint8_t weaponAttackType {0};
    std::array<float, 3> rangedTarget {};
    // Retain gameplay precision; the signed-short cast is at save only.
    std::array<int32_t, 15> damage;
    uint8_t killingBlow {0};
    uint8_t coupDeGrace {0};
    uint8_t criticalThreat {0};
    uint8_t deflected {0};
    std::string attackDebugText;
    std::string damageDebugText;

    AttackEventFields() { damage.fill(-1); }
};

/** Per-recipient snapshot. Next deliberately does not resolve saved IDs. */
class AttackerList {
public:
    template <class Objects, class Matches, class Id>
    uint32_t first(const Objects *objects, Matches matches, Id id) {
        if (!objects) return 0x7f000000;
        _ids.clear();
        for (const auto &object : *objects) {
            if (matches(object)) _ids.push_back(id(object));
        }
        if (_ids.empty()) return 0x7f000000;
        _next = 1;
        return _ids.front();
    }
    uint32_t next() {
        return _next < _ids.size() ? _ids[_next++] : 0x7f000000;
    }
    void clear() { _ids.clear(); _next = 0; }

private:
    std::vector<uint32_t> _ids;
    size_t _next {0};
};

/** Identifies an item-on-hit effect subtype. */
enum class ItemOnHitSubtype : uint16_t {
    Sleep = 0,
    Stun = 1,
    Paralyze = 2,
    Confusion = 3,
    Fear = 4,
    Slow = 5,
    AbilityDrain = 6,
    ItemPoison = 7,
    SlayRG = 8,
    SlayAG = 9,
    InstantDeath = 10,
    Knockdown = 11,
};

/** Stores one active item property before attack resolution. */
struct ItemOnHitProperty {
    /** The item-on-hit effect subtype. */
    ItemOnHitSubtype subtype {ItemOnHitSubtype::Sleep};
    /** The activation chance as a percentage. */
    int chance {0};
    /** The effect duration in seconds. */
    float duration {0.0f};
    /** The saving throw difficulty class. */
    int difficultyClass {20};
    /** The required saving throw category. */
    SavingThrow savingThrow {SavingThrow::None};
    /** The saving throw subtype. */
    SavingThrowType savingThrowType {SavingThrowType::All};
    /** A subtype-specific value. */
    int parameter {0};
    /** True for a state-duration handler, independently of table selection. */
    bool durationBranch {false};
};

/**
 * Chance and duration carry across the entire on-hit property iteration.
 * Slay properties inherit the previous chance, initially zero. Table selection
 * and handler selection are separate steps.
 */
struct ItemOnHitSelectionState {
    int chance {0};
    int rounds {0};

    void select(ItemOnHitSubtype subtype) {
        if (subtype == ItemOnHitSubtype::AbilityDrain ||
            subtype == ItemOnHitSubtype::ItemPoison ||
            subtype == ItemOnHitSubtype::InstantDeath ||
            subtype == ItemOnHitSubtype::Knockdown) {
            chance = 100;
        }
    }
};

constexpr bool usesItemOnHitDurationHandler(ItemOnHitSubtype subtype) {
    return subtype >= ItemOnHitSubtype::Sleep && subtype <= ItemOnHitSubtype::Slow;
}

// The property is a byte and the race is an unsigned word. No wildcard.
constexpr bool matchesSlayRacialGroup(int race, int parameter) {
    return static_cast<uint16_t>(race) == static_cast<uint8_t>(parameter);
}

/** Stores one admitted item-on-hit effect until its impact resolves. */
struct ItemOnHitApplication {
    /** The item-on-hit effect subtype. */
    ItemOnHitSubtype subtype {ItemOnHitSubtype::Sleep};
    /** The effect duration in seconds. */
    float duration {0.0f};
    /** A subtype-specific value. */
    int parameter {0};
    /** Bound attacker retained across resolution and impact. */
    RuntimeObjectRef<Object> creator;
    /** The retained feedback values for the effect outcome. */
    EffectOutcomeBreakdown effectOutcome;
    /** True if the impact emits effect-outcome feedback; false otherwise. */
    bool emitEffectOutcome {false};
};

void applyItemOnHitApplications(std::vector<ItemOnHitApplication> applications,
                               Object &target, Game &game, ServicesView &services);

static constexpr float kAttackDamageDelay = 1.0f;

/**
 * A scripted attack's forced values: the swing (an animations row; for a
 * ranged attacker the weapondischarge row), the result of every attack of the
 * round and the damage of each hit.
 */
struct CutsceneAttack {
    int animation {0};
    int result {static_cast<int>(AttackResultType::Miss)};
    int damage {0};
};

/**
 * Predicate for melee weapon.
 *
 * Stun Baton and Hand-to-Hand is NOT a melee weapon. These require special
 * animations that are different from regular melee weapons.
 */
bool isMeleeWieldType(CreatureWieldType type);

/**
 * Returns true if \p result is HitSuccessful, CriticalHit, AutomaticHit.
 * The rest are variouns forms for failed attacks (Parried, Deflected, etc.)
 */
bool isAttackSuccessful(AttackResultType result);

/**
 * Predicate for feats that resolve as a physical weapon or unarmed attack.
 */
bool isPhysicalAttackFeat(FeatType feat);

/**
 * A running physical attack is over once its target is dead, or is a party
 * member down at zero vitality.
 */
bool isAttackTargetDown(const Object &target);

struct AttackBonusBreakdown {
    int baseAttackBonus {0};
    int strengthModifier {0};
    int dexterityModifier {0};
    int dualWieldPenalty {0};
    int smallOffhandBonus {0};
    int featBonus {0};
    FeatType duelingFeat {FeatType::Invalid};
    int duelingBonus {0};
    int closeProximityRangedBonus {0};
    int meleeOnRangedBonus {0};
    int weaponFocusBonus {0};
    int targetingBonus {0};
    int superiorWeaponFocusBonus {0};
    int formBonus {0};
    int dualStrikeBonus {0};
    int effectBonus {0};
    int inspireFollowersBonus {0};
    int leaderCharismaBonus {0};
    int crushOppositionPenalty {0};

    int total() const {
        return baseAttackBonus +
               strengthModifier +
               dexterityModifier +
               dualWieldPenalty +
               smallOffhandBonus +
               featBonus +
               duelingBonus +
               closeProximityRangedBonus +
               meleeOnRangedBonus +
               weaponFocusBonus +
               targetingBonus +
               superiorWeaponFocusBonus +
               formBonus +
               dualStrikeBonus +
               effectBonus +
               inspireFollowersBonus +
               leaderCharismaBonus +
               crushOppositionPenalty;
    }
};

struct DefenseBreakdown {
    int total {0};
    int armor {0};
    int dexterity {0};
    int classDefense {0};
    int natural {0};
    int dodgeAndDeflection {0};
    int feat {0};
    int stance {0};
    int form {0};
    int debilitationPenalty {0};
};

/**
 * The parts of the last deflection roll made against the round's shots, as
 * the combat log reports them. A roll writes its d20, totals, base attack
 * bonus, Dexterity and effects; each feat, power or form part only when it
 * applies, so a part from an earlier roll of the round stays.
 */
struct DeflectionBreakdown {
    int roll {0};
    int attackTotal {0};
    int total {0};
    int baseAttackBonus {0};
    int dexterity {0};
    int effects {0};
    FeatType jediDefenseFeat {FeatType::Invalid};
    int jediDefenseBonus {0};
    int redirectionBonus {0};
    CombatForm form {CombatForm::None};
    int formBonus {0};
    int deflectFeatBonus {0};
    FeatType shooterFeat {FeatType::Invalid};
    int shooterFeatPenalty {0};
};

struct PhysicalDamageBonus {
    int damageAbilityModifier {0};
    int strengthModifier {0};
    int weaponSpecialization {0};
    int combatFeatDamage {0};
    int preciseShotDamage {0};
    int formDamage {0};
    int furyDamage {0};
    // Two independent active unarmed feat families (209-211 and 212-219).
    // Rolled once per subattack, then reused by the critical accumulation.
    int unarmedDice209 {0};
    int unarmedDice212 {0};

    int total() const {
        return damageAbilityModifier + weaponSpecialization +
               combatFeatDamage + preciseShotDamage + formDamage + furyDamage;
    }
};

struct DamageBreakdown {
    DamageBreakdown();

    void addRawDamage(int amount, DamageType type);

    std::array<int, 14> rawDamageSlots;
    int strengthModifier {0};
    int otherSpecialBonus {0};
    int sneakAttack {0};
    int weaponSpecialization {0};
    int combatFeatDamage {0};
    int preciseShotDamage {0};
    int formDamage {0};
    int unarmedFeatDamage209 {0};
    int unarmedFeatDamage212 {0};
    int criticalMultiplier {0};
};

/**
 * Make and collect multiple attacks, but delay damage effects until later.
 */
class AttackBuffer {
public:
    enum class Source {
        Main,
        Offhand,
    };

    /**
     * Construct an ordered physical attack round from the attacker's equipped
     * weapons, active effects, and optional combat feat.
     */
    // Returns false when a synchronous effect callback retires/rebuilds this
    // buffer. Callers must not resume its animation or result publication.
    bool addPhysicalAttacks(Creature &attacker, Object &target,
                            FeatType feat = FeatType::Invalid);
    /** Every round of this buffer is a cutscene attack with these values. */
    void forceCutscene(const CutsceneAttack &cutscene) { _cutscene = cutscene; }
    const std::optional<CutsceneAttack> &cutscene() const { return _cutscene; }

    void resolve(Creature &attacker, Object &target);
    /**
     * Settle the target's reaction to this round on its attack records and
     * return the reaction animation id (10001 when there is none).
     */
    uint16_t resolveReaction(const Creature *target, bool engaged, bool targetEngaged,
                             bool targetHasRoom, bool forced);

    void saveContinuation(SavedPhysicalAction &state, const Game &game) const;
    void restoreContinuation(const SavedPhysicalAction &state);
    size_t attackCount() const { return _attacks.size(); }
    uint16_t currentCombatAttackType() const {
        if (_resolvingHistory) return _resolvingHistory->type;
        if (_roundHistory) return _roundHistory->type;
        return _attacks.empty() ? 0 : _attacks.front().history->type;
    }
    void prepareMeleeSequence(
        const IAnimations &animations,
        const std::vector<std::string> &attackAnimations);
    size_t signalReadyMelee(
        int elapsedMilliseconds,
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target);
    /**
     * Release the ranged discharges of this round whose time precedes
     * \p elapsedMilliseconds of the attack animation, at most one per call.
     * At the end of the physical attack pause, release all remaining ones.
     */
    size_t signalReadyRanged(
        int elapsedMilliseconds,
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target);
    void discardPending();
    void clearHistory();
    void clearSpecialAttacks();
    bool hasPendingMelee() const;
    bool hasPendingDischarges() const { return _nextDischarge < _discharges.size(); }

    /**
     * Get the best result for a series of attacks collected in AttackBuffer.
     */
    AttackResultType result() const;

    /** The result the round's record holds: that of the last attack resolved. */
    AttackResultType recordResult() const {
        return _attacks.empty() ? AttackResultType::Invalid : _attacks.back().result;
    }

    /**
     * Result held by the round's record: the last released discharge, or
     * before any release the last one resolved, unless a touch attack's
     * deflection has overwritten it since.
     */
    AttackResultType currentRangedResult() const {
        if (_roundFields) return static_cast<AttackResultType>(_roundFields->result);
        if (_discharges.empty()) return AttackResultType::Invalid;
        return _nextDischarge > 0 ? _discharges[_nextDischarge - 1].result : _discharges.back().result;
    }

    /**
     * The ranged record's attack score, the roll plus the modifier it last
     * took; none while the record is not a ranged one.
     */
    std::optional<int> rangedRecordScore() const {
        if (!_roundFields || !_roundFields->ranged) return std::nullopt;
        return _recordRoll + _recordModifier;
    }
    /** The record's weapon attack type: 1 for the right hand, 2 for the left. */
    uint8_t recordWeaponAttackType() const { return _roundFields ? _roundFields->weaponAttackType : 0; }
    /** The record's attack kills its target. */
    bool recordKillingBlow() const { return _roundFields && _roundFields->killingBlow != 0; }
    void setRecordResult(AttackResultType result) {
        if (_roundFields) _roundFields->result = static_cast<uint8_t>(result);
    }
    /** The record's last deflection roll. */
    DeflectionBreakdown &recordDeflection() { return _recordDeflection; }

private:
    struct CriticalThreatBreakdown {
        int threshold {0};
        bool threatened {false};
        int confirmationRoll {0};
        int confirmationBonus {0};
        bool confirmed {false};
        int multiplier {0};
    };

    /**
     * One weapon discharge of a ranged round. A hit shot carries a logical
     * attack; the other discharges are presentation-only misses, which a
     * target may still parry or catch on a force shield.
     */
    struct Discharge {
        int timeMilliseconds {0};
        int hand {0};
        int attack {-1};
        AttackResultType result {AttackResultType::Miss};
        glm::vec3 endpoint {0.0f};
        int delayMilliseconds {0};
        // Record snapshot taken when the discharge was resolved.
        uint16_t attackType {0};
        uint8_t weaponAttackType {0};
        int recordRoll {0};
        int recordModifier {0};
    };

    struct Attack {
        Attack(
            Source source,
            bool ranged,
            AttackBonusBreakdown attackBonusBreakdown,
            DamagePacket damage = DamagePacket()) :
            source(source),
            ranged(ranged),
            attackBonusBreakdown(std::move(attackBonusBreakdown)),
            damage(std::move(damage)) {}

        std::shared_ptr<AttackHistory> history {std::make_shared<AttackHistory>()};
        std::shared_ptr<AttackEventFields> eventFields {std::make_shared<AttackEventFields>()};
        Source source;
        bool ranged;
        PhysicalAttackKind kind {PhysicalAttackKind::MainHand};
        AttackResultType result {AttackResultType::Invalid};
        int roll {0};
        AttackBonusBreakdown attackBonusBreakdown;
        DefenseBreakdown defenseBreakdown;
        bool naturalTwenty {false};
        bool naturalOne {false};
        bool coupDeGrace {false};
        CriticalThreatBreakdown criticalThreat;
        DamageBreakdown damageBreakdown;
        int impactTimeMilliseconds {0};
        // The roll hit, or was parried, deflected or shielded, before any
        // interception of a miss.
        bool rolledHitLike {false};
        bool signaled {false};
        RuntimeObjectRef<Item> sourceItem;
        RuntimeObjectRef<Object> sourceActor;
        std::vector<DeferredCombatFeedback> deferredFeedback;
        std::vector<ItemOnHitApplication> onHitApplications;
        bool onHitResolved {false};
        DamagePacket damage;
        // Ordered target work: passive, selected post-roll, then post-damage
        // effects. Item on-hit applications retain their separate owner and
        // take effect before the target effects from effectsAfterOnHit on.
        std::vector<EffectInstance> targetEffects;
        size_t effectsAfterOnHit {std::numeric_limits<size_t>::max()};
        // The last saving throw a feat or passive rolled, the effect code a
        // failed feat save records, and whether the hit reports the outcome.
        EffectOutcomeBreakdown saveRecord;
        int effectCode {0};
        bool reportsEffectOutcome {false};
    };

    bool addPhysicalAttack(
        Creature &attacker,
        Object &target,
        const Item *weapon,
        Source source,
        PhysicalAttackKind kind,
        size_t logicalIndex,
        FeatType feat,
        const Item *dischargeWeapon);
    bool addRangedAttacks(
        Creature &attacker,
        Object &target,
        const std::shared_ptr<Item> &main,
        const std::shared_ptr<Item> &offhand,
        int mainHandAttacks);
    bool resolvePostRoll(Attack &attack, size_t logicalIndex,
                         Creature &attacker, Object &target);
    /** Whether the record \p attack reads carries the sneak attack marker: the round's record when there is one. */
    bool recordSneakAttack(const Attack &attack) const;
    void resolveUnarmedPassive(Attack &attack, Creature &attacker, Creature &target);
    void resolveEchaniKnockdown(Attack &attack, Creature &attacker, Creature &target, bool special);
    SavingThrowResult rollDeferredSave(Attack &attack, const Creature &attacker, Creature &target, int dc);
    void appendHitVisuals(const Creature &attacker, Attack &attack);
    void reportEffectOutcome(Game &game, ServicesView &services, const Object &target, Attack &attack);
    void resolveAssassinatePassive(Attack &attack, Creature &attacker, Creature &target);
    void appendTargetEffect(Attack &attack, std::shared_ptr<Effect> effect,
                            DurationType durationType, float duration);
    bool resolveDamage(const Creature &attacker, Object &target, Attack &attack);
    void resolveItemOnHitProperties(
        const Creature &attacker, Object &target, Attack &attack);
    void signalAttack(
        Attack &attack,
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target);
    void signalDischarge(
        const Discharge &discharge,
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target);
    void returnDeflected(
        Attack &attack,
        Game &game,
        Creature &attacker,
        Creature &deflector,
        uint64_t impactTime);
    void releaseTargetWork(
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target,
        uint64_t impactTime);
    // The round's feedback leaves with its first release: a ranged round's
    // first discharge, a melee round's first hit.
    std::vector<DeferredCombatFeedback> takeDeferredFeedback();
    void releaseDeferredFeedback(
        Game &game,
        ServicesView &services,
        Creature &attacker,
        Object &target,
        uint64_t impactTime,
        std::vector<DeferredCombatFeedback> feedback);
    void publishAttacked(
        const Attack &attack,
        const std::shared_ptr<AttackHistory> &history,
        const std::shared_ptr<AttackEventFields> &fields,
        Game &game,
        const Creature &attacker,
        const Object &target,
        uint64_t when);
    void applyDamage(
        Attack &attack,
        Creature &attacker,
        Object &target,
        Game &game,
        uint64_t impactTime);
    void applyEffects(
        Attack &attack,
        Creature &attacker,
        Object &target,
        Game &game,
        ServicesView &services,
        uint64_t impactTime,
        std::vector<ItemOnHitApplication> onHit);
    void addCombatFeedback(
        Game &game,
        ServicesView &services,
        const Creature &attacker,
        const Object &target,
        const Attack &attack) const;
    void addDeflectionFeedback(
        Game &game,
        ServicesView &services,
        const Creature &attacker,
        const Object &target,
        const Attack &attack) const;

    SmallVector<Attack, 8> _attacks;
    // Ranged rounds release their discharges in order; undischarged ones are
    // dropped when the round is cancelled.
    SmallVector<Discharge, 12> _discharges;
    size_t _nextDischarge {0};
    // The record's roll and signed-byte modifier, and its last deflection
    // roll. They are not saved.
    int _recordRoll {0};
    int _recordModifier {0};
    DeflectionBreakdown _recordDeflection;
    std::optional<CutsceneAttack> _cutscene;
    // TSL times a cutscene hit by an impact row that does not exist.
    bool _cutsceneHitsAtRelease {false};
    // The first hit-like discharge releases the round's target effects and
    // item on-hit work, whichever shot carried them.
    bool _targetWorkReleased {false};
    // The round's one attack record, shared by its queued events.
    std::shared_ptr<AttackHistory> _roundHistory;
    std::shared_ptr<AttackEventFields> _roundFields;
    FeatType _feat {FeatType::Invalid};
    // Current record while the action-owned buffer is resolving. This is not
    // another attack/history snapshot or serialized state.
    std::shared_ptr<AttackHistory> _resolvingHistory;
    size_t _pendingMeleeAttacks {0};
    bool _meleeSequencePrepared {false};
    // Invalidates a ready list if a synchronous callback cancels or rebuilds it.
    uint64_t _signalGeneration {0};
};

class AttackSchedule {
public:
    enum State {
        WaitAttack,
        Attack,
        WaitDamage,
        Damage,
        WaitFinish,
        Finish,
    };

    State update(const CombatRound &round, Action &action, float dt);
    void startMelee();
    void startRanged();
    void skipAttacks() { _state = WaitFinish; }
    void saveContinuation(resource::Gff &state) const;
    void restoreContinuation(const resource::Gff &state);

    // The attack holds its round until its pause ends and its hits are out.
    bool holdsCombatRound() const { return _state < Damage; }
    bool started() const { return _state != WaitAttack; }
    bool isMelee() const { return _melee; }
    bool isRanged() const { return _ranged; }
    int elapsedMilliseconds() const { return _elapsedMilliseconds; }

private:
    void startClock(int completionMilliseconds);
    bool clocked() const { return _melee || _ranged; }

    State _state {WaitAttack};
    float _time {0.0f};
    bool _melee {false};
    bool _ranged {false};
    int _elapsedMilliseconds {0};
    int _completionMilliseconds {0};
    float _elapsedRemainderMilliseconds {0.0f};
};

/** How far an attack has come in closing on its target. */
struct AttackApproach {
    // The attacker has reached its target, or leapt at it.
    bool reached {false};
    // The Force Jump the attack became by leaping at its target.
    FeatType forceJump {FeatType::Invalid};
    // Having turned on another enemy for want of sight, the attack no longer
    // closes on its target.
    bool holdsGround {false};
    // The attack is taken up where the attacker stands, but without a swing.
    bool withoutSwing {false};
};

enum class AttackApproachStep {
    Approaching,
    Reached,
    Ended
};

/**
 * One update of an attack closing on its target, until it has reached it.
 * A door or placeable is attacked at the attacker's use point for it, and
 * every distance and the sight line below are measured to that point. The
 * target is reached when it is in the attacker's area, within the attacker's
 * maximum attack range (a tenth of a metre more in TSL), in sight, and, for an
 * attacker outside the party, not nearer than the desired distance allows. In
 * TSL a door is attacked from its stand-off point, twice the desired distance
 * out from the door beyond its use point, and is reached only within the
 * desired distance of that point. Sight is a clear line from just above the
 * attacker's eyes to just below the target's, past nothing but the two of
 * them. An attacker out of sight remembers where it stood; the controlled
 * creature, finding itself out of sight again from the same spot, says so and
 * turns on the nearest enemy it can find, or, with none, drops its combat
 * modes and ends the attack. An attack that has turned so ends, with the
 * attacker's combat modes, whenever its target is out of reach or sight, as
 * does an attack on a target in no area that is not a creature, or by a
 * creature an encounter spawned. In TSL an attacker that cannot move takes up
 * its attack on a creature from where it stands, and swings only at one within
 * reach. Otherwise an attacker that \p mayLeap tries its Force Jump at a target in
 * sight; one too near walks back to the desired distance, or runs to a door's
 * stand-off point from farther than the square root of three metres; a TSL
 * party member set to fight at range or to stand its ground, unless
 * controlled, only turns toward the target; one farther than its closing
 * distance (its reach with a ranged weapon, the desired distance in melee,
 * and for a door or placeable the attacker's use range for it, a door
 * counting as the target of the attacker's own path) closes on the target
 * until the target is within the attacker's use range for it lengthened to
 * the maximum attack range, in a clear line; and one already that near turns
 * toward the target and waits for sight. A creature standing on the way back
 * to the desired distance cuts it short as a wall does.
 */
AttackApproachStep approachAttackTarget(Creature &attacker, Object &target, float dt, AttackApproach &approach,
                                        Game &game, const Action &parent, bool mayLeap);

/**
 * An attacker trained in Force Jump and holding a lightsaber leaps at a
 * creature in its sight ten metres away or more, with no one but the target
 * on the straight line to it, instead of closing on it: the Force jump is
 * applied to the attacker, and the attack becomes the highest Force Jump the
 * attacker knows, which is returned. Returns FeatType::Invalid, applying
 * nothing, when the attacker does not leap.
 */
FeatType resolveForceJumpAttack(Creature &attacker, Object &target);

bool isCreatureCombat(
    const Creature &attacker,
    const Object &target);

/**
 * The swing of a physical attack round as it is presented: one clip, however
 * many attacks the round holds.
 */
struct PhysicalAttackSwing {
    // The animation that times each logical melee attack's hit, or the one
    // ranged animation.
    std::vector<std::string> animations;
    // The clip the attacker shows and the target's reaction is keyed by.
    std::string clip;
    // The target is engaged in the exchange.
    bool engaged {false};
    // The exchange uses the engaged placeholder rather than the generic one.
    bool engagedPlaceholder {false};
    bool ranged {false};
    // The feat the swing shows, or 0 for a plain one.
    uint16_t attackType {0};
};

/**
 * The visual an improved or master special attack, or a sniper shot, shows on
 * the attacker's weapons as its swing starts: on the right-hand weapon, and on
 * the left too with two blades or two pistols. Each weapon carries one; the
 * next replaces it, and it goes once its impact animation has played.
 */
class SpecialAttackVisuals {
public:
    ~SpecialAttackVisuals();

    void show(Creature &attacker, uint16_t attackType, bool ranged);
    void clear();
    void update();

    /**
     * A rebuilt body takes back each weapon's visual: it comes off the old
     * weapon model and goes back to the same node of the new one, carrying on
     * where it was. A visual whose weapon has left the hand, or whose node the
     * new weapon model lacks, goes.
     */
    void detachFromBody();
    void reattachToBody(const Creature &attacker, scene::ModelSceneNode &body);

private:
    struct Attached {
        // The weapon that carries the visual.
        std::weak_ptr<Item> weapon;
        bool ranged {false};
        scene::ModelNodeSceneNode *hook {nullptr};
        std::shared_ptr<scene::ModelSceneNode> model;
    };

    static void detach(Attached &attached);

    // The right hand, then the left.
    std::array<Attached, 2> _hands;
};

/**
 * Engage the target (unless \p engages is false), roll a physical attack
 * round and choose the attacker's swing. Returns nothing when a callback
 * retired the buffer, and no animations when a melee round has no attacks or
 * the attack is taken up without a swing, which rolls nothing.
 */
std::optional<PhysicalAttackSwing> beginPhysicalAttack(
    Creature &attacker, Object &target,
    const IAnimations &animations, AttackBuffer &attacks,
    FeatType feat = FeatType::Invalid, bool engages = true, bool swings = true);

/**
 * Settle the target's reaction to a resolved round, then queue the attacker's
 * swing and the target's reaction.
 */
void presentPhysicalAttack(
    Creature &attacker, Object &target,
    const IAnimations &animations, AttackBuffer &attacks,
    const PhysicalAttackSwing &swing);

/**
 * The target's reaction to a physical attack: melee by the round's result,
 * ranged a dodge only when no roll hit. \p committed tells whether it takes
 * the target's time; an uncommitted melee reaction is none.
 */
uint16_t resolveAttackReaction(const Creature *target, bool ranged, AttackResultType result,
                               bool anyHit, bool engaged, bool targetEngaged,
                               bool targetHasRoom, bool forced, bool &committed);

/**
 * An engaged exchange uses the engaged placeholder when a creature model takes
 * part or the pair is choreographed; everything else is generic.
 */
bool usesEngagedAttackPlaceholder(const Creature &attacker, const Object &target, bool engaged);

/** Show a reaction on the target, keyed by the clip of the attacker's swing. */
void showAttackReaction(Creature &attacker, Object &target, const std::string &swing,
                        uint16_t reaction, bool engagedPlaceholder, bool ranged,
                        const IAnimations &animations);

} // namespace game

} // namespace reone
