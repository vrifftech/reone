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

#include "reone/game/attack.h"
#include "reone/game/combatfeedback.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/death.h"
#include "reone/game/effect/destroyshields.h"
#include "reone/game/effect/poison.h"
#include "reone/game/action/movetoobject.h"

#include "reone/game/animations.h"
#include "reone/game/d20/feats.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/assassinate.h"
#include "reone/game/effect/movementspeeddecrease.h"
#include "reone/game/effect/immunity.h"
#include "reone/game/effect/forcejump.h"
#include "reone/game/effect/forcepushed.h"
#include "reone/game/effect/stunned.h"
#include "reone/game/game.h"
#include "reone/game/effect/visual.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/object/placeable.h"
#include "reone/game/projectiles.h"
#include "reone/game/shaperules.h"
#include "reone/game/visualeffects.h"
#include "reone/graphics/animation.h"
#include "reone/scene/collision.h"
#include "reone/scene/graph.h"
#include "reone/system/arrayref.h"
#include "reone/system/randomutil.h"

#include "physicalcombatrules.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <limits>
#include <stdexcept>

#include <boost/algorithm/string.hpp>

namespace reone {

namespace game {

static constexpr int kUnarmedCriticalThreat = 1;
static constexpr float kSpecialAttackDefensePenaltyDuration = 3.0f;
static constexpr float kCriticalStrikeStunDuration = 6.0f;
static constexpr float kPowerAttackKnockdownDuration = 0.1f;
// Standard physical attack actions request a 1500-ms pause.
static constexpr int kPhysicalAttackPauseMilliseconds = 1500;
// A Force jump needs the target ten metres away or more, and the attacker's
// eye line to it clear from a metre and a half above each of them.
static constexpr float kForceJumpMinimumDistance2 = 100.0f;
static constexpr float kAttackEyeHeight = 1.5f;
static constexpr float kAttackEyeOffset = 0.01f;
static constexpr int kNoAttackSightStrRef = 1516;
static constexpr int kNoAttackSightStrRefTSL = 1435;
// Reach is measured with a tenth of a metre to spare in TSL.
static constexpr float kAttackReachAllowance = 0.1f;
// An attacker outside the party stands off unless the square of its distance
// is at least the square of the desired distance less this.
static constexpr float kAttackStandOffAllowance = 0.2f;
// Square of the distance beyond which a party member says it moves to attack.
static constexpr float kMovingToAttackMessageDistance2 = 0.25f;
static constexpr int kMovingToAttackCombatMessage = 42477;
// Square of the distance beyond which an attacker runs to a door's stand-off
// point.
static constexpr float kDoorStandOffRunDistance2 = 3.0f;
// The turret that takes up attacks without ever swinging.
static constexpr const char *kNonSwingingTurretTag = "g_turretbl002";


bool isPhysicalAttackFeat(FeatType feat) {
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::ImprovedCriticalStrike:
    case FeatType::MasterCriticalStrike:
    case FeatType::Flurry:
    case FeatType::ImprovedFlurry:
    case FeatType::WhirlwindAttack:
    case FeatType::PowerAttack:
    case FeatType::ImprovedPowerAttack:
    case FeatType::MasterPowerAttack:
    case FeatType::RapidShot:
    case FeatType::ImprovedRapidShot:
    case FeatType::MultiShot:
    case FeatType::SniperShot:
    case FeatType::ImprovedSniperShot:
    case FeatType::MasterSniperShot:
    case FeatType::PowerBlast:
    case FeatType::ImprovedPowerBlast:
    case FeatType::MasterPowerBlast:
    case FeatType::ShieldBreaker:
        return true;
    default:
        return false;
    }
}

bool isAttackTargetDown(const Object &target) {
    if (target.isDead()) return true;
    const auto *creature = dyn_cast<const Creature>(&target);
    return creature && creature->isTemporarilyDead();
}

static int getBaseCriticalThreat(const Item *weapon) {
    return weapon ? weapon->criticalThreat() : kUnarmedCriticalThreat;
}

static int getCriticalThreat(const Item *weapon, int threatBonus) {
    int threat = getBaseCriticalThreat(weapon);
    if (weapon && weapon->hasActiveProperty(ItemProperty::Keen)) {
        threat *= 2;
    }
    return threat + threatBonus;
}

struct AttackResolution {
    AttackResultType result {AttackResultType::Invalid};
    int roll {0};
    DefenseBreakdown defenseBreakdown;
    bool naturalTwenty {false};
    bool naturalOne {false};
    bool coupDeGrace {false};
    int criticalThreatThreshold {0};
    bool criticalThreatened {false};
    int criticalConfirmationRoll {0};
    int criticalConfirmationBonus {0};
    bool criticalConfirmed {false};
    // A confirmed critical hit the target's immunity turned into an ordinary hit.
    bool criticalHitImmune {false};
};

static AttackResolution computeAttack(
    const Creature &attacker,
    const Object &target,
    int attackBonus,
    int criticalThreat,
    int damageFlags,
    bool ranged,
    std::optional<AttackResultType> forcedResult,
    DeflectionBreakdown &deflection) {

    AttackResolution resolution;

    // Determine defense of a target
    const auto *targetCreature = dyn_cast<Creature>(&target);
    if (targetCreature) {
        resolution.defenseBreakdown = targetCreature->getDefenseBreakdown(
            &attacker,
            damageFlags);
    }
    int defense = resolution.defenseBreakdown.total;

    // Consume the d20 before applying Coup de Grace's automatic result.
    resolution.roll = randomInt(1, 20);

    // Only a KotOR melee attack looks for a helpless target to finish off. The
    // attack is marked before its result is decided, a cutscene attack's too.
    resolution.coupDeGrace = !attacker.game().isTSL() &&
                             !ranged &&
                             targetCreature &&
                             targetCreature->isDebilitated() &&
                             targetCreature->attributes().getAggregateLevel() <= 4 &&
                             !targetCreature->isPartyMember();

    // A cutscene attack takes its result as given once the target's defense
    // is known; it keeps no roll and has no critical threat.
    if (forcedResult) {
        resolution.result = *forcedResult;
        resolution.roll = 0;
        return resolution;
    }

    if (resolution.coupDeGrace) {
        resolution.result = AttackResultType::AutomaticHit;
        resolution.roll = 20;
        return resolution;
    }

    if (attacker.hasAssuredHit()) {
        resolution.result = AttackResultType::HitSuccessful;
        debug(str(boost::format("computeAttack: assured hit: roll(%d)") % resolution.roll),
              LogChannel::Combat);
        return resolution;
    }

    if (ranged && targetCreature) {
        resolution.result = targetCreature->resolveRangedDefense(
            attacker, damageFlags, resolution.roll + attackBonus, deflection);
        if (resolution.result != AttackResultType::Invalid) return resolution;
    }

    if (resolution.roll == 1) {
        resolution.result = AttackResultType::Miss;
        resolution.naturalOne = true;
        debug(str(boost::format("computeAttack: miss: roll(1)")), LogChannel::Combat);
        return resolution;
    }

    if (resolution.roll != 20 &&
        (resolution.roll + attackBonus) < defense) {
        resolution.result = AttackResultType::Miss;
        debug(str(boost::format("computeAttack: miss: roll(%d), bonus(%d), defense(%d)") %
                  resolution.roll % attackBonus % defense),
              LogChannel::Combat);
        return resolution;
    }

    resolution.naturalTwenty = resolution.roll == 20;

    auto rightHand = attacker.getEquippedItem(InventorySlots::rightWeapon);
    bool rightHandLightsaber = rightHand && rightHand->isLightsaber();

    // Critical threat. K2 applies Soresu/Ataru to the final threshold,
    // after weapon threat, Keen, and special-attack threat have been combined.
    resolution.criticalThreatThreshold = getCriticalThreatThreshold(
        attacker.game().isTSL(),
        rightHandLightsaber,
        attacker.currentForm(),
        21 - criticalThreat);
    if (resolution.roll >= resolution.criticalThreatThreshold) {
        resolution.criticalThreatened = true;

        // Critical confirmation. Juyo contributes to confirmation only, not to
        // the original attack roll or the stored d20 result.
        resolution.criticalConfirmationRoll = randomInt(1, 20);
        resolution.criticalConfirmationBonus = getCriticalConfirmationBonus(
            attacker.game().isTSL(),
            rightHandLightsaber,
            attacker.currentForm());
        if ((resolution.criticalConfirmationRoll +
             resolution.criticalConfirmationBonus +
             attackBonus) >= defense) {
            resolution.criticalConfirmed = true;

            bool criticalHitImmune =
                targetCreature && targetCreature->hasEffectImmunity(
                    ImmunityType::CriticalHit, &attacker);
            resolution.criticalHitImmune = criticalHitImmune;
            if (!criticalHitImmune) {
                resolution.result = AttackResultType::CriticalHit;
                debug(str(boost::format("computeAttack: critical hit: roll(%d), confirmation(%d),"
                                        " bonus(%d), defense(%d), critical threat(%d)") %
                          resolution.roll % resolution.criticalConfirmationRoll % attackBonus %
                          defense % criticalThreat),
                      LogChannel::Combat);
                return resolution;
            }
        }
    }

    resolution.result = AttackResultType::HitSuccessful;
    debug(str(boost::format("computeAttack: hit: roll(%d), bonus(%d), defense(%d),"
                            " critical threat(%d)") %
              resolution.roll % attackBonus % defense % criticalThreat),
          LogChannel::Combat);

    return resolution;
}

static int rollDamageDice(int numDice, int die) {
    int result = 0;
    for (int i = 0; i < numDice; ++i) {
        result += randomInt(1, die);
    }
    return result;
}

static int getResolvedCriticalMultiplier(
    const Creature &attacker,
    const Item *weapon,
    FeatType feat) {

    int baseMultiplier = weapon ? weapon->criticalHitMultiplier() : 2;
    auto rightHand = attacker.getEquippedItem(InventorySlots::rightWeapon);
    bool rightHandLightsaber = rightHand && rightHand->isLightsaber();
    bool tsl = attacker.game().isTSL();
    return baseMultiplier +
           getPowerAttackCriticalMultiplierBonus(tsl, feat) +
           getCriticalMultiplierBonus(
               tsl,
               rightHandLightsaber,
               attacker.currentForm());
}

static void computeWeaponDamage(
    const Creature &attacker, const Object &target, const Item &weapon,
    AttackBuffer::Source source, bool creatureWeapon, AttackResultType result,
    bool criticalConfirmed, int criticalMultiplier, int damageBonus, int sneakDice,
    DamagePacket &damage, DamageBreakdown &breakdown) {

    int multiplier = result == AttackResultType::CriticalHit
                         ? criticalMultiplier
                         : 1;
    bool offHand = source == AttackBuffer::Source::Offhand;
    PhysicalDamageBonus physicalBonus = attacker.getPhysicalDamageBonus(
        &weapon,
        offHand);

    int baseDamage = 0;
    int amount = multiplier * (
        damageBonus + physicalBonus.total());
    if (!weapon.hasActiveProperty(ItemProperty::NoDamage)) {
        // A creature weapon's dice come from its Monster Damage property.
        const auto dice = creatureWeapon ? weapon.monsterDamageDice()
                                         : std::make_pair(weapon.numDice(), weapon.dieToRoll());
        for (int multiple = 0; multiple < multiplier; ++multiple) {
            baseDamage += rollDamageDice(dice.first, dice.second);
        }
        baseDamage *= attacker.getPhysicalDamageAutoBalanceFactor();
    }
    amount += baseDamage;
    // The sneak attack dice join the base damage once, whatever the critical.
    const int sneakDamage = rollDamageDice(sneakDice, 6);
    amount += sneakDamage;
    breakdown.sneakAttack = sneakDamage;

    int massiveCriticalDamage = attacker.getMassiveCriticalDamage(
        &weapon, result == AttackResultType::CriticalHit);
    amount += massiveCriticalDamage;

    breakdown.strengthModifier = weapon.isRanged()
                                     ? 0
                                     : multiplier * physicalBonus.strengthModifier;
    breakdown.weaponSpecialization =
        multiplier * physicalBonus.weaponSpecialization;
    breakdown.combatFeatDamage = multiplier * physicalBonus.combatFeatDamage;
    breakdown.preciseShotDamage = multiplier * physicalBonus.preciseShotDamage;
    breakdown.formDamage = multiplier * physicalBonus.formDamage;
    breakdown.otherSpecialBonus =
        multiplier * (damageBonus + physicalBonus.furyDamage) +
        massiveCriticalDamage;
    breakdown.criticalMultiplier = criticalConfirmed
                                       ? criticalMultiplier
                                       : 0;

    DamageType type = getPrimaryDamageType(weapon.damageFlags());
    if (baseDamage > 0) {
        breakdown.addRawDamage(baseDamage, type);
    }
    damage.addPhysicalBase(amount, type);
    damage.setDamageFlags(weapon.damageFlags());
    damage.setBaseDamageAmounts(breakdown.rawDamageSlots);
    const auto *targetCreature = dyn_cast<Creature>(&target);
    attacker.addPhysicalDamageModifiers(
        damage,
        breakdown,
        targetCreature,
        &weapon,
        offHand,
        multiplier);
    damage.setPower(attacker.calculateDamagePower(
        targetCreature,
        &weapon,
        offHand));
    damage.setCutsDoors(weapon.cutsDoors());

    debug(str(boost::format("computeWeaponDamage: %s -> %s (%d)") % attacker.tag() % target.tag() % damage.total()),
          LogChannel::Combat);
}

static void computeUnarmedDamage(
    const Creature &attacker, const Object &target, PhysicalAttackKind kind,
    AttackResultType result, bool criticalConfirmed, int criticalMultiplier,
    int damageBonus, int sneakDice, DamagePacket &damage, DamageBreakdown &breakdown) {

    int multiplier = result == AttackResultType::CriticalHit
                         ? criticalMultiplier
                         : 1;
    PhysicalDamageBonus physicalBonus = attacker.getPhysicalDamageBonus(
        nullptr,
        false);
    // The unarmed feat dice belong to unarmed attacks only, not to an empty
    // left creature slot.
    if (kind != PhysicalAttackKind::Unarmed && kind != PhysicalAttackKind::ExtraUnarmed) {
        physicalBonus.unarmedDice209 = 0;
        physicalBonus.unarmedDice212 = 0;
    }

    auto featDamage = rollUnarmedFeatDamage(
        physicalBonus.unarmedDice209, physicalBonus.unarmedDice212,
        [](int die) { return randomInt(1, die); });
    int baseDamage = 0;
    int amount = multiplier * (
        damageBonus + physicalBonus.total() + featDamage.total());
    breakdown.unarmedFeatDamage209 = multiplier * featDamage.rank209;
    breakdown.unarmedFeatDamage212 = multiplier * featDamage.rank212;
    for (int multiple = 0; multiple < multiplier; ++multiple) {
        baseDamage += randomInt(1, getOrdinaryUnarmedDamageDie(
            attacker.game().isTSL(), attacker.size()));
    }
    baseDamage *= attacker.getPhysicalDamageAutoBalanceFactor();
    amount += baseDamage;
    const int sneakDamage = rollDamageDice(sneakDice, 6);
    amount += sneakDamage;
    breakdown.sneakAttack = sneakDamage;

    int massiveCriticalDamage = attacker.getMassiveCriticalDamage(
        nullptr, result == AttackResultType::CriticalHit);
    amount += massiveCriticalDamage;

    breakdown.strengthModifier =
        multiplier * physicalBonus.strengthModifier;
    breakdown.weaponSpecialization =
        multiplier * physicalBonus.weaponSpecialization;
    breakdown.combatFeatDamage = multiplier * physicalBonus.combatFeatDamage;
    breakdown.preciseShotDamage = multiplier * physicalBonus.preciseShotDamage;
    breakdown.formDamage = multiplier * physicalBonus.formDamage;
    breakdown.otherSpecialBonus =
        multiplier * (damageBonus + physicalBonus.furyDamage) +
        massiveCriticalDamage;
    breakdown.criticalMultiplier = criticalConfirmed
                                       ? criticalMultiplier
                                       : 0;

    if (baseDamage > 0) {
        breakdown.addRawDamage(baseDamage, DamageType::Bludgeoning);
    }
    damage.addPhysicalBase(amount, DamageType::Bludgeoning);
    damage.setDamageFlags(static_cast<int>(DamageType::Bludgeoning));
    damage.setBaseDamageAmounts(breakdown.rawDamageSlots);
    const auto *targetCreature = dyn_cast<Creature>(&target);
    attacker.addPhysicalDamageModifiers(
        damage,
        breakdown,
        targetCreature,
        nullptr,
        false,
        multiplier);
    damage.setPower(attacker.calculateDamagePower(
        targetCreature,
        nullptr,
        false));

    debug(str(boost::format("computeUnarmedDamage: %s -> %s (%d)") % attacker.tag() % target.tag() % damage.total()),
          LogChannel::Combat);
}

static bool grantsExtraMainHandAttack(FeatType feat) {
    switch (feat) {
    case FeatType::Flurry:
    case FeatType::ImprovedFlurry:
    case FeatType::WhirlwindAttack:
    case FeatType::RapidShot:
    case FeatType::ImprovedRapidShot:
    case FeatType::MultiShot:
        return true;
    default:
        return false;
    }
}

static int getSpecialAttackThreatBonus(
    FeatType feat,
    const Item *weapon) {

    int multiplier = 0;
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::SniperShot:
        multiplier = 1;
        break;
    case FeatType::ImprovedCriticalStrike:
    case FeatType::ImprovedSniperShot:
        multiplier = 2;
        break;
    case FeatType::MasterCriticalStrike:
    case FeatType::MasterSniperShot:
        multiplier = 3;
        break;
    default:
        break;
    }
    return multiplier * getBaseCriticalThreat(weapon);
}

bool AttackBuffer::addPhysicalAttacks(Creature &attacker, Object &target,
                                      FeatType feat) {
    const auto generation = ++_signalGeneration;
    _feat = feat;
    _discharges.clear();
    _nextDischarge = 0;
    _targetWorkReleased = false;
    _roundHistory.reset();
    _roundFields.reset();
    _recordRoll = 0;
    _recordModifier = 0;
    _recordDeflection = DeflectionBreakdown {};

    const bool tsl = attacker.game().isTSL();
    _cutsceneHitsAtRelease = _cutscene && tsl;
    // K2 caps the raw bonus at consumption, without a negative floor. K1's
    // effect callbacks have already saturated its stored value.
    const int bonusAttacks = tsl ? std::min(attacker.modifiedAttacks(), 2)
                                 : attacker.modifiedAttacks();
    int mainHandAttacks = 1 + bonusAttacks;
    if (grantsExtraMainHandAttack(feat)) ++mainHandAttacks;

    const auto main = attacker.getEquippedItem(InventorySlots::rightWeapon);
    if (grantsJuyoExtraOnHandAttack(tsl, main && main->isLightsaber(), attacker.currentForm()))
        ++mainHandAttacks;

    attacker.beginCombatAttack(target, feat);
    if (generation != _signalGeneration) return false;
    // A ranged main-hand weapon makes the round ranged, as it does for the attack action.
    if (main && main->isRanged()) {
        return addRangedAttacks(attacker, target, main, attacker.getOffhandAttackWeapon(), mainHandAttacks);
    }

    // A melee round also keeps one record, which each hit takes over when it is released.
    _roundHistory = std::make_shared<AttackHistory>();
    _roundHistory->type = static_cast<uint16_t>(feat);
    _roundFields = std::make_shared<AttackEventFields>();

    // Retain the first logical attack's type clear when constructing subsequent records.
    auto selectedFeat = [&]() {
        return _attacks.empty() ? feat : static_cast<FeatType>(_attacks.front().history->type);
    };
    size_t logicalIndex = 0;
    // With both hands empty, creature weapons replace the unarmed attacks. Every
    // attack uses the left creature weapon; the right and bite slots never attack.
    const bool creatureWeapons = !main && !attacker.getEquippedItem(InventorySlots::leftWeapon) &&
        (attacker.getEquippedItem(InventorySlots::cWeaponL) ||
         attacker.getEquippedItem(InventorySlots::cWeaponR) ||
         attacker.getEquippedItem(InventorySlots::cWeaponB));
    const auto creatureWeapon = creatureWeapons ? attacker.getEquippedItem(InventorySlots::cWeaponL)
                                                : std::shared_ptr<Item>();
    for (int i = 0; i < mainHandAttacks; ++i) {
        // Modified, feat, and form attacks are included in the on-hand count. They retain
        // the on-hand classification rather than becoming separate extra attacks.
        if (creatureWeapons) {
            const auto kind = creatureWeapon ? PhysicalAttackKind::CreatureWeapon : PhysicalAttackKind::None;
            if (!addPhysicalAttack(attacker, target, creatureWeapon.get(), Source::Main, kind,
                                   logicalIndex++, selectedFeat(), creatureWeapon.get())) return false;
            continue;
        }
        const auto kind = main ? PhysicalAttackKind::MainHand : PhysicalAttackKind::Unarmed;
        if (!addPhysicalAttack(attacker, target, main.get(), Source::Main, kind,
                               logicalIndex++, selectedFeat(), main.get())) return false;
    }
    // The represented unarmed round does not invent an offhand weapon attack.
    if (!main) return true;
    const auto offhand = attacker.getOffhandAttackWeapon();
    if (offhand && !addPhysicalAttack(attacker, target, offhand.get(), Source::Offhand,
                                     PhysicalAttackKind::Offhand, logicalIndex,
                                     selectedFeat(), offhand.get())) return false;
    return true;
}

void AttackBuffer::appendTargetEffect(Attack &attack, std::shared_ptr<Effect> effect,
                                      DurationType durationType, float duration) {
    effect->setSaveFacingCreator(attack.sourceActor.resolve());
    auto instance = effect->saveFacingInstance();
    instance.effect = std::move(effect);
    instance.setDuration(durationType, duration);
    attack.targetEffects.push_back(std::move(instance));
}

void AttackBuffer::resolveUnarmedPassive(Attack &attack, Creature &attacker, Creature &target) {
    if (attack.kind != PhysicalAttackKind::Unarmed &&
        attack.kind != PhysicalAttackKind::ExtraUnarmed) return;

    // This family (221-223) is distinct from the base-damage feat families.
    int rank = 0;
    for (int candidate = 3; candidate != 0; --candidate) {
        if (attacker.hasEffectiveFeat(static_cast<FeatType>(220 + candidate))) {
            rank = candidate;
            break;
        }
    }
    if (rank == 0 || !isAttackSuccessful(attack.result)) return;

    int amount = 0;
    for (int die = 0; die < rank; ++die) amount += randomInt(1, 6);
    appendTargetEffect(attack, DamageEffect::fromResolvedAttackAmount(amount, 7),
                       DurationType::Instant, 0.0f);

    // The damage is independent of the movement effect's immunity/proc/save.
    auto movement = attacker.game().newEffect<MovementSpeedDecreaseEffect>(50);
    movement->setSubType(kMagicalEffectCategory);
    movement->setSaveFacingCreator(attack.sourceActor.resolve());
    const int level = static_cast<uint8_t>(attacker.attributes().getAggregateLevel());
    if (target.isEffectLinkImmune(*movement)) return;
    const int proc = randomInt(0, 99);
    if ((rank == 1 && proc >= 10) || (rank == 2 && proc >= 15) ||
        (rank == 3 && proc > 19)) return;
    const int dc = static_cast<uint16_t>(level + 15);
    if (rollDeferredSave(attack, attacker, target, dc) == SavingThrowResult::Failed) {
        appendTargetEffect(attack, std::move(movement), DurationType::Temporary, 6.0f);
    }
}

// A feat or passive's saving throw is reported with the hit: its save line,
// and the record the hit's effect-application line reads.
SavingThrowResult AttackBuffer::rollDeferredSave(Attack &attack, const Creature &attacker, Creature &target, int dc) {
    const auto save = target.getSavingThrowBreakdown(SavingThrow::Fortitude, SavingThrowType::All, &attacker);
    const int roll = randomInt(1, 20);
    const int total = roll + save.total();
    const auto result = target.getSavingThrowResult(total, dc, SavingThrowType::All, &attacker);
    attack.deferredFeedback.emplace_back(
        SavingThrowFeedback {SavingThrow::Fortitude, save.base, save.modifier, roll, dc, SavingThrowType::All});
    auto &record = attack.saveRecord;
    record.saveType = static_cast<int>(SavingThrow::Fortitude);
    record.saveMode = 0;
    record.saveRoll = roll;
    record.modifierTotal = save.modifier;
    record.baseSave = save.base;
    record.finalTotal = total;
    record.difficultyClass = dc;
    record.outcome = static_cast<int>(result);
    return result;
}

// TSL Echani Strike I-III may knock down on the first unarmed hit, unless a
// feat already knocked down or stunned the target. The save comes before the
// chance.
void AttackBuffer::resolveEchaniKnockdown(Attack &attack, Creature &attacker, Creature &target, bool special) {
    if (!isAttackSuccessful(attack.result) || special ||
        (attack.kind != PhysicalAttackKind::Unarmed && attack.kind != PhysicalAttackKind::ExtraUnarmed)) return;
    int chance = 0;
    if (attacker.hasEffectiveFeat(static_cast<FeatType>(211))) chance = 20;
    else if (attacker.hasEffectiveFeat(static_cast<FeatType>(210))) chance = 15;
    else if (attacker.hasEffectiveFeat(static_cast<FeatType>(209))) chance = 10;
    if (chance == 0) return;
    auto effect = attacker.game().newEffect<ForcePushedEffect>();
    effect->setSaveFacingCreator(attack.sourceActor.resolve());
    const int level = static_cast<uint8_t>(attacker.attributes().getAggregateLevel());
    if (!target.isEffectLinkImmune(*effect) &&
        rollDeferredSave(attack, attacker, target, static_cast<uint16_t>(level + 15)) == SavingThrowResult::Failed &&
        randomInt(0, 99) < chance) {
        appendTargetEffect(attack, std::move(effect), DurationType::Temporary, kPowerAttackKnockdownDuration);
        attack.effectCode = 14;
    }
    attack.reportsEffectOutcome = true;
}

void AttackBuffer::resolveAssassinatePassive(Attack &attack, Creature &attacker, Creature &target) {
    if (!attacker.hasAssassinateWeaponPresentation()) return;
    int rank = 0;
    for (int candidate = 3; candidate != 0; --candidate) {
        if (attacker.hasEffectiveFeat(static_cast<FeatType>(227 + candidate))) {
            rank = candidate;
            break;
        }
    }
    if (rank == 0 || attack.result != AttackResultType::CriticalHit || randomInt(0, 99) > 19)
        return;
    auto effect = attacker.game().newEffect<AssassinateEffect>(rank);
    effect->setSaveFacingCreator(attack.sourceActor.resolve());
    const int level = static_cast<uint8_t>(attacker.attributes().getAggregateLevel());
    if (target.isEffectLinkImmune(*effect)) return;
    if (rollDeferredSave(attack, attacker, target, static_cast<uint16_t>(level + 10)) == SavingThrowResult::Failed) {
        appendTargetEffect(attack, std::move(effect), DurationType::Instant, 0.0f);
    }
}

bool AttackBuffer::resolvePostRoll(Attack &attack, size_t logicalIndex,
                                   Creature &attacker, Object &target) {
    if (logicalIndex != 0) return true;
    const auto generation = _signalGeneration;
    const bool tsl = attacker.game().isTSL();
    auto *targetCreature = dyn_cast<Creature>(&target);
    // Both K2 passive producers precede selected-type/use revalidation.
    if (tsl && targetCreature) {
        if (attack.ranged) resolveAssassinatePassive(attack, attacker, *targetCreature);
        else resolveUnarmedPassive(attack, attacker, *targetCreature);
    }
    if (generation != _signalGeneration) return false;

    // This is a post-roll type mutation, not spending, rejection or a reroll.
    // In TSL a plain attack (type 0) is never checked for feat uses; in KotOR it is.
    if ((!tsl || attack.history->type != 0) &&
        attacker.featRemainingUses(static_cast<FeatType>(attack.history->type)) == 0) {
        attack.history->type = 0;
        if (tsl && !attack.ranged && targetCreature) resolveEchaniKnockdown(attack, attacker, *targetCreature, false);
        return generation == _signalGeneration;
    }
    const auto feat = static_cast<FeatType>(attack.history->type);
    const int penalty = attack.ranged ? getRangedSpecialAttackDefensePenalty(feat)
                                      : getMeleeSpecialAttackDefensePenalty(tsl, feat);
    if (penalty != 0) {
        auto effect = attacker.game().newEffect<ACDecreaseEffect>(penalty, ACBonus::Dodge,
                                                                 kPhysicalDamageTypeFlags);
        effect->setSaveFacingCreator(attack.sourceActor.resolve());
        attacker.applyEffect(std::move(effect), DurationType::Temporary,
                             kSpecialAttackDefensePenaltyDuration);
        // Cancellation/rebuild can occur inside the effect application. The
        // local attack survives, but it must not resume the retired buffer.
        if (generation != _signalGeneration) return false;
    }
    if (!targetCreature) return true;

    if (attack.ranged) {
        const int level = static_cast<uint8_t>(attacker.attributes().getAggregateLevel());
        if (isRangedSniperFeat(feat) && isAttackSuccessful(attack.result)) {
            // Sniper reads the signed INT modifier, not the DEX modifier.
            const int rawIntelligence = static_cast<uint8_t>(
                attacker.getEffectiveAbilityModifier(Ability::Intelligence));
            const int intelligence = rawIntelligence < 128 ? rawIntelligence : rawIntelligence - 256;
            const int dc = static_cast<uint16_t>(level + intelligence);
            auto effect = attacker.game().newEffect<StunnedEffect>();
            effect->setSaveFacingCreator(attack.sourceActor.resolve());
            if (!targetCreature->isEffectLinkImmune(*effect) &&
                rollDeferredSave(attack, attacker, *targetCreature, dc) == SavingThrowResult::Failed) {
                appendTargetEffect(attack, std::move(effect), DurationType::Temporary,
                                   kCriticalStrikeStunDuration);
                attack.effectCode = 4;
            }
            attack.reportsEffectOutcome = true;
        } else if (tsl && isRangedPowerBlastFeat(feat) && attack.result == AttackResultType::CriticalHit) {
            const int rawDexterity = static_cast<uint8_t>(
                attacker.getEffectiveAbilityModifier(Ability::Dexterity));
            int dexterity = rawDexterity < 128 ? rawDexterity : rawDexterity - 256;
            // Do not apply armor limiting to this Dexterity modifier.
            // A debilitated actor's positive modifier is still limited to zero.
            if (attacker.isDebilitated(true)) dexterity = std::min(dexterity, 0);
            const int dc = static_cast<uint16_t>(level + 2 * dexterity);
            auto effect = attacker.game().newEffect<ForcePushedEffect>();
            effect->setSaveFacingCreator(attack.sourceActor.resolve());
            if (!targetCreature->isEffectLinkImmune(*effect) &&
                rollDeferredSave(attack, attacker, *targetCreature, dc) == SavingThrowResult::Failed) {
                appendTargetEffect(attack, std::move(effect), DurationType::Temporary,
                                   kPowerAttackKnockdownDuration);
                attack.effectCode = 14;
            }
            attack.reportsEffectOutcome = true;
        }
        return generation == _signalGeneration;
    }

    // Retain the existing selected-melee contracts, now at the same post-roll
    // boundary as ranged and ordinary attacks instead of an action-only tail.
    const int level = attacker.attributes().getAggregateLevel();
    const int strength = attacker.getEffectiveAbilityModifier(Ability::Strength);
    // A knockdown or stun from the feat keeps Echani Strike from knocking down.
    bool special = false;
    if (shouldAttemptPowerAttackKnockdown(tsl, feat, attack.result)) {
        const int dc = getMeleeSpecialAttackSaveDC(tsl, feat, level, strength);
        auto effect = attacker.game().newEffect<ForcePushedEffect>();
        effect->setSaveFacingCreator(attack.sourceActor.resolve());
        if (!targetCreature->isEffectLinkImmune(*effect) &&
            rollDeferredSave(attack, attacker, *targetCreature, dc) == SavingThrowResult::Failed) {
            appendTargetEffect(attack, std::move(effect), DurationType::Temporary,
                               kPowerAttackKnockdownDuration);
            attack.effectCode = 14;
            special = true;
        }
        attack.reportsEffectOutcome = true;
    } else if (shouldAttemptCriticalStrikeStun(feat, attack.result)) {
        // Stun immunity is decided when the stun is applied, after the save.
        const int dc = getMeleeSpecialAttackSaveDC(tsl, feat, level, strength);
        auto effect = attacker.game().newEffect<StunnedEffect>();
        effect->setSaveFacingCreator(attack.sourceActor.resolve());
        if (!targetCreature->isEffectLinkImmune(*effect) &&
            rollDeferredSave(attack, attacker, *targetCreature, dc) == SavingThrowResult::Failed) {
            appendTargetEffect(attack, std::move(effect), DurationType::Temporary,
                               kCriticalStrikeStunDuration);
            attack.effectCode = 4;
            special = true;
        }
        attack.reportsEffectOutcome = true;
    } else if (tsl && feat == FeatType::ShieldBreaker && isAttackSuccessful(attack.result)) {
        // Shield Breaker shows its visual and strips the target's Force shields.
        auto visual = attacker.game().newEffect<VisualEffect>(8001, false, attacker.services());
        visual->setSubType(kMagicalEffectCategory);
        appendTargetEffect(attack, std::move(visual), DurationType::Instant, 0.0f);
        appendTargetEffect(attack, attacker.game().newEffect<DestroyShieldsEffect>(), DurationType::Instant, 0.0f);
    }
    if (tsl) resolveEchaniKnockdown(attack, attacker, *targetCreature, special);
    return generation == _signalGeneration;
}

// nDamage takes the slot of the damage flag and the base amount; the slot
// holds no negative amount.
static void setCutsceneDamage(DamagePacket &damage, int amount, int damageFlags) {
    std::array<int, 14> slots;
    slots.fill(-1);
    const auto slot = getScriptDamageSlot(damageFlags);
    if (slot < slots.size()) slots[slot] = narrowAttackDamage(std::max(0, amount));
    damage.setDamageFlags(damageFlags);
    damage.setBaseDamageAmounts(slots);
    damage.addPhysicalBase(amount, getPrimaryDamageType(damageFlags));
}

// The damage a hit deals: the amounts of its typed slots once resolved, or
// the packet as it stands while a returned bolt waits to meet its shooter.
static int getHitDamageTotal(const DamagePacket &damage) {
    if (!damage.isResolved()) return damage.total();
    int total = 0;
    for (size_t slot = 0; slot < 14; ++slot) {
        const int amount = damage.resolution().damageAmounts[slot];
        if (amount >= 0) total += amount;
    }
    return total;
}

enum class SneakAttackOutcome {
    None,
    Marked,
    Immune
};

static constexpr float kSneakAttackRangedReach = 10.0f;
static constexpr double kSneakAttackFrontArcCosine = 0.707;

/**
 * Sneak attack marks the round's attack record; a marked record adds the
 * sneak attack dice to each damage roll of the round. A target caught
 * helpless or unaware is tested for immunity first; a flanked target is not.
 */
static SneakAttackOutcome resolveSneakAttack(const Creature &attacker, const Creature &target, bool ranged) {
    if (attacker.sneakAttackDice() == 0) return SneakAttackOutcome::None;

    const float distance = ranged ? glm::distance(attacker.position(), target.position()) : 0.0f;
    // A helpless target, meditation included, is caught.
    bool caught = target.isDebilitated(true);
    if (!caught && !target.perception().sees(attacker.id())) {
        caught = !ranged || distance <= kSneakAttackRangedReach;
    }
    if (caught) {
        if (target.hasEffectImmunity(ImmunityType::SneakAttack, &attacker) ||
            target.hasEffectImmunity(ImmunityType::CriticalHit, &attacker)) {
            return SneakAttackOutcome::Immune;
        }
        return SneakAttackOutcome::Marked;
    }

    const glm::vec3 toAttacker = shape::normalize(attacker.position() - target.position());
    const float facing = target.getFacing();
    const glm::vec3 front = shape::normalize(glm::vec3(-std::sin(facing), std::cos(facing), 0.0f));
    const double cosine = static_cast<double>(glm::dot(toAttacker, front));
    if (cosine >= kSneakAttackFrontArcCosine) return SneakAttackOutcome::None;
    if (ranged && distance > kSneakAttackRangedReach) return SneakAttackOutcome::None;
    return SneakAttackOutcome::Marked;
}

bool AttackBuffer::recordSneakAttack(const Attack &attack) const {
    return _roundFields ? _roundFields->sneakAttack != 0
                        : attack.eventFields && attack.eventFields->sneakAttack != 0;
}

static std::optional<int> getItemOnHitEffectOutcomeType(
    ItemOnHitSubtype subtype) {

    switch (subtype) {
    case ItemOnHitSubtype::Confusion:
        return 1;
    case ItemOnHitSubtype::Fear:
        return 2;
    case ItemOnHitSubtype::Stun:
        return 4;
    case ItemOnHitSubtype::Paralyze:
        return 5;
    case ItemOnHitSubtype::Sleep:
        return 6;
    case ItemOnHitSubtype::Slow:
    case ItemOnHitSubtype::AbilityDrain:
    case ItemOnHitSubtype::ItemPoison:
    case ItemOnHitSubtype::SlayRG:
    case ItemOnHitSubtype::SlayAG:
    case ItemOnHitSubtype::InstantDeath:
    case ItemOnHitSubtype::Knockdown:
        return std::nullopt;
    }
    assert(false && "unsupported item on-hit subtype");
    return std::nullopt;
}

static bool hasItemOnHitImmunity(const Creature &target, ItemOnHitSubtype subtype,
                                const std::shared_ptr<Object> &source) {
    const auto *creator = dyn_cast<Creature>(source.get());
    switch (subtype) {
    case ItemOnHitSubtype::Sleep: return hasStateImmunity(target, CreatureState::Sleep, creator);
    case ItemOnHitSubtype::Stun: return hasStateImmunity(target, CreatureState::Stun, creator);
    case ItemOnHitSubtype::Paralyze: return hasStateImmunity(target, CreatureState::Paralysis, creator);
    case ItemOnHitSubtype::Confusion: return hasStateImmunity(target, CreatureState::Confusion, creator);
    case ItemOnHitSubtype::Fear: return hasStateImmunity(target, CreatureState::Fear, creator);
    case ItemOnHitSubtype::Slow: return hasSlowImmunity(target, creator);
    case ItemOnHitSubtype::AbilityDrain: return target.hasEffectImmunity(ImmunityType::AbilityDecrease, creator);
    case ItemOnHitSubtype::ItemPoison: return target.hasEffectImmunity(ImmunityType::Poison, creator);
    case ItemOnHitSubtype::InstantDeath: return target.hasEffectImmunity(ImmunityType::Death, creator);
    case ItemOnHitSubtype::Knockdown:
    case ItemOnHitSubtype::SlayRG: case ItemOnHitSubtype::SlayAG: return false;
    }
    return false;
}

static void applyStandalone(Object &target, const std::shared_ptr<Effect> &effect,
                            const std::shared_ptr<Object> &creator,
                            DurationType type, float duration = 0.0f) {
    effect->setSaveFacingCreator(creator);
    target.applyEffect(effect, type, duration);
}

static EffectInstance prepareOnHitEffect(Object &target, const std::shared_ptr<Effect> &effect,
                                        const std::shared_ptr<Object> &creator, float duration,
                                        DurationType type = DurationType::Temporary,
                                        uint16_t category = 0x18) {
    effect->setSaveFacingCreator(creator);
    auto record = effect->saveFacingInstance();
    record.effect = effect;
    record.id = target.game().allocateEffectId();
    record.setDuration(type, duration);
    record.subType = static_cast<uint16_t>((record.subType & ~uint16_t(0x18)) | (category & 0x18));
    record.exposed = 1;
    return record;
}

static void applyItemOnHitApplication(const ItemOnHitApplication &application, Object &target,
                                     Game &game, ServicesView &services) {
    auto *creature = dyn_cast<Creature>(&target);
    if (!creature || creature->isDead()) return;
    const auto creator = application.creator.resolve();
    StateApplicationResult result = StateApplicationResult::Rejected;
    CreatureState state = CreatureState::None;
    switch (application.subtype) {
    case ItemOnHitSubtype::Sleep: state = CreatureState::Sleep; break;
    case ItemOnHitSubtype::Stun: state = CreatureState::Stun; break;
    case ItemOnHitSubtype::Paralyze: state = CreatureState::Paralysis; break;
    case ItemOnHitSubtype::Confusion: state = CreatureState::Confusion; break;
    case ItemOnHitSubtype::Fear: state = CreatureState::Fear; break;
    case ItemOnHitSubtype::Slow: applySlowPackage(target, application.duration, creator); break;
    case ItemOnHitSubtype::AbilityDrain: {
        if (application.parameter < 0 || application.parameter > 5 ||
            hasItemOnHitImmunity(*creature, application.subtype, creator)) return;
        applyStandalone(target, std::make_shared<VisualEffectMarkerEffect>(91), creator, DurationType::Temporary, 30.0f);
        auto record = prepareOnHitEffect(target, std::make_shared<AbilityDecreaseEffect>(
            static_cast<Ability>(application.parameter), 1), creator, 30.0f);
        target.applyEffect(record);
        target.applyEffect(record.linkedChild(std::make_shared<EffectIconMarkerEffect>(28)));
        break;
    }
    case ItemOnHitSubtype::ItemPoison: {
        auto record = prepareOnHitEffect(target,
            std::make_shared<PoisonEffect>(static_cast<Poison>(application.parameter)),
            creator, 0.0f, DurationType::Permanent, 0x08);
        // The linked icon is submitted even if poison admission queues removal.
        // Both leaves share the ID, so that removal also removes the icon.
        target.applyEffect(record);
        target.applyEffect(record.linkedChild(std::make_shared<EffectIconMarkerEffect>(23)));
        break;
    }
    case ItemOnHitSubtype::SlayRG:
        // The record captured the after-save race qualification already.
        applyStandalone(target, std::make_shared<VisualEffectMarkerEffect>(50), creator, DurationType::Instant);
        applyStandalone(target, std::make_shared<DeathEffect>(false, true, false), creator, DurationType::Instant);
        break;
    case ItemOnHitSubtype::Knockdown:
        if (game.isTSL())
            applyStandalone(target, std::make_shared<ForcePushedEffect>(), creator,
                            DurationType::Temporary, 0.1f);
        break;
    case ItemOnHitSubtype::SlayAG:
        // No additional payload; selection still consumes chance and save checks.
        break;
    case ItemOnHitSubtype::InstantDeath:
        if (!hasItemOnHitImmunity(*creature, application.subtype, creator))
            applyStandalone(target, std::make_shared<DeathEffect>(false, true, false), creator, DurationType::Instant);
        break;
    }
    if (state != CreatureState::None) {
        result = applyStatePackage(target, state, application.duration, creator);
        // An immune target is told by the state itself.
        if (result == StateApplicationResult::Applied && application.emitEffectOutcome) {
            addAttackEffectOutcomeFeedback(game, services, creator, target, application.effectOutcome);
        }
    }
}

void applyItemOnHitApplications(std::vector<ItemOnHitApplication> applications, Object &target,
                               Game &game, ServicesView &services) {
    for (const auto &application : applications) applyItemOnHitApplication(application, target, game, services);
}

namespace {
// Keep the current-attack query pointed at this record only for its resolution.
// A nested rebuild owns its replacement pointer; the old scope cannot clear it.
struct ResolvingAttackScope {
    std::shared_ptr<AttackHistory> &current;
    std::shared_ptr<AttackHistory> record;
    ResolvingAttackScope(std::shared_ptr<AttackHistory> &current,
                         const std::shared_ptr<AttackHistory> &record) :
        current(current), record(record) { current = record; }
    ~ResolvingAttackScope() { if (current == record) current.reset(); }
};
}

bool AttackBuffer::addPhysicalAttack(
    Creature &attacker,
    Object &target,
    const Item *weapon,
    Source source,
    PhysicalAttackKind kind,
    size_t logicalIndex,
    FeatType feat,
    const Item *dischargeWeapon) {

    const auto generation = _signalGeneration;
    const bool offHand = source == Source::Offhand;
    const bool ranged = weapon && weapon->isRanged();
    const bool tsl = attacker.game().isTSL();
    const auto *targetCreature = dyn_cast<Creature>(&target);
    // A creature weapon threatens and multiplies as an empty hand does.
    const bool creatureWeapon = kind == PhysicalAttackKind::CreatureWeapon;
    const Item *handWeapon = creatureWeapon ? nullptr : weapon;
    const auto gloves = handWeapon ? std::shared_ptr<Item>() : attacker.getEquippedItem(InventorySlots::hands);
    const Item *criticalWeapon = handWeapon ? handWeapon : gloves.get();
    // With only the right or bite creature slot filled, no item attacks.
    const Item *attackItem = kind == PhysicalAttackKind::None ? nullptr : weapon ? weapon : gloves.get();
    AttackBonusBreakdown bonus = attacker.getAttackBonusBreakdown(targetCreature, weapon, offHand);
    attacker.addPartyLeadershipTerms(bonus, targetCreature);
    // Dispatch arithmetic by feat ID. Item-specific exceptions read the attacking weapon.
    bonus.featBonus = getMeleeSpecialAttackRollBonus(tsl, feat) +
        getRangedSpecialAttackRollBonus(tsl, feat,
            criticalWeapon && criticalWeapon->rapidShotBonus(),
            criticalWeapon && criticalWeapon->sniperBonus());
    const int attackRollBonus = bonus.total();
    const int threat = getCriticalThreat(criticalWeapon, getSpecialAttackThreatBonus(feat, criticalWeapon));
    const int damageFlags = weapon ? weapon->damageFlags() : static_cast<int>(DamageType::Bludgeoning);
    const auto forcedResult = _cutscene
        ? std::optional<AttackResultType>(static_cast<AttackResultType>(static_cast<uint8_t>(_cutscene->result)))
        : std::nullopt;
    const auto resolution = computeAttack(attacker, target, attackRollBonus, threat, damageFlags, ranged, forcedResult,
                                          _recordDeflection);
    // The roll takes the record's roll and modifier, as bytes; a cutscene
    // attack leaves both at zero but keeps its bonus terms for the breakdown.
    _recordRoll = resolution.roll;
    _recordModifier = _cutscene ? 0 : static_cast<int8_t>(attackRollBonus);

    Attack attack(source, ranged, std::move(bonus));
    attack.kind = kind;
    attack.history->type = feat == FeatType::Invalid ? 0 : static_cast<uint16_t>(feat);
    ResolvingAttackScope resolving(_resolvingHistory, attack.history);
    attack.sourceActor = attacker.game().getObjectById(attacker.id());
    for (const auto &[slot, item] : attacker.equipment()) {
        if (item.get() == attackItem) { attack.sourceItem = item; break; }
    }
    attack.result = resolution.result;
    attack.roll = resolution.roll;
    attack.defenseBreakdown = resolution.defenseBreakdown;
    attack.naturalTwenty = resolution.naturalTwenty;
    attack.naturalOne = resolution.naturalOne;
    attack.coupDeGrace = resolution.coupDeGrace;
    attack.criticalThreat.threshold = resolution.criticalThreatThreshold;
    attack.criticalThreat.threatened = resolution.criticalThreatened;
    attack.criticalThreat.confirmationRoll = resolution.criticalConfirmationRoll;
    attack.criticalThreat.confirmationBonus = resolution.criticalConfirmationBonus;
    attack.criticalThreat.confirmed = resolution.criticalConfirmed;
    if (targetCreature) {
        switch (resolveSneakAttack(attacker, *targetCreature, ranged)) {
        case SneakAttackOutcome::Marked:
            // The round's record keeps the marker until the record is cleared.
            attack.eventFields->sneakAttack = 1;
            if (_roundFields) _roundFields->sneakAttack = 1;
            break;
        case SneakAttackOutcome::Immune:
            attack.deferredFeedback.emplace_back(SneakAttackImmunityFeedback {});
            break;
        case SneakAttackOutcome::None:
            break;
        }
    }
    if (resolution.criticalHitImmune) attack.deferredFeedback.emplace_back(CriticalHitImmunityFeedback {});
    if (!resolvePostRoll(attack, logicalIndex, attacker, target)) return false;

    attack.rolledHitLike = isAttackSuccessful(attack.result) ||
        attack.result == AttackResultType::Parried ||
        attack.result == AttackResultType::Deflected ||
        attack.result == AttackResultType::ShieldHit;
    // Non-damaging ranged interception follows special/passive post-roll work.
    // It tests the weapon in the hand that fires the discharge.
    if (ranged && targetCreature &&
        (attack.result == AttackResultType::Miss || attack.result == AttackResultType::AttackResisted ||
         attack.result == AttackResultType::AttackFailed)) {
        const auto interception = targetCreature->resolveRangedMiss(attacker, dischargeWeapon);
        if (interception != AttackResultType::Invalid) attack.result = interception;
    }
    const auto resolvedFeat = static_cast<FeatType>(attack.history->type);
    if (resolution.criticalConfirmed)
        attack.criticalThreat.multiplier = getResolvedCriticalMultiplier(attacker, handWeapon, resolvedFeat);
    const int damageBonus = getMeleeSpecialAttackDamageBonus(tsl, resolvedFeat) +
                            getRangedSpecialAttackDamageBonus(tsl, resolvedFeat);
    if (_cutscene) {
        // The forced damage is dealt as it is, in the attacking item's damage
        // slot, by a hit (and by a returned bolt).
        if (isAttackSuccessful(attack.result) || (ranged && attack.result == AttackResultType::Deflected)) {
            setCutsceneDamage(attack.damage, _cutscene->damage,
                              attackItem ? attackItem->damageFlags() : static_cast<int>(DamageType::Bludgeoning));
            if (attack.result != AttackResultType::Deflected) attack.damage.resolveUnmitigated();
        }
    } else if ((isAttackSuccessful(attack.result) || attack.result == AttackResultType::Deflected) &&
        !(attacker.game().isConversationActive() && attacker.isPartyMember())) {
        const int sneakDice = recordSneakAttack(attack) ? attacker.sneakAttackDice() : 0;
        if (weapon) {
            computeWeaponDamage(attacker, target, *weapon, source, creatureWeapon, attack.result,
                resolution.criticalConfirmed, attack.criticalThreat.multiplier, damageBonus, sneakDice,
                attack.damage, attack.damageBreakdown);
        } else {
            computeUnarmedDamage(attacker, target, kind, attack.result, resolution.criticalConfirmed,
                attack.criticalThreat.multiplier, damageBonus, sneakDice, attack.damage, attack.damageBreakdown);
        }
    }
    if (generation != _signalGeneration) return false;
    // Pool exhaustion can remove linked defenses. Finish this hit before the
    // next attack samples the target's state; HP delivery remains deferred.
    if (!resolveDamage(attacker, target, attack)) return false;
    // A confirmed critical hit makes the attacker call out. A cutscene hit, a
    // hit on a plot target and a party member's hit during a conversation do not.
    if (!_cutscene && attack.result == AttackResultType::CriticalHit && !target.plotFlag() &&
        !(attacker.game().isConversationActive() && attacker.isPartyMember()) && attacker.isHeardByLeader())
        attacker.playSound(resource::SoundSetEntry::CriticalHit);
    // A ranged hit that deals no damage tells the attacker, once a round, that
    // its weapon cannot hurt the target.
    if (ranged && (isAttackSuccessful(attack.result) || attack.result == AttackResultType::Deflected) &&
        getHitDamageTotal(attack.damage) <= 0 && !attacker.weaponIneffectiveReported() &&
        !attacker.isWeaponEffective(&target, attack.kind == PhysicalAttackKind::Offhand)) {
        attack.deferredFeedback.emplace_back(WeaponIneffectiveFeedback {});
        attacker.setWeaponIneffectiveReported(true);
    }
    _attacks.emplace_back(std::move(attack));
    return true;
}

static bool isHitLikeRangedResult(AttackResultType result) {
    return isAttackSuccessful(result) ||
           result == AttackResultType::Parried ||
           result == AttackResultType::Deflected ||
           result == AttackResultType::ShieldHit;
}

// Flight time of a blaster bolt: 42 units per second, truncated to milliseconds.
static int projectileDelayMilliseconds(const glm::vec3 &from, const glm::vec3 &to) {
    return static_cast<int>(glm::distance(from, to) / 42.0f * 1000.0f);
}

/**
 * Where a bolt from \p shooter that misses \p target ends: aimed beside the
 * target, lower with distance, extended 1000 units and stopped by the first
 * line-of-sight obstruction. It scatters wider when the target's own current
 * record holds a shield hit, as it does for a bolt bouncing back to its shooter.
 */
static glm::vec3 projectileMissLocation(const Object &shooter, const Object &target, Game &game) {
    float spread = 0.40002f;
    float scale = 1.0f;
    if (const auto *creature = dyn_cast<Creature>(&target)) {
        spread = creature->personalSpace() * 0.6667f;
        if (creature->currentCombatAttackResult() == AttackResultType::ShieldHit) {
            const int first = randomInt(0, 2);
            const int second = randomInt(0, 2);
            scale = static_cast<float>(first + second + randomInt(0, 2));
        }
    }
    const glm::vec3 offset = shooter.position() - target.position();
    const float length = glm::length(offset);
    const glm::vec3 direction = length > 0.0f ? offset / length : glm::vec3(0.0f);
    glm::vec3 side(direction.y, -direction.x, 0.0f);
    if (randomInt(0, 1) != 0) side = -side;
    glm::vec3 aim = target.position() + side * (scale * spread);
    const float falloff = glm::dot(offset, offset) * 1.5f / 100.0f;
    const float wobble = static_cast<float>(randomInt(0, 999)) / 1000.0f * falloff * 0.75f;
    aim.z += 1.5f - falloff / 1.5f + wobble;
    const glm::vec3 origin = shooter.position() + glm::vec3(0.0f, 0.0f, 1.5f);
    const glm::vec3 toAim = aim - origin;
    const float aimLength = glm::length(toAim);
    const glm::vec3 end = origin + (aimLength > 0.0f ? toAim / aimLength : glm::vec3(0.0f)) * 1000.0f;
    auto module = game.module();
    auto area = module ? module->area() : nullptr;
    scene::Collision collision;
    if (area && area->graph().testLineOfSight(origin, end, collision)) return collision.intersection;
    return end;
}

bool AttackBuffer::addRangedAttacks(
    Creature &attacker,
    Object &target,
    const std::shared_ptr<Item> &main,
    const std::shared_ptr<Item> &offhand,
    int mainHandAttacks) {

    const auto generation = _signalGeneration;
    // A cutscene attack's animation is the discharge row.
    const auto spec = attacker.services().game.projectiles.discharge(
        _cutscene ? _cutscene->animation : rangedAttackAnimation(
            static_cast<uint16_t>(_feat),
            attacker.getWieldType(),
            attacker.modelType() == Creature::ModelType::Creature),
        attacker);
    // A ranged round keeps one attack record. Each discharge takes a snapshot
    // of it when resolved and writes the snapshot back when released.
    _roundHistory = std::make_shared<AttackHistory>();
    _roundHistory->type = static_cast<uint16_t>(_feat);
    _roundFields = std::make_shared<AttackEventFields>();
    _roundFields->ranged = 1;
    // An animation without discharges resolves no shot and no attack.
    if (!spec) return true;

    const auto *targetCreature = dyn_cast<Creature>(&target);
    const int onHandAttacks = std::max(0, mainHandAttacks);
    int hitShots = onHandAttacks + (offhand ? 1 : 0);
    size_t logicalIndex = 0;
    const int shots = static_cast<int>(spec->shots.size());
    for (int shotIndex = 0; shotIndex < shots; ++shotIndex) {
        const auto &shot = spec->shots[shotIndex];
        Discharge discharge;
        discharge.timeMilliseconds = shot.timeMilliseconds;
        discharge.hand = shot.hand;
        const auto handWeapon = shot.hand == 0 ? attacker.getEquippedItem(InventorySlots::rightWeapon)
                              : shot.hand == 1 ? attacker.getEquippedItem(InventorySlots::leftWeapon)
                                               : nullptr;
        // The round's attacks take random discharges; the rest are misses.
        if (randomInt(0, shots - shotIndex - 1) < hitShots) {
            --hitShots;
            const bool off = static_cast<int>(logicalIndex) >= onHandAttacks;
            const auto feat = _attacks.empty() ? _feat : static_cast<FeatType>(_attacks.front().history->type);
            if (!addPhysicalAttack(
                    attacker,
                    target,
                    off ? offhand.get() : main.get(),
                    off ? Source::Offhand : Source::Main,
                    off ? PhysicalAttackKind::Offhand : PhysicalAttackKind::MainHand,
                    logicalIndex,
                    feat,
                    handWeapon.get())) {
                return false;
            }
            discharge.attack = static_cast<int>(logicalIndex++);
            discharge.result = _attacks.back().result;
            // A hit shot sets the record's attack type and hand; miss discharges
            // inherit whatever the record holds.
            _roundHistory->type = _attacks.back().history->type;
            _roundFields->weaponAttackType = static_cast<uint8_t>(_attacks.back().kind);
        } else if (targetCreature) {
            const auto interception = targetCreature->resolveRangedMiss(attacker, handWeapon.get());
            if (interception != AttackResultType::Invalid) discharge.result = interception;
        }
        if (isHitLikeRangedResult(discharge.result)) {
            discharge.endpoint = target.position();
        } else {
            discharge.endpoint = projectileMissLocation(attacker, target, attacker.game());
        }
        discharge.delayMilliseconds = projectileDelayMilliseconds(attacker.position(), discharge.endpoint);
        discharge.attackType = _roundHistory->type;
        discharge.weaponAttackType = _roundFields->weaponAttackType;
        discharge.recordRoll = _recordRoll;
        discharge.recordModifier = _recordModifier;
        _roundFields->result = static_cast<uint8_t>(discharge.result);
        _roundFields->rangedTarget = {discharge.endpoint.x, discharge.endpoint.y, discharge.endpoint.z};
        _roundFields->reactionDelay = static_cast<uint16_t>(discharge.delayMilliseconds);
        _discharges.push_back(discharge);
        if (generation != _signalGeneration) return false;
    }
    return true;
}

void AttackBuffer::saveContinuation(SavedPhysicalAction &saved, const Game &game) const {
    using G = resource::Gff; using F = G::Field;
    std::vector<std::shared_ptr<G>> attacks;
    saved.sources.clear(); saved.targetEffects.clear(); saved.histories.clear();
    for (const auto &a : _attacks) {
        std::vector<std::shared_ptr<G>> values, slots;
        const int fields[] = {static_cast<int>(a.source), static_cast<int>(a.ranged), static_cast<int>(a.result), static_cast<int>(a.roll), static_cast<int>(a.naturalTwenty), static_cast<int>(a.naturalOne), static_cast<int>(a.coupDeGrace), static_cast<int>(a.criticalThreat.threshold), static_cast<int>(a.criticalThreat.threatened), static_cast<int>(a.criticalThreat.confirmationRoll), static_cast<int>(a.criticalThreat.confirmationBonus), static_cast<int>(a.criticalThreat.confirmed), static_cast<int>(a.criticalThreat.multiplier), static_cast<int>(a.impactTimeMilliseconds), static_cast<int>(a.signaled), static_cast<int>(a.onHitResolved), static_cast<int>(a.attackBonusBreakdown.baseAttackBonus), static_cast<int>(a.attackBonusBreakdown.strengthModifier), static_cast<int>(a.attackBonusBreakdown.dexterityModifier), static_cast<int>(a.attackBonusBreakdown.dualWieldPenalty), static_cast<int>(a.attackBonusBreakdown.smallOffhandBonus), static_cast<int>(a.attackBonusBreakdown.featBonus), static_cast<int>(a.attackBonusBreakdown.duelingFeat), static_cast<int>(a.attackBonusBreakdown.duelingBonus), static_cast<int>(a.attackBonusBreakdown.closeProximityRangedBonus), static_cast<int>(a.attackBonusBreakdown.meleeOnRangedBonus), static_cast<int>(a.attackBonusBreakdown.weaponFocusBonus), static_cast<int>(a.attackBonusBreakdown.targetingBonus), static_cast<int>(a.attackBonusBreakdown.superiorWeaponFocusBonus), static_cast<int>(a.attackBonusBreakdown.formBonus), static_cast<int>(a.attackBonusBreakdown.dualStrikeBonus), static_cast<int>(a.attackBonusBreakdown.effectBonus), static_cast<int>(a.defenseBreakdown.total), static_cast<int>(a.defenseBreakdown.armor), static_cast<int>(a.defenseBreakdown.dexterity), static_cast<int>(a.defenseBreakdown.classDefense), static_cast<int>(a.defenseBreakdown.natural), static_cast<int>(a.defenseBreakdown.dodgeAndDeflection), static_cast<int>(a.defenseBreakdown.feat), static_cast<int>(a.defenseBreakdown.stance), static_cast<int>(a.defenseBreakdown.form), static_cast<int>(a.defenseBreakdown.debilitationPenalty), static_cast<int>(a.damageBreakdown.strengthModifier), static_cast<int>(a.damageBreakdown.otherSpecialBonus), static_cast<int>(a.damageBreakdown.sneakAttack), static_cast<int>(a.damageBreakdown.weaponSpecialization), static_cast<int>(a.damageBreakdown.combatFeatDamage), static_cast<int>(a.damageBreakdown.preciseShotDamage), static_cast<int>(a.damageBreakdown.formDamage), static_cast<int>(a.damageBreakdown.unarmedFeatDamage209), static_cast<int>(a.damageBreakdown.unarmedFeatDamage212), static_cast<int>(a.damageBreakdown.criticalMultiplier), static_cast<int>(a.attackBonusBreakdown.inspireFollowersBonus), static_cast<int>(a.attackBonusBreakdown.leaderCharismaBonus), static_cast<int>(a.attackBonusBreakdown.crushOppositionPenalty), a.effectsAfterOnHit == std::numeric_limits<size_t>::max() ? -1 : static_cast<int>(a.effectsAfterOnHit), static_cast<int>(a.reportsEffectOutcome), a.effectCode, a.saveRecord.saveType, a.saveRecord.saveRoll, a.saveRecord.modifierTotal, a.saveRecord.baseSave, a.saveRecord.finalTotal, a.saveRecord.difficultyClass, a.saveRecord.outcome};
        for (int value : fields) values.push_back(G::Builder().type(0).field(F::newInt("Value", value)).build());
        for (int value : a.damageBreakdown.rawDamageSlots) slots.push_back(G::Builder().type(0).field(F::newInt("Value", value)).build());
        SavedWeaponImpact onHit; onHit.applications = a.onHitApplications; onHit.feedback = a.deferredFeedback;
        auto g = G::Builder().type(0).field(F::newList("Values", std::move(values)))
            .field(F::newList("DamageSlots", std::move(slots))).field(F::newStruct("Packet", a.damage.saveContinuation()))
            .field(F::newStruct("OnHit", onHit.toGff())).build();
        attacks.push_back(std::move(g));
        for (const auto &object : {std::static_pointer_cast<Object>(a.sourceItem.resolve()), a.sourceActor.resolve()}) {
            auto ref = SavedObjectReference::fromRuntimeId(object ? object->id() : script::kObjectInvalid);
            game.bindSavedObjectReference(ref); saved.sources.push_back(std::move(ref));
        }
        saved.targetEffects.push_back(a.targetEffects);
        SavedCombatAttack history; history.history = std::make_shared<AttackHistory>(*a.history);
        history.fields = std::make_shared<AttackEventFields>(*a.eventFields); saved.histories.push_back(std::move(history));
    }
    std::vector<std::shared_ptr<G>> discharges;
    for (const auto &d : _discharges) {
        discharges.push_back(G::Builder().type(0)
            .field(F::newInt("Time", d.timeMilliseconds)).field(F::newInt("Hand", d.hand))
            .field(F::newInt("Attack", d.attack)).field(F::newInt("Result", static_cast<int>(d.result)))
            .field(F::newFloat("X", d.endpoint.x)).field(F::newFloat("Y", d.endpoint.y))
            .field(F::newFloat("Z", d.endpoint.z)).field(F::newInt("Delay", d.delayMilliseconds))
            .field(F::newInt("AttackType", d.attackType)).field(F::newInt("WeaponAttackType", d.weaponAttackType)).build());
    }
    saved.roundRecord.reset();
    if (_roundHistory) {
        SavedCombatAttack record;
        record.history = std::make_shared<AttackHistory>(*_roundHistory);
        record.fields = std::make_shared<AttackEventFields>(*_roundFields);
        saved.roundRecord = std::move(record);
    }
    saved.state = G::Builder().type(0).field(F::newInt("Feat", static_cast<int>(_feat)))
        .field(F::newInt("Pending", static_cast<int>(_pendingMeleeAttacks)))
        .field(F::newByte("Prepared", _meleeSequencePrepared)).field(F::newList("Attacks", std::move(attacks)))
        .field(F::newList("Discharges", std::move(discharges)))
        .field(F::newInt("NextDischarge", static_cast<int>(_nextDischarge)))
        .field(F::newByte("TargetReleased", _targetWorkReleased)).build();
}

void AttackBuffer::restoreContinuation(const SavedPhysicalAction &saved) {
    ++_signalGeneration;
    _resolvingHistory.reset();
    _attacks.clear();
    _recordRoll = 0;
    _recordModifier = 0;
    _recordDeflection = DeflectionBreakdown {};
    _feat = static_cast<FeatType>(saved.state->getInt("Feat", -1));
    _pendingMeleeAttacks = saved.state->getInt("Pending"); _meleeSequencePrepared = saved.state->getBool("Prepared");
    _discharges.clear();
    for (const auto &g : saved.state->getList("Discharges")) {
        Discharge d;
        d.timeMilliseconds = g->getInt("Time");
        d.hand = g->getInt("Hand");
        d.attack = g->getInt("Attack", -1);
        d.result = static_cast<AttackResultType>(g->getInt("Result", static_cast<int>(AttackResultType::Miss)));
        d.endpoint = glm::vec3(g->getFloat("X"), g->getFloat("Y"), g->getFloat("Z"));
        d.delayMilliseconds = g->getInt("Delay");
        d.attackType = static_cast<uint16_t>(g->getInt("AttackType"));
        d.weaponAttackType = static_cast<uint8_t>(g->getInt("WeaponAttackType"));
        _discharges.push_back(d);
    }
    _roundHistory.reset();
    _roundFields.reset();
    if (saved.roundRecord) {
        _roundHistory = std::make_shared<AttackHistory>(*saved.roundRecord->history);
        _roundFields = std::make_shared<AttackEventFields>(*saved.roundRecord->fields);
    }
    _nextDischarge = std::min(static_cast<size_t>(std::max(0, saved.state->getInt("NextDischarge"))), _discharges.size());
    _targetWorkReleased = saved.state->getBool("TargetReleased");
    size_t index = 0;
    for (const auto &g : saved.state->getList("Attacks")) {
        const auto &values = g->getList("Values");
        auto value = [&](size_t i) { return i < values.size() ? values[i]->getInt("Value") : 0; };
        auto packet = g->findStruct("Packet");
        auto &a = _attacks.emplace_back(
            Source::Main, false, AttackBonusBreakdown {},
            packet ? DamagePacket::restoreContinuation(*packet) : DamagePacket());
        a.source = static_cast<decltype(a.source)>(value(0));
        a.ranged = static_cast<decltype(a.ranged)>(value(1));
        a.result = static_cast<decltype(a.result)>(value(2));
        a.roll = static_cast<decltype(a.roll)>(value(3));
        a.naturalTwenty = static_cast<decltype(a.naturalTwenty)>(value(4));
        a.naturalOne = static_cast<decltype(a.naturalOne)>(value(5));
        a.coupDeGrace = static_cast<decltype(a.coupDeGrace)>(value(6));
        a.criticalThreat.threshold = static_cast<decltype(a.criticalThreat.threshold)>(value(7));
        a.criticalThreat.threatened = static_cast<decltype(a.criticalThreat.threatened)>(value(8));
        a.criticalThreat.confirmationRoll = static_cast<decltype(a.criticalThreat.confirmationRoll)>(value(9));
        a.criticalThreat.confirmationBonus = static_cast<decltype(a.criticalThreat.confirmationBonus)>(value(10));
        a.criticalThreat.confirmed = static_cast<decltype(a.criticalThreat.confirmed)>(value(11));
        a.criticalThreat.multiplier = static_cast<decltype(a.criticalThreat.multiplier)>(value(12));
        a.impactTimeMilliseconds = static_cast<decltype(a.impactTimeMilliseconds)>(value(13));
        a.signaled = static_cast<decltype(a.signaled)>(value(14));
        a.onHitResolved = static_cast<decltype(a.onHitResolved)>(value(15));
        a.attackBonusBreakdown.baseAttackBonus = static_cast<decltype(a.attackBonusBreakdown.baseAttackBonus)>(value(16));
        a.attackBonusBreakdown.strengthModifier = static_cast<decltype(a.attackBonusBreakdown.strengthModifier)>(value(17));
        a.attackBonusBreakdown.dexterityModifier = static_cast<decltype(a.attackBonusBreakdown.dexterityModifier)>(value(18));
        a.attackBonusBreakdown.dualWieldPenalty = static_cast<decltype(a.attackBonusBreakdown.dualWieldPenalty)>(value(19));
        a.attackBonusBreakdown.smallOffhandBonus = static_cast<decltype(a.attackBonusBreakdown.smallOffhandBonus)>(value(20));
        a.attackBonusBreakdown.featBonus = static_cast<decltype(a.attackBonusBreakdown.featBonus)>(value(21));
        a.attackBonusBreakdown.duelingFeat = static_cast<decltype(a.attackBonusBreakdown.duelingFeat)>(value(22));
        a.attackBonusBreakdown.duelingBonus = static_cast<decltype(a.attackBonusBreakdown.duelingBonus)>(value(23));
        a.attackBonusBreakdown.closeProximityRangedBonus = static_cast<decltype(a.attackBonusBreakdown.closeProximityRangedBonus)>(value(24));
        a.attackBonusBreakdown.meleeOnRangedBonus = static_cast<decltype(a.attackBonusBreakdown.meleeOnRangedBonus)>(value(25));
        a.attackBonusBreakdown.weaponFocusBonus = static_cast<decltype(a.attackBonusBreakdown.weaponFocusBonus)>(value(26));
        a.attackBonusBreakdown.targetingBonus = static_cast<decltype(a.attackBonusBreakdown.targetingBonus)>(value(27));
        a.attackBonusBreakdown.superiorWeaponFocusBonus = static_cast<decltype(a.attackBonusBreakdown.superiorWeaponFocusBonus)>(value(28));
        a.attackBonusBreakdown.formBonus = static_cast<decltype(a.attackBonusBreakdown.formBonus)>(value(29));
        a.attackBonusBreakdown.dualStrikeBonus = static_cast<decltype(a.attackBonusBreakdown.dualStrikeBonus)>(value(30));
        a.attackBonusBreakdown.effectBonus = static_cast<decltype(a.attackBonusBreakdown.effectBonus)>(value(31));
        a.defenseBreakdown.total = static_cast<decltype(a.defenseBreakdown.total)>(value(32));
        a.defenseBreakdown.armor = static_cast<decltype(a.defenseBreakdown.armor)>(value(33));
        a.defenseBreakdown.dexterity = static_cast<decltype(a.defenseBreakdown.dexterity)>(value(34));
        a.defenseBreakdown.classDefense = static_cast<decltype(a.defenseBreakdown.classDefense)>(value(35));
        a.defenseBreakdown.natural = static_cast<decltype(a.defenseBreakdown.natural)>(value(36));
        a.defenseBreakdown.dodgeAndDeflection = static_cast<decltype(a.defenseBreakdown.dodgeAndDeflection)>(value(37));
        a.defenseBreakdown.feat = static_cast<decltype(a.defenseBreakdown.feat)>(value(38));
        a.defenseBreakdown.stance = static_cast<decltype(a.defenseBreakdown.stance)>(value(39));
        a.defenseBreakdown.form = static_cast<decltype(a.defenseBreakdown.form)>(value(40));
        a.defenseBreakdown.debilitationPenalty = static_cast<decltype(a.defenseBreakdown.debilitationPenalty)>(value(41));
        a.damageBreakdown.strengthModifier = static_cast<decltype(a.damageBreakdown.strengthModifier)>(value(42));
        a.damageBreakdown.otherSpecialBonus = static_cast<decltype(a.damageBreakdown.otherSpecialBonus)>(value(43));
        a.damageBreakdown.sneakAttack = static_cast<decltype(a.damageBreakdown.sneakAttack)>(value(44));
        a.damageBreakdown.weaponSpecialization = static_cast<decltype(a.damageBreakdown.weaponSpecialization)>(value(45));
        a.damageBreakdown.combatFeatDamage = static_cast<decltype(a.damageBreakdown.combatFeatDamage)>(value(46));
        a.damageBreakdown.preciseShotDamage = static_cast<decltype(a.damageBreakdown.preciseShotDamage)>(value(47));
        a.damageBreakdown.formDamage = static_cast<decltype(a.damageBreakdown.formDamage)>(value(48));
        a.damageBreakdown.unarmedFeatDamage209 = static_cast<decltype(a.damageBreakdown.unarmedFeatDamage209)>(value(49));
        a.damageBreakdown.unarmedFeatDamage212 = static_cast<decltype(a.damageBreakdown.unarmedFeatDamage212)>(value(50));
        a.damageBreakdown.criticalMultiplier = static_cast<decltype(a.damageBreakdown.criticalMultiplier)>(value(51));
        a.attackBonusBreakdown.inspireFollowersBonus = value(52);
        a.attackBonusBreakdown.leaderCharismaBonus = value(53);
        a.attackBonusBreakdown.crushOppositionPenalty = value(54);
        a.effectsAfterOnHit = value(55) < 0 ? std::numeric_limits<size_t>::max()
                                            : static_cast<size_t>(value(55));
        a.reportsEffectOutcome = value(56) != 0;
        a.effectCode = value(57);
        a.saveRecord.saveType = value(58);
        a.saveRecord.saveRoll = value(59);
        a.saveRecord.modifierTotal = value(60);
        a.saveRecord.baseSave = value(61);
        a.saveRecord.finalTotal = value(62);
        a.saveRecord.difficultyClass = value(63);
        a.saveRecord.outcome = value(64);
        const auto &slots = g->getList("DamageSlots");
        for (size_t n = 0; n < std::min(slots.size(), a.damageBreakdown.rawDamageSlots.size()); ++n)
            a.damageBreakdown.rawDamageSlots[n] = slots[n]->getInt("Value");
        a.sourceItem = std::dynamic_pointer_cast<Item>(saved.sources[index * 2].boundObject());
        a.sourceActor = saved.sources[index * 2 + 1].boundObject();
        a.targetEffects = saved.targetEffects[index];
        for (auto &effect : a.targetEffects) effect.restoring = false;
        a.history = std::make_shared<AttackHistory>(*saved.histories[index].history);
        a.eventFields = std::make_shared<AttackEventFields>(*saved.histories[index].fields);
        a.kind = static_cast<PhysicalAttackKind>(a.eventFields->weaponAttackType);
        if (auto p = g->findStruct("OnHit")) {
            auto onHit = SavedWeaponImpact::fromGff(*p, SerializedIdentityContext {});
            a.onHitApplications = std::move(onHit.applications); a.deferredFeedback = std::move(onHit.feedback);
            for (auto &application : a.onHitApplications) application.creator = a.sourceActor;
        }
        ++index;
    }
}

void AttackSchedule::saveContinuation(resource::Gff &g) const {
    using F = resource::Gff::Field;
    g.fields().push_back(F::newInt("Phase", _state)); g.fields().push_back(F::newFloat("Time", _time));
    g.fields().push_back(F::newByte("Melee", _melee)); g.fields().push_back(F::newByte("Ranged", _ranged));
    g.fields().push_back(F::newInt("MeleeElapsed", _elapsedMilliseconds));
    g.fields().push_back(F::newInt("MeleeEnd", _completionMilliseconds));
    g.fields().push_back(F::newFloat("MeleeRemainder", _elapsedRemainderMilliseconds));
}
void AttackSchedule::restoreContinuation(const resource::Gff &g) {
    _state = static_cast<State>(g.getInt("Phase")); _time = g.getFloat("Time");
    _melee = g.getBool("Melee"); _ranged = g.getBool("Ranged");
    _elapsedMilliseconds = g.getInt("MeleeElapsed"); _completionMilliseconds = g.getInt("MeleeEnd");
    _elapsedRemainderMilliseconds = g.getFloat("MeleeRemainder");
    // Entry states have already executed before the snapshot was taken.
    if (_state == Attack) _state = WaitDamage;
    else if (_state == Damage) _state = clocked() && _elapsedMilliseconds < _completionMilliseconds ? WaitDamage : WaitFinish;
}

static bool producesItemOnHitEffects(AttackResultType result) {
    return isAttackSuccessful(result) ||
           result == AttackResultType::Deflected;
}

void AttackBuffer::resolveItemOnHitProperties(
    const Creature &attacker,
    Object &target,
    Attack &attack) {

    if (attack.onHitResolved) {
        return;
    }
    attack.onHitResolved = true;
    const auto sourceItem = attack.sourceItem.resolve();
    if (!sourceItem ||
        !producesItemOnHitEffects(attack.result) ||
        target.plotFlag()) {
        return;
    }

    auto *targetCreature = dyn_cast<Creature>(&target);
    if (!targetCreature) {
        return;
    }

    for (const ItemOnHitProperty &property :
         sourceItem->itemOnHitProperties()) {
        if (property.durationBranch &&
            (property.chance <= 0 || property.duration <= 0.0f)) {
            continue;
        }

        if (property.durationBranch || property.chance != 100) {
            int chanceRoll = randomInt(0, 99);
            if (chanceRoll >= property.chance) {
                continue;
            }
        }

        std::optional<int> effectOutcomeType =
            getItemOnHitEffectOutcomeType(property.subtype);
        if (effectOutcomeType) {
            for (ItemOnHitApplication &pending :
                 attack.onHitApplications) {
                pending.emitEffectOutcome = false;
            }
        }

        EffectOutcomeBreakdown effectOutcome;
        bool saved = false;
        if (property.savingThrow != SavingThrow::None) {
            SavingThrowBreakdown save =
                targetCreature->getSavingThrowBreakdown(
                    property.savingThrow,
                    property.savingThrowType, &attacker);
            int roll = randomInt(1, 20);
            int total = roll + save.total();
            const auto result = targetCreature->getSavingThrowResult(total, property.difficultyClass,
                property.savingThrowType, &attacker);
            saved = result != SavingThrowResult::Failed;

            attack.deferredFeedback.emplace_back(SavingThrowFeedback {
                property.savingThrow,
                save.base,
                save.modifier,
                roll,
                property.difficultyClass,
                property.savingThrowType,
            });

            if (effectOutcomeType) {
                effectOutcome.present = true;
                effectOutcome.saveType =
                    static_cast<int>(property.savingThrow);
                effectOutcome.effectType = *effectOutcomeType;
                effectOutcome.saveMode = 0;
                effectOutcome.saveRoll = roll;
                effectOutcome.modifierTotal = save.modifier;
                effectOutcome.baseSave = save.base;
                effectOutcome.finalTotal = total;
                effectOutcome.difficultyClass =
                    property.difficultyClass;
                effectOutcome.outcome = static_cast<int>(result);
            }
        }
        if (saved) {
            continue;
        }

        if (property.subtype == ItemOnHitSubtype::SlayAG ||
            (property.subtype == ItemOnHitSubtype::SlayRG &&
             !matchesSlayRacialGroup(
                 static_cast<int>(targetCreature->racialType()), property.parameter))) {
            continue;
        }

        if (property.subtype == ItemOnHitSubtype::AbilityDrain &&
            property.parameter >= static_cast<int>(Ability::Strength) &&
            property.parameter <= static_cast<int>(Ability::Charisma)) {
            attack.deferredFeedback.emplace_back(AbilityDrainFeedback {
                static_cast<Ability>(property.parameter),
                1,
                30,
            });
        }

        attack.onHitApplications.emplace_back(ItemOnHitApplication {
            property.subtype,
            property.duration,
            property.parameter,
            attack.sourceActor,
            effectOutcome,
            effectOutcomeType.has_value(),
        });
    }
}

bool AttackBuffer::resolveDamage(
    const Creature &attacker,
    Object &target,
    Attack &attack) {

    const auto generation = _signalGeneration;
    // A cutscene attack's damage carries no item on-hit properties.
    if (attack.result == AttackResultType::Deflected) {
        // A returned shot does not consume the defender's finite pools. Its
        // item on-hit properties still resolve against the defender.
        attack.effectsAfterOnHit = attack.targetEffects.size();
        if (!_cutscene) {
            resolveItemOnHitProperties(attacker, target, attack);
            appendHitVisuals(attacker, attack);
        }
        return generation == _signalGeneration;
    }
    if (!attack.damage.empty() && !attack.damage.isResolved()) {
        attack.damage.resolvePhysical(target, attacker);
        // Mitigation lines travel with the round's first release: a ranged
        // round's first discharge, a melee round's first hit.
        for (const auto &feedback : attack.damage.resolution().mitigationFeedback)
            attack.deferredFeedback.emplace_back(feedback);
    }
    if (generation != _signalGeneration) return false;
    // Item on-hit effects take effect before the hit visuals and the death
    // that follow them.
    attack.effectsAfterOnHit = attack.targetEffects.size();
    if (!_cutscene) {
        resolveItemOnHitProperties(attacker, target, attack);
        appendHitVisuals(attacker, attack);
    }
    if (!attack.ranged && attack.damage.isResolved()) {
        attack.eventFields->killingBlow = getHitDamageTotal(attack.damage) >= target.currentHitPoints();
        if (attack.coupDeGrace && isa<Creature>(target)) {
            auto death = std::make_shared<DeathEffect>(false, true, false);
            death->setSaveFacingCreator(attack.sourceActor.resolve());
            auto instance = death->saveFacingInstance();
            instance.effect = std::move(death);
            instance.setDuration(DurationType::Instant, 0.0f);
            attack.targetEffects.push_back(std::move(instance));
        }
    }
    return generation == _signalGeneration;
}

// The damage types a hit dealt show the hit visual of damagehitvisual.2da
// (the ranged column for a ranged hit), and a deflected bolt shows the
// deflection. A blank cell names no visual effect.
void AttackBuffer::appendHitVisuals(const Creature &attacker, Attack &attack) {
    static constexpr int kDeflectionVisual = 4023;
    auto &game = attacker.game();
    if (attack.damage.isResolved()) {
        const auto &amounts = attack.damage.resolution().damageAmounts;
        for (int slot = 3; slot < 14; ++slot) {
            if (amounts[slot] <= 0) continue;
            const int id = attacker.services().game.visualEffects.hitVisual(slot, attack.ranged);
            if (id <= 0) continue;
            appendTargetEffect(attack, game.newEffect<VisualEffect>(id, false, attacker.services()),
                               DurationType::Instant, 0.0f);
        }
    }
    // Of the interception results only a deflected shot reaches the damage
    // step, so it alone shows the deflection.
    if (attack.result == AttackResultType::Deflected)
        appendTargetEffect(attack, game.newEffect<VisualEffect>(kDeflectionVisual, false, attacker.services()),
                           DurationType::Instant, 0.0f);
}

// A hit whose feat attempted a saving throw reports the effect it applied,
// as soon as the hit is released. The attack record starts cleared, so
// without a save its outcome is 0, which reads as a failed save.
void AttackBuffer::reportEffectOutcome(Game &game, ServicesView &services, const Object &target, Attack &attack) {
    if (!attack.reportsEffectOutcome) return;
    attack.reportsEffectOutcome = false;
    auto record = attack.saveRecord;
    record.present = true;
    record.effectType = attack.effectCode;
    if (record.outcome < 0) record.outcome = static_cast<int>(SavingThrowResult::Failed);
    addAttackEffectOutcomeFeedback(game, services, attack.sourceActor.resolve(), target, record);
}

void AttackBuffer::resolve(Creature &attacker, Object &target) {
    // A ranged animation without discharges resolves an empty round.

    // Each hit is already resolved. Publish the round summary without
    // consuming protection again or rerunning post-roll work.
    int resolvedBaseDamage = 0;
    for (const auto &attack : _attacks) {
        // Misses and returned bolts have no target-resolved packet here. Accumulate
        // only positive signed-word damage totals from resolved attacks.
        if (attack.damage.isResolved()) {
            const int baseDamage = narrowAttackDamage(attack.damage.resolvedDamage());
            if (baseDamage > 0) resolvedBaseDamage += baseDamage;
        }
    }
    if (resolvedBaseDamage >= target.currentHitPoints()) {
        attacker.incrementFuryDamageBonus();
    }
    for (auto &attack : _attacks) {
        // Populate only fields with an existing represented producer. The
        // remaining reaction/animation/debug fields retain clear state.
        auto &fields = *attack.eventFields;
        fields.result = static_cast<uint8_t>(attack.result);
        fields.deflected = attack.result == AttackResultType::Deflected;
        fields.ranged = attack.ranged ? 1 : 0;
        // A ranged attack publishes the endpoint and flight time of the
        // discharge that carried it.
        const auto discharge = std::find_if(_discharges.begin(), _discharges.end(),
            [&](const Discharge &d) { return d.attack == static_cast<int>(&attack - &_attacks.front()); });
        if (attack.ranged && discharge != _discharges.end()) {
            fields.rangedTarget = {discharge->endpoint.x, discharge->endpoint.y, discharge->endpoint.z};
            fields.reactionDelay = static_cast<uint16_t>(discharge->delayMilliseconds);
        }
        // Hand, extra-main, and unarmed attacks have distinct serialized kind values.
        fields.weaponAttackType = static_cast<uint8_t>(attack.kind);
        fields.coupDeGrace = attack.coupDeGrace ? 1 : 0;
        if (attack.damage.isResolved()) fields.damage = attack.damage.resolution().damageAmounts;
    }
    // Every record carries the round's animation length.
    for (auto &attack : _attacks) attack.eventFields->animationLength = kPhysicalAttackPauseMilliseconds;
    if (_roundFields) _roundFields->animationLength = kPhysicalAttackPauseMilliseconds;
    // Until a hit is released, the melee record holds the last attack resolved,
    // without its damage, which travels with the hit.
    if (_roundFields && !_roundFields->ranged && !_attacks.empty()) {
        const auto &last = _attacks.back();
        _roundHistory->type = last.history->type;
        _roundFields->result = last.eventFields->result;
        _roundFields->weaponAttackType = last.eventFields->weaponAttackType;
        _roundFields->deflected = last.eventFields->deflected;
        _roundFields->coupDeGrace = last.eventFields->coupDeGrace;
        _roundFields->killingBlow = last.eventFields->killingBlow;
        _roundFields->damage.fill(-1);
    }
    // The ranged record keeps the round-level values of its last hit shot;
    // releases only replace the per-discharge snapshot.
    if (_roundFields && _roundFields->ranged) {
        for (const auto &discharge : _discharges) {
            if (discharge.attack < 0 || static_cast<size_t>(discharge.attack) >= _attacks.size()) continue;
            const auto &fields = *_attacks[discharge.attack].eventFields;
            _roundFields->deflected = fields.deflected;
            _roundFields->coupDeGrace = fields.coupDeGrace;
            _roundFields->killingBlow = fields.killingBlow;
        }
    }
}

uint16_t AttackBuffer::resolveReaction(const Creature *target, bool engaged, bool targetEngaged,
                                       bool targetHasRoom, bool forced) {
    const bool ranged = _roundFields && _roundFields->ranged;
    if (!target || (!ranged && _attacks.empty())) return 10001;
    const bool anyHit = std::any_of(_attacks.begin(), _attacks.end(),
        [](const Attack &attack) { return attack.rolledHitLike; });
    const auto result = recordResult();
    bool committed = false;
    const uint16_t reaction = resolveAttackReaction(target, ranged, result, anyHit, engaged,
        targetEngaged, targetHasRoom, forced, committed);
    const uint16_t length = committed ? kPhysicalAttackPauseMilliseconds : 0;
    for (auto &attack : _attacks) {
        attack.eventFields->reactionAnimation = reaction;
        attack.eventFields->reactionAnimationLength = length;
    }
    if (_roundFields) {
        _roundFields->reactionAnimation = reaction;
        _roundFields->reactionAnimationLength = length;
    }
    return reaction;
}

static void setEventTime(SavedEventRecord &event, const Game &game, uint64_t when) {
    event.day = static_cast<uint32_t>(when / game.millisecondsPerWorldDay());
    event.time = static_cast<uint32_t>(when % game.millisecondsPerWorldDay());
}

static void enqueueAppliedEffect(Module &module, const Game &game, const Object &caller, const Object &target,
                                 EffectInstance effect, uint64_t when) {
    SavedEventRecord event;
    setEventTime(event, game, when);
    event.object = SavedObjectReference::fromRuntimeId(target.id());
    event.caller = SavedObjectReference::fromRuntimeId(caller.id());
    event.eventId = static_cast<uint32_t>(SavedEventType::ApplyEffect);
    event.payload = std::move(effect);
    module.enqueueSaveEvent(std::move(event));
}

static constexpr int kProjectileMissVisualEffect = 4032;

// The impact of a bolt that hits nothing is shown at its endpoint, owned by the area.
static void queueProjectileMissImpact(Game &game, ServicesView &services, const Object &caller,
                                      const glm::vec3 &point, uint64_t when) {
    auto module = game.module();
    auto area = module ? module->area() : nullptr;
    if (!area) return;
    auto visual = std::make_shared<VisualEffect>(kProjectileMissVisualEffect, false, services);
    visual->setLocation(point);
    visual->setSaveFacingCreator(game.getObjectById(caller.id()));
    auto instance = visual->saveFacingInstance();
    instance.effect = visual;
    instance.setDuration(DurationType::Instant, 0.0f);
    enqueueAppliedEffect(*module, game, caller, *area, std::move(instance), when);
}

void AttackBuffer::applyDamage(
    Attack &attack,
    Creature &attacker,
    Object &target,
    Game &game,
    uint64_t impactTime) {

    if (attack.damage.empty()) return;
    DamageEffect::ApplicationContext context;
    context.damageAmounts = attack.damage.resolution().damageAmounts;
    context.suppressDamageShields = attack.ranged;
    // The hit's own attack line reports it.
    context.feedbackHandled = true;

    auto effect = game.newEffect<DamageEffect>(
        std::move(attack.damage),
        std::move(context));
    effect->setSaveFacingCreator(attack.sourceActor.resolve());
    if (auto module = game.module()) {
        auto instance = effect->saveFacingInstance();
        instance.effect = effect;
        instance.setDuration(DurationType::Instant, 0.0f);
        enqueueAppliedEffect(*module, game, attacker, target, std::move(instance), impactTime);
    } else {
        // Isolated objects have no world queue. Live module combat always
        // takes the saved-event path above, after event 15 at equal time.
        target.applyEffect(std::move(effect), DurationType::Instant);
    }
}

void AttackBuffer::applyEffects(
    Attack &attack,
    Creature &attacker,
    Object &target,
    Game &game,
    ServicesView &services,
    uint64_t impactTime,
    std::vector<ItemOnHitApplication> onHit) {

    // Transfer hit-owned work before any synchronous damage/effect callback.
    auto targetEffects = std::move(attack.targetEffects);
    const size_t split = std::min(attack.effectsAfterOnHit, targetEffects.size());
    const auto module = game.module();
    const bool deferTargetEffects = !isa<Creature>(target);
    if (!isAttackSuccessful(attack.result)) {
        game.floatingText().addMiss(attacker, target);
    }
    applyDamage(attack, attacker, target, game, impactTime);
    // Non-creature melee recipients receive their effects after weapon damage
    // at the same deadline. Creature melee effects are synchronous. In both
    // cases the emitted hit owns the remaining work. Item on-hit effects come
    // before the hit visuals and the death.
    auto applyTargetEffect = [&](EffectInstance &effect) {
        if (module && deferTargetEffects) enqueueAppliedEffect(*module, game, attacker, target, std::move(effect), impactTime);
        else target.applyEffect(std::move(effect));
    };
    for (size_t index = 0; index < split; ++index) applyTargetEffect(targetEffects[index]);
    applyItemOnHitApplications(std::move(onHit), target, game, services);
    for (size_t index = split; index < targetEffects.size(); ++index) applyTargetEffect(targetEffects[index]);
}

void AttackBuffer::publishAttacked(
    const Attack &attack,
    const std::shared_ptr<AttackHistory> &history,
    const std::shared_ptr<AttackEventFields> &fields,
    Game &game,
    const Creature &attacker,
    const Object &target,
    uint64_t when) {

    if (!isa<Creature>(target) && !isa<Door>(target) && !isa<Placeable>(target)) return;
    auto module = game.module();
    if (!module) return;
    // The event shares the record; delivery reads its values at that time.
    SavedCombatAttack payload;
    payload.data.type = 0x2222;
    payload.history = history;
    payload.fields = fields;
    payload.reactionObject = SavedObjectReference::fromRuntimeId(target.id());
    const auto weapon = attack.sourceItem.resolve();
    payload.ammoItem = SavedObjectReference::fromRuntimeId(weapon ? weapon->id() : script::kObjectInvalid);
    SavedEventRecord event;
    setEventTime(event, game, when);
    event.object = SavedObjectReference::fromRuntimeId(target.id());
    event.caller = SavedObjectReference::fromRuntimeId(attacker.id());
    event.eventId = static_cast<uint32_t>(SavedEventType::OnMeleeAttacked);
    event.payload = std::move(payload);
    module->enqueueSaveEvent(std::move(event));
}

void AttackBuffer::signalAttack(
    Attack &attack,
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    if (attack.signaled) return;
    attack.signaled = true;
    const uint64_t impactTime = game.worldTimeMilliseconds();
    // The released hit takes over the round's record; every melee hit,
    // misses included, emits event 15 with that shared record, and its
    // damage follows in the same instant.
    if (_roundFields) {
        _roundHistory->type = attack.history->type;
        _roundFields->result = attack.eventFields->result;
        _roundFields->weaponAttackType = attack.eventFields->weaponAttackType;
        _roundFields->deflected = attack.eventFields->deflected;
        _roundFields->coupDeGrace = attack.eventFields->coupDeGrace;
        _roundFields->killingBlow = attack.eventFields->killingBlow;
        _roundFields->damage = attack.eventFields->damage;
    }
    _recordRoll = attack.roll;
    _recordModifier = _cutscene ? 0 : static_cast<int8_t>(attack.attackBonusBreakdown.total());
    publishAttacked(attack,
        _roundHistory ? _roundHistory : attack.history,
        _roundFields ? _roundFields : attack.eventFields,
        game, attacker, target, impactTime);
    addCombatFeedback(game, services, attacker, target, attack);
    reportEffectOutcome(game, services, target, attack);
    addDeflectionFeedback(game, services, attacker, target, attack);
    // A hit sends the feedback the round has gathered, after its damage; a
    // miss leaves it for the next hit.
    auto feedback = isAttackSuccessful(attack.result) ? takeDeferredFeedback()
                                                      : std::vector<DeferredCombatFeedback> {};
    auto applications = std::move(attack.onHitApplications);
    attack.onHitApplications.clear();
    applyEffects(attack, attacker, target, game, services, impactTime, std::move(applications));
    releaseDeferredFeedback(game, services, attacker, target, impactTime, std::move(feedback));
}

void AttackBuffer::returnDeflected(
    Attack &attack,
    Game &game,
    Creature &attacker,
    Creature &deflector,
    uint64_t impactTime) {

    if (attack.damage.empty()) return;
    // The returned bolt meets the shooter's resistance and immunity when it is
    // released and strikes the shooter as the deflector's damage.
    // The shooter is both the receiver and the damager of this resolution, so
    // its mitigation lines join the round's feedback, which the next discharge
    // released carries; after the last discharge they are never shown.
    attack.damage.resolveReturned(attacker);
    for (const auto &feedback : attack.damage.resolution().mitigationFeedback)
        attack.deferredFeedback.emplace_back(ReturnedMitigationFeedback {feedback});
    DamageEffect::ApplicationContext context;
    context.damageAmounts = attack.damage.resolution().damageAmounts;
    context.suppressDamageShields = true;
    context.feedbackHandled = true;
    auto effect = game.newEffect<DamageEffect>(std::move(attack.damage), std::move(context));
    effect->setSaveFacingCreator(game.getObjectById(deflector.id()));
    if (auto module = game.module()) {
        auto instance = effect->saveFacingInstance();
        instance.effect = effect;
        instance.setDuration(DurationType::Instant, 0.0f);
        enqueueAppliedEffect(*module, game, deflector, attacker, std::move(instance), impactTime);
    } else {
        attacker.applyEffect(std::move(effect), DurationType::Instant);
    }
}

void AttackBuffer::releaseTargetWork(
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target,
    uint64_t impactTime) {

    // Transfer the round's work before any synchronous callback. Attack by
    // attack, the item on-hit work takes effect after the effects that precede
    // it and before the hit visuals and the death that follow it.
    struct AttackWork {
        std::vector<EffectInstance> beforeOnHit;
        std::vector<ItemOnHitApplication> onHit;
        std::vector<EffectInstance> afterOnHit;
    };
    std::vector<AttackWork> work;
    for (auto &attack : _attacks) {
        AttackWork entry;
        const size_t split = std::min(attack.effectsAfterOnHit, attack.targetEffects.size());
        for (size_t index = 0; index < attack.targetEffects.size(); ++index) {
            auto &destination = index < split ? entry.beforeOnHit : entry.afterOnHit;
            destination.push_back(std::move(attack.targetEffects[index]));
        }
        attack.targetEffects.clear();
        entry.onHit = std::move(attack.onHitApplications);
        attack.onHitApplications.clear();
        work.push_back(std::move(entry));
    }
    auto module = game.module();
    for (auto &entry : work) {
        if (!module) {
            for (auto &effect : entry.beforeOnHit) target.applyEffect(std::move(effect));
            applyItemOnHitApplications(std::move(entry.onHit), target, game, services);
            for (auto &effect : entry.afterOnHit) target.applyEffect(std::move(effect));
            continue;
        }
        for (auto &effect : entry.beforeOnHit)
            enqueueAppliedEffect(*module, game, attacker, target, std::move(effect), impactTime);
        if (!entry.onHit.empty()) {
            SavedWeaponImpact onHit;
            onHit.applications = std::move(entry.onHit);
            onHit.source = SavedObjectReference::fromRuntimeId(attacker.id());
            game.bindSavedObjectReference(onHit.source);
            SavedEventRecord event;
            setEventTime(event, game, impactTime);
            event.eventId = static_cast<uint32_t>(SavedEventType::ItemOnHitSpellImpact);
            event.object = SavedObjectReference::fromRuntimeId(target.id());
            event.caller = onHit.source;
            event.payload = std::move(onHit);
            module->enqueueSaveEvent(std::move(event));
        }
        for (auto &effect : entry.afterOnHit)
            enqueueAppliedEffect(*module, game, attacker, target, std::move(effect), impactTime);
    }
}

std::vector<DeferredCombatFeedback> AttackBuffer::takeDeferredFeedback() {
    std::vector<DeferredCombatFeedback> result;
    for (auto &attack : _attacks) {
        for (auto &feedback : attack.deferredFeedback) result.push_back(std::move(feedback));
        attack.deferredFeedback.clear();
    }
    return result;
}

void AttackBuffer::releaseDeferredFeedback(
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target,
    uint64_t impactTime,
    std::vector<DeferredCombatFeedback> feedback) {

    // Each piece travels as its own feedback-log message from the attacker to
    // the target, delivered at the release's impact.
    auto module = game.module();
    if (!module) {
        showCombatFeedback(game, services, game.getObjectById(attacker.id()), target, feedback);
        return;
    }
    for (const auto &record : feedback) {
        auto message = makeCombatFeedbackMessage(record, target, &attacker);
        SavedEventRecord event;
        setEventTime(event, game, impactTime);
        event.eventId = static_cast<uint32_t>(SavedEventType::FeedbackMessage);
        event.object = SavedObjectReference::fromRuntimeId(target.id());
        event.caller = SavedObjectReference::fromRuntimeId(attacker.id());
        event.payload = std::move(message);
        module->enqueueSaveEvent(std::move(event));
    }
}

void AttackBuffer::signalDischarge(
    const Discharge &discharge,
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    const auto generation = _signalGeneration;
    const uint64_t now = game.worldTimeMilliseconds();
    const uint64_t impactTime = now + static_cast<uint64_t>(discharge.delayMilliseconds);
    auto *targetCreature = dyn_cast<Creature>(&target);
    Attack *attack = discharge.attack >= 0 && static_cast<size_t>(discharge.attack) < _attacks.size()
        ? &_attacks[discharge.attack] : nullptr;
    // The released discharge writes its snapshot into the round's record.
    // Events already queued for earlier shots of this round share that record.
    if (_roundFields) {
        _roundFields->result = static_cast<uint8_t>(discharge.result);
        _roundHistory->type = discharge.attackType;
        _roundFields->weaponAttackType = discharge.weaponAttackType;
        _roundFields->rangedTarget = {discharge.endpoint.x, discharge.endpoint.y, discharge.endpoint.z};
        _roundFields->reactionDelay = static_cast<uint16_t>(discharge.delayMilliseconds);
        if (attack) _roundFields->damage = attack->eventFields->damage;
        else _roundFields->damage.fill(-1);
    }
    _recordRoll = discharge.recordRoll;
    _recordModifier = discharge.recordModifier;
    const uint16_t attackType = discharge.attackType;
    if (attack) {
        // Only a hit shot carries an attack roll and emits event 15.
        attack->signaled = true;
        publishAttacked(*attack,
            _roundHistory ? _roundHistory : attack->history,
            _roundFields ? _roundFields : attack->eventFields,
            game, attacker, target, now);
        addCombatFeedback(game, services, attacker, target, *attack);
    }
    if (attack) {
        reportEffectOutcome(game, services, target, *attack);
        addDeflectionFeedback(game, services, attacker, target, *attack);
    }
    // Every discharge, a miss included, sends the feedback gathered so far,
    // timed to its own impact.
    releaseDeferredFeedback(game, services, attacker, target, impactTime, takeDeferredFeedback());

    SafeProjectileShot shot;
    shot.endpoint = discharge.endpoint;
    shot.delayMilliseconds = static_cast<uint32_t>(discharge.delayMilliseconds);
    shot.hand = discharge.hand;
    shot.special = attackType == static_cast<uint16_t>(FeatType::ImprovedPowerBlast) ||
                   attackType == static_cast<uint16_t>(FeatType::PowerBlast) ||
                   attackType == static_cast<uint16_t>(FeatType::MasterPowerBlast);
    if (isHitLikeRangedResult(discharge.result)) {
        shot.result = discharge.result;
        if (targetCreature && (discharge.result == AttackResultType::Parried ||
                               discharge.result == AttackResultType::ShieldHit)) {
            // A parried or shielded bolt bounces off towards a miss point.
            shot.endpoint = projectileMissLocation(target, attacker, game);
            const int missedBy = projectileDelayMilliseconds(target.position(), shot.endpoint);
            shot.missedByMilliseconds = static_cast<uint32_t>(missedBy);
            // The bounce is written into the round's record, which queued
            // events of this round still read.
            if (_roundFields) {
                _roundFields->rangedTarget = {shot.endpoint.x, shot.endpoint.y, shot.endpoint.z};
                _roundFields->missedBy = static_cast<uint32_t>(missedBy);
            }
            queueProjectileMissImpact(game, services, target, shot.endpoint, now + static_cast<uint64_t>(missedBy));
        } else if (attack && targetCreature && discharge.result == AttackResultType::Deflected) {
            returnDeflected(*attack, game, attacker, *targetCreature,
                            now + 2 * static_cast<uint64_t>(discharge.delayMilliseconds));
        } else if (attack) {
            applyDamage(*attack, attacker, target, game, impactTime);
        }
        if (generation != _signalGeneration) return;
        if (!_targetWorkReleased) {
            _targetWorkReleased = true;
            releaseTargetWork(game, services, attacker, target, impactTime);
        }
    } else {
        shot.result = AttackResultType::Miss;
        queueProjectileMissImpact(game, services, attacker, discharge.endpoint, impactTime);
    }
    if (generation != _signalGeneration) return;

    // Every discharge is shown from the weapon in its hand, without an AI event.
    const auto weapon = discharge.hand == 0 ? attacker.getEquippedItem(InventorySlots::rightWeapon)
                      : discharge.hand == 1 ? attacker.getEquippedItem(InventorySlots::leftWeapon)
                                            : nullptr;
    if (weapon) services.game.projectiles.launchSafeProjectile(attacker, target, *weapon, shot, game, services);
}

size_t AttackBuffer::signalReadyRanged(
    int elapsedMilliseconds,
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    // One discharge per update while the attack pause runs; the pause end
    // releases the rest.
    const bool pauseEnded = elapsedMilliseconds >= kPhysicalAttackPauseMilliseconds;
    const auto generation = _signalGeneration;
    size_t signaled = 0;
    while (generation == _signalGeneration && hasPendingDischarges()) {
        const Discharge discharge = _discharges[_nextDischarge];
        if (!pauseEnded && discharge.timeMilliseconds >= elapsedMilliseconds) break;
        // Consume the discharge before callbacks, which may cancel the round.
        ++_nextDischarge;
        ++signaled;
        signalDischarge(discharge, game, services, attacker, target);
        if (!pauseEnded) break;
    }
    return signaled;
}

void AttackBuffer::prepareMeleeSequence(
    const IAnimations &animations,
    const std::vector<std::string> &attackAnimations) {

    ++_signalGeneration;
    assert(!_attacks.empty() && "Physical attack buffer is empty");
    assert(std::none_of(
               _attacks.begin(),
               _attacks.end(),
               [](const Attack &attack) { return attack.ranged; }) &&
           "Ranged attack in melee sequence");
    assert(attackAnimations.size() == _attacks.size() &&
           "melee attack animation count does not match attack count");

    for (size_t index = 0; index < _attacks.size(); ++index) {
        Attack &attack = _attacks[index];
        attack.impactTimeMilliseconds = _cutsceneHitsAtRelease
            ? 0 : animations.getMeleeImpactTime(attackAnimations[index], index);
        attack.signaled = false;
    }

    _pendingMeleeAttacks = _attacks.size();
    _meleeSequencePrepared = true;
}

size_t AttackBuffer::signalReadyMelee(
    int elapsedMilliseconds,
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    assert(_meleeSequencePrepared &&
           "melee attack sequence is not prepared");
    // Hits are released in attack order, one per update while the attack
    // pause runs, each once its impact time has passed; the pause end
    // releases the rest.
    const bool pauseEnded = elapsedMilliseconds >= kPhysicalAttackPauseMilliseconds;
    const auto generation = _signalGeneration;
    size_t signaled = 0;
    for (size_t index = 0; index < _attacks.size(); ++index) {
        if (generation != _signalGeneration || !hasPendingMelee()) break;
        Attack &attack = _attacks[index];
        if (attack.signaled) continue;
        if (!pauseEnded && attack.impactTimeMilliseconds >= elapsedMilliseconds) break;
        // Consume the pending slot before invoking effects. A callback may
        // cancel the sequence and zero the count.
        --_pendingMeleeAttacks;
        ++signaled;
        signalAttack(attack, game, services, attacker, target);
        // Do not touch the attack or decrement a count after a callback.
        if (!pauseEnded) break;
    }
    return signaled;
}

void AttackBuffer::clearSpecialAttacks() {
    // Released shots keep the type they captured; the records lose it.
    if (_resolvingHistory) _resolvingHistory->type = 0;
    if (_roundHistory) _roundHistory->type = 0;
    for (auto &attack : _attacks) attack.history->type = 0;
}

void AttackBuffer::clearHistory() {
    // Retiring the round clears its attack data in place: undelivered events
    // share these records and read the cleared values when delivered. Pending
    // hits and undischarged shots are dropped with it.
    discardPending();
    if (_resolvingHistory) *_resolvingHistory = AttackHistory {};
    _resolvingHistory.reset();
    if (_roundHistory) *_roundHistory = AttackHistory {};
    if (_roundFields) *_roundFields = AttackEventFields {};
    _recordRoll = 0;
    _recordModifier = 0;
    _recordDeflection = DeflectionBreakdown {};
    for (auto &attack : _attacks) {
        *attack.history = AttackHistory {};
        *attack.eventFields = AttackEventFields {};
    }
}

void AttackBuffer::discardPending() {
    ++_signalGeneration;
    for (Attack &attack : _attacks) {
        attack.signaled = true;
    }
    _pendingMeleeAttacks = 0;
    // Undischarged shots are dropped; released bolts and their queued work remain.
    _discharges.clear();
    _nextDischarge = 0;
}

bool AttackBuffer::hasPendingMelee() const {
    return _meleeSequencePrepared && _pendingMeleeAttacks != 0;
}

static constexpr float kCombatFeedbackRange2 = 900.0f;

static bool canReceiveCombatFeedback(
    const Creature &player,
    const Creature &subject) {

    return player.faction() == subject.faction() &&
           player.getSquareDistanceTo(subject) <= kCombatFeedbackRange2;
}

static constexpr int kStrRefAttackSummary = 42042;
static constexpr int kStrRefAttackSuccessVerb = 42043;
static constexpr int kStrRefAttackFailureVerb = 42044;
static constexpr int kStrRefAttackFeat = 42046;
static constexpr int kStrRefAttackRoll = 42119;
static constexpr int kStrRefAttackRollSuccess = 42133;
static constexpr int kStrRefAttackRollFailure = 42134;
static constexpr int kStrRefAttackBreakdown = 42146;
static constexpr int kStrRefCriticalThreatBreakdown = 42148;
static constexpr int kStrRefDefenseBreakdown = 42149;
static constexpr int kStrRefDamageBreakdown = 42150;
static constexpr int kStrRefStrengthModifier = 42154;
static constexpr int kStrRefOtherDamageBonus = 42155;
static constexpr int kStrRefSneakAttackDamage = 42156;
static constexpr int kStrRefConfirmedCritical = 1511;
static constexpr int kStrRefCriticalThreatConfirmed = 1392;
static constexpr int kStrRefCriticalThreatFailed = 1393;
static constexpr int kStrRefUniversalDamage = 1422;
static constexpr int kStrRefPhysicalDamage = 1423;
static constexpr int kStrRefAcidDamage = 1440;
static constexpr int kStrRefColdDamage = 1441;
static constexpr int kStrRefLightSideDamage = 1442;
static constexpr int kStrRefElectricalDamage = 1443;
static constexpr int kStrRefFireDamage = 1444;
static constexpr int kStrRefDarkSideDamage = 1445;
static constexpr int kStrRefSonicDamage = 1446;
static constexpr int kStrRefIonDamage = 1447;
static constexpr int kStrRefEnergyDamage = 1448;
static constexpr int kStrRefMainhand = 42314;
static constexpr int kStrRefOffhand = 42315;
static constexpr int kStrRefAttackRollComponent = 42316;
static constexpr int kStrRefMeleeOnRangedBonus = 42317;
static constexpr int kStrRefFeatAttackBonus = 42318;
static constexpr int kStrRefCloseProximityRangedBonus = 42330;
static constexpr int kStrRefWeaponFocusBonus = 42331;
static constexpr int kStrRefEffectBonus = 42332;
static constexpr int kStrRefDualWieldPenalty = 42333;
static constexpr int kStrRefSmallOffhandBonus = 42334;
static constexpr int kStrRefDefenseArmor = 42338;
static constexpr int kStrRefDefenseDexterity = 42339;
static constexpr int kStrRefDefenseClass = 42340;
static constexpr int kStrRefDefenseNatural = 42341;
static constexpr int kStrRefDefenseEffects = 42342;
static constexpr int kStrRefDefenseFeat = 42343;
static constexpr int kStrRefWeaponSpecializationDamage = 42363;
static constexpr int kStrRefDexterityModifier = 42375;
static constexpr int kStrRefCriticalDamageMultiplier = 42386;
static constexpr int kStrRefCoupDeGrace = 42303;
static constexpr int kStrRefSneakAttack = 1459;
static constexpr int kStrRefAttackParried = 42411;
static constexpr int kStrRefAttackDeflected = 42421;
static constexpr int kStrRefAutomaticHit = 42390;
static constexpr int kStrRefAutomaticMiss = 42391;
static constexpr int kStrRefAttackEffect = 42030;
static constexpr int kStrRefBaseAttackBonus = 42392;
static constexpr int kStrRefPoisonDamage = 41902;
static constexpr int kStrRefDefenseDebilitated = 42427;
static constexpr int kStrRefImprovedToughnessDamage = 42433;
static constexpr int kStrRefWookieeEnduranceDamage = 42434;
static constexpr int kStrRefDeflectionBreakdown = 42417;
static constexpr int kStrRefDeflectionBaseAttackBonus = 42419;
static constexpr int kStrRefDeflectionDexterity = 42420;
static constexpr int kStrRefDeflectionEffects = 42620;
static constexpr int kStrRefDualStrikeBonus = 48201;
static constexpr int kStrRefPcCharismaBonus = 126694;

static void appendDefenseComponent(
    Game &game,
    ServicesView &services,
    std::string &breakdown,
    int strRef,
    int value) {

    if (value == 0) {
        return;
    }
    breakdown += game.getFeedbackText(strRef,
        {{0, std::to_string(value)}});
}

static void appendDamageComponent(
    Game &game,
    ServicesView &services,
    std::string &breakdown,
    int strRef,
    int value) {

    if (value <= 0) {
        return;
    }
    if (!breakdown.empty()) {
        breakdown += " + ";
    }
    breakdown += game.getFeedbackText(strRef,
        {{0, std::to_string(value)}});
}

static void appendDamageModifier(
    Game &game,
    ServicesView &services,
    std::string &breakdown,
    const char *separator,
    int strRef,
    int value) {

    breakdown += separator;
    breakdown += game.getFeedbackText(strRef,
        {{0, std::to_string(value)}});
}

static std::string getDamageBreakdown(
    Game &game,
    ServicesView &services,
    const DamageBreakdown &values,
    const DamagePacket &damage) {

    const DamageResolution &resolution = damage.resolution();
    int finalDamage = resolution.finalDamage;
    if (finalDamage <= 0) {
        return {};
    }

    const auto &slots = values.rawDamageSlots;
    int physicalDamage = 0;
    for (int slot = 0; slot != 3; ++slot) {
        if (slots[slot] > 0) {
            physicalDamage += slots[slot];
        }
    }

    std::string breakdown;
    appendDamageComponent(
        game,
        services,
        breakdown,
        kStrRefEnergyDamage,
        slots[12]);
    appendDamageComponent(
        game,
        services,
        breakdown,
        kStrRefPhysicalDamage,
        physicalDamage);
    static constexpr std::pair<int, int> kDamageComponents[] {
        {3, kStrRefUniversalDamage},
        {4, kStrRefAcidDamage},
        {5, kStrRefColdDamage},
        {6, kStrRefLightSideDamage},
        {7, kStrRefElectricalDamage},
        {8, kStrRefFireDamage},
        {9, kStrRefDarkSideDamage},
        {10, kStrRefSonicDamage},
        {11, kStrRefIonDamage},
        {13, kStrRefPoisonDamage},
    };
    for (const auto &[slot, strRef] : kDamageComponents) {
        appendDamageComponent(
            game,
            services,
            breakdown,
            strRef,
            slots[slot]);
    }

    if (values.strengthModifier != 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " ",
            kStrRefStrengthModifier,
            values.strengthModifier);
    }
    if (values.weaponSpecialization != 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " ",
            kStrRefWeaponSpecializationDamage,
            values.weaponSpecialization);
    }
    if (values.otherSpecialBonus > 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " + ",
            kStrRefOtherDamageBonus,
            values.otherSpecialBonus);
    }
    if (values.sneakAttack > 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " + ",
            kStrRefSneakAttackDamage,
            values.sneakAttack);
    }

    int improvedToughness = resolution.improvedToughnessBonus;
    if (improvedToughness != 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " - ",
            kStrRefImprovedToughnessDamage,
            improvedToughness);
    }
    int wookieeEndurance = resolution.wookieeEnduranceBonus;
    if (wookieeEndurance != 0) {
        appendDamageModifier(
            game,
            services,
            breakdown,
            " - ",
            kStrRefWookieeEnduranceDamage,
            wookieeEndurance);
    }

    std::string finalText;
    if (values.criticalMultiplier > 0) {
        finalText = game.getFeedbackText(kStrRefCriticalDamageMultiplier,
            {{0, std::to_string(values.criticalMultiplier)}});
    }
    finalText += std::to_string(finalDamage);

    return game.getFeedbackText(kStrRefDamageBreakdown,
        {
            {0, finalText},
            {1, breakdown},
        });
}

static void addDamageBreakdownFeedback(
    Game &game,
    ServicesView &services,
    const DamageBreakdown &values,
    const DamagePacket &damage,
    int broadcasts) {

    if (damage.empty() || !damage.isResolved()) return;

    std::string breakdown = getDamageBreakdown(
        game,
        services,
        values,
        damage);
    if (breakdown.empty()) {
        return;
    }

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            breakdown,
            MessageLog::Buffer::Combat);
    }
}

void AttackBuffer::addCombatFeedback(
    Game &game,
    ServicesView &services,
    const Creature &attacker,
    const Object &target,
    const Attack &attack) const {

    auto leader = game.party().getLeader();
    if (!leader) {
        return;
    }

    bool broadcastFromAttacker = canReceiveCombatFeedback(*leader, attacker);
    const auto *targetCreature = dyn_cast<Creature>(&target);
    bool broadcastFromTarget = targetCreature &&
                               canReceiveCombatFeedback(*leader, *targetCreature);
    int broadcasts = static_cast<int>(broadcastFromAttacker) +
                     static_cast<int>(broadcastFromTarget);
    if (broadcasts == 0) {
        return;
    }

    const std::string &attackerName = attacker.name();
    const std::string &targetName = target.name();
    int defense = attack.defenseBreakdown.total;

    // A cutscene attack reports no modifier, so its total is its zero roll.
    const int attackTotal = attack.roll +
        (_cutscene ? 0 : static_cast<int8_t>(attack.attackBonusBreakdown.total()));

    bool successful = isAttackSuccessful(attack.result);
    std::string feedback = game.getFeedbackText(kStrRefAttackSummary,
        {
            {0, attackerName},
            {1, game.getFeedbackText(
                    successful ? kStrRefAttackSuccessVerb : kStrRefAttackFailureVerb)},
            {2, targetName},
        });
    feedback += ". ";

    // Any attack type names its feat.
    if (attack.history->type != 0) {
        auto feat = services.game.feats.get(static_cast<FeatType>(attack.history->type));
        feedback += game.getFeedbackText(kStrRefAttackFeat,
            {{0, feat ? feat->name : std::string()}});
        feedback += ". ";
    }

    feedback += game.getFeedbackText(kStrRefAttackRoll,
        {
            {0, game.getFeedbackText(
                    successful ? kStrRefAttackRollSuccess : kStrRefAttackRollFailure)},
            {1, std::to_string(attackTotal)},
            {2, std::to_string(defense)},
            {3, std::to_string(
                    (attack.damage.empty() || !attack.damage.isResolved())
                        ? 0
                        : std::max(0, game.scaleDamageForDifficulty(
                                          attack.damage.resolvedDamage(),
                                          target)))},
        });

    // Each suffix is followed by a space. The sneak-attack flag belongs to
    // the round's record.
    auto appendSuffix = [&](int strRef) {
        feedback += game.getFeedbackText(strRef);
        feedback += " ";
    };
    if (attack.criticalThreat.confirmed) appendSuffix(kStrRefConfirmedCritical);
    if (recordSneakAttack(attack)) appendSuffix(kStrRefSneakAttack);
    if (attack.coupDeGrace) appendSuffix(kStrRefCoupDeGrace);
    if (attack.naturalTwenty) {
        appendSuffix(kStrRefAutomaticHit);
    } else if (attack.naturalOne) {
        appendSuffix(kStrRefAutomaticMiss);
    }
    if (attack.result == AttackResultType::Deflected) {
        appendSuffix(kStrRefAttackDeflected);
    } else if (attack.result == AttackResultType::Parried) {
        appendSuffix(kStrRefAttackParried);
    }
    // A feat effect the hit applied closes the line. The summary goes out
    // before the effect meets the target, so it never reads as resisted.
    if (attack.effectCode != 0) {
        const int label = *getEffectOutcomeLabelStrRef(game.isTSL(), attack.effectCode);
        feedback += game.getFeedbackText(kStrRefAttackEffect,
            {{0, targetName}, {1, game.getFeedbackText(label)}});
    }

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Combat,
            feedback,
            MessageLog::Buffer::Combat);
    }

    const AttackBonusBreakdown &bonus = attack.attackBonusBreakdown;
    std::string breakdown = game.getFeedbackText(kStrRefAttackBreakdown,
        {
            {0, game.getFeedbackText(
                    attack.source == Source::Main
                        ? kStrRefMainhand
                        : kStrRefOffhand)},
            {1, std::to_string(attackTotal)},
        });
    breakdown += game.getFeedbackText(kStrRefAttackRollComponent,
        {{0, std::to_string(attack.roll)}});

    if (attack.naturalTwenty) {
        breakdown += " ";
        breakdown += game.getFeedbackText(kStrRefAutomaticHit);
    } else if (attack.naturalOne) {
        breakdown += " ";
        breakdown += game.getFeedbackText(kStrRefAutomaticMiss);
    } else {
        breakdown += game.getFeedbackText(kStrRefBaseAttackBonus,
            {{0, std::to_string(bonus.baseAttackBonus)}});

        if (bonus.dualWieldPenalty != 0) {
            breakdown += game.getFeedbackText(kStrRefDualWieldPenalty,
                {{0, std::to_string(bonus.dualWieldPenalty)}});
        }
        if (bonus.smallOffhandBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefSmallOffhandBonus,
                {{0, std::to_string(bonus.smallOffhandBonus)}});
        }
        if (_feat != FeatType::Invalid && bonus.featBonus != 0) {
            auto feat = services.game.feats.get(_feat);
            breakdown += game.getFeedbackText(kStrRefFeatAttackBonus,
                {
                    {0, feat->name},
                    {1, std::to_string(bonus.featBonus)},
                });
        }
        if (bonus.duelingBonus != 0) {
            auto feat = services.game.feats.get(bonus.duelingFeat);
            breakdown += game.getFeedbackText(kStrRefFeatAttackBonus,
                {
                    {0, feat->name},
                    {1, std::to_string(bonus.duelingBonus)},
                });
        }
        if (bonus.closeProximityRangedBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefCloseProximityRangedBonus,
                {{0, std::to_string(bonus.closeProximityRangedBonus)}});
        }
        if (bonus.meleeOnRangedBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefMeleeOnRangedBonus,
                {{0, std::to_string(bonus.meleeOnRangedBonus)}});
        }
        // The TSL terms follow, each with this attack's own value; a named
        // term reads "name: value".
        auto appendNamedTerm = [&](const std::string &name, int value) {
            breakdown += game.getFeedbackText(kStrRefFeatAttackBonus,
                {{0, name}, {1, std::to_string(value)}});
        };
        auto spellName = [&](SpellType type) {
            auto spell = services.game.spells.get(type);
            assert(spell && "attack bonus power must exist");
            return spell->name;
        };
        if (bonus.dualStrikeBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefDualStrikeBonus,
                {{0, std::to_string(bonus.dualStrikeBonus)}});
        }
        if (bonus.targetingBonus != 0) {
            // The Targeting bonus is the rank of the highest Targeting feat.
            auto feat = services.game.feats.get(static_cast<FeatType>(
                static_cast<int>(FeatType::Targeting1) + bonus.targetingBonus - 1));
            assert(feat && "targeting feat must exist");
            appendNamedTerm(feat->name, bonus.targetingBonus);
        }
        if (bonus.crushOppositionPenalty != 0) {
            appendNamedTerm(spellName(bonus.crushOppositionPenalty == -2
                                          ? SpellType::CrushOppositionVI
                                          : SpellType::CrushOppositionIII),
                bonus.crushOppositionPenalty);
        }
        if (bonus.inspireFollowersBonus != 0) {
            appendNamedTerm(spellName(bonus.inspireFollowersBonus == 2
                                          ? SpellType::InspireFollowersVI
                                          : SpellType::InspireFollowersIII),
                bonus.inspireFollowersBonus);
        }
        if (bonus.leaderCharismaBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefPcCharismaBonus,
                {{0, std::to_string(bonus.leaderCharismaBonus)}});
        }
        if (bonus.formBonus != 0) {
            appendNamedTerm(spellName(static_cast<SpellType>(attacker.currentForm())), bonus.formBonus);
        }
        if (bonus.dexterityModifier != 0) {
            breakdown += game.getFeedbackText(kStrRefDexterityModifier,
                {{0, std::to_string(bonus.dexterityModifier)}});
        } else if (bonus.strengthModifier != 0) {
            breakdown += game.getFeedbackText(kStrRefStrengthModifier,
                {{0, std::to_string(bonus.strengthModifier)}});
        }
        // Weapon Focus includes Superior Weapon Focus.
        const int weaponFocus = bonus.weaponFocusBonus + bonus.superiorWeaponFocusBonus;
        if (weaponFocus != 0) {
            breakdown += game.getFeedbackText(kStrRefWeaponFocusBonus,
                {{0, std::to_string(weaponFocus)}});
        }
        if (bonus.effectBonus != 0) {
            breakdown += game.getFeedbackText(kStrRefEffectBonus,
                {{0, std::to_string(bonus.effectBonus)}});
        }
    }

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            breakdown,
            MessageLog::Buffer::Combat);
    }

    if (attack.criticalThreat.threatened) {
        std::string criticalThreatBreakdown = game.getFeedbackText(kStrRefCriticalThreatBreakdown,
            {
                {0, std::to_string(attack.roll)},
                {1, std::to_string(attack.criticalThreat.threshold)},
                {2, game.getFeedbackText(
                        attack.criticalThreat.confirmed
                            ? kStrRefCriticalThreatConfirmed
                            : kStrRefCriticalThreatFailed)},
                {3, std::to_string(
                        attack.criticalThreat.confirmationRoll +
                        attack.criticalThreat.confirmationBonus +
                        bonus.total())},
                {4, std::to_string(defense)},
            });

        for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
            game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                criticalThreatBreakdown,
                MessageLog::Buffer::Combat);
        }
    }

    if (defense == 0) {
        addDamageBreakdownFeedback(
            game,
            services,
            attack.damageBreakdown,
            attack.damage,
            broadcasts);
        return;
    }

    const DefenseBreakdown &defenseValues = attack.defenseBreakdown;
    std::string defenseBreakdown = game.getFeedbackText(kStrRefDefenseBreakdown,
        {{0, std::to_string(defense)}});
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseArmor,
        defenseValues.armor);
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseDexterity,
        defenseValues.dexterity);
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseClass,
        defenseValues.classDefense);
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseNatural,
        defenseValues.natural);
    // The effects line also carries Total Defense and the lightsaber form.
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseEffects,
        defenseValues.dodgeAndDeflection + defenseValues.stance + defenseValues.form);
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseFeat,
        defenseValues.feat);
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseDebilitated,
        defenseValues.debilitationPenalty);

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            defenseBreakdown,
            MessageLog::Buffer::Combat);
    }

    addDamageBreakdownFeedback(
        game,
        services,
        attack.damageBreakdown,
        attack.damage,
        broadcasts);
}

// A parried, deflected or shielded shot reports the record's last deflection
// roll: the defender, the roll's total and parts, and the shot's score. In
// KotOR every number wraps to 0..255. In TSL the base attack bonus and
// Dexterity do too, and the effects, Deflect and Precise Shot parts wrap to
// -128..127.
void AttackBuffer::addDeflectionFeedback(
    Game &game,
    ServicesView &services,
    const Creature &attacker,
    const Object &target,
    const Attack &attack) const {

    if (attack.result != AttackResultType::Parried && attack.result != AttackResultType::Deflected &&
        attack.result != AttackResultType::ShieldHit) {
        return;
    }
    auto leader = game.party().getLeader();
    if (!leader) return;
    const auto *targetCreature = dyn_cast<Creature>(&target);
    const int broadcasts = static_cast<int>(canReceiveCombatFeedback(*leader, attacker)) +
                           static_cast<int>(targetCreature && canReceiveCombatFeedback(*leader, *targetCreature));
    if (broadcasts == 0) return;

    const bool tsl = game.isTSL();
    auto unsignedByte = [](int value) { return static_cast<int>(static_cast<uint8_t>(value)); };
    auto signedByte = [](int value) { return static_cast<int>(static_cast<int8_t>(value)); };
    const DeflectionBreakdown &record = _recordDeflection;
    const int total = tsl ? record.total : unsignedByte(record.total);
    // A record without a roll reports nothing.
    if (total == 0) return;
    const int attackTotal = tsl ? record.attackTotal : unsignedByte(record.attackTotal);
    const int effects = tsl ? signedByte(record.effects) : unsignedByte(record.effects);

    std::string parts;
    auto appendPart = [&](int strRef, int value) {
        if (value == 0) return;
        parts += game.getFeedbackText(strRef, {{0, std::to_string(value)}});
    };
    auto appendNamedPart = [&](const std::string &name, int value) {
        if (value == 0) return;
        parts += game.getFeedbackText(kStrRefFeatAttackBonus, {{0, name}, {1, std::to_string(value)}});
    };
    auto featName = [&](FeatType type) {
        auto feat = services.game.feats.get(type);
        assert(feat && "deflection feat must exist");
        return feat->name;
    };
    auto spellName = [&](SpellType type) {
        auto spell = services.game.spells.get(type);
        assert(spell && "deflection power must exist");
        return spell->name;
    };
    appendPart(kStrRefAttackRollComponent, unsignedByte(record.roll));
    if (record.jediDefenseBonus != 0)
        appendNamedPart(featName(record.jediDefenseFeat), unsignedByte(record.jediDefenseBonus));
    if (record.deflectFeatBonus != 0)
        appendNamedPart(featName(FeatType::Deflect), signedByte(record.deflectFeatBonus));
    if (record.shooterFeatPenalty != 0)
        appendNamedPart(featName(record.shooterFeat), signedByte(record.shooterFeatPenalty));
    if (record.redirectionBonus != 0)
        appendNamedPart(spellName(SpellType::ForceRedirection), unsignedByte(record.redirectionBonus));
    if (record.formBonus != 0)
        appendNamedPart(spellName(static_cast<SpellType>(record.form)), record.formBonus);
    appendPart(kStrRefDeflectionBaseAttackBonus, unsignedByte(record.baseAttackBonus));
    appendPart(kStrRefDeflectionDexterity, unsignedByte(record.dexterity));
    appendPart(kStrRefDeflectionEffects, effects);

    const std::string text = game.getFeedbackText(kStrRefDeflectionBreakdown,
        {
            {0, target.name()},
            {1, std::to_string(total)},
            {2, parts},
            {3, std::to_string(attackTotal)},
        });
    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            text,
            MessageLog::Buffer::Combat);
    }
}

AttackResultType AttackBuffer::result() const {
    AttackResultType sorted[] {
        AttackResultType::Invalid,
        AttackResultType::Miss,
        AttackResultType::AttackResisted,
        AttackResultType::AttackFailed,
        AttackResultType::Parried,
        AttackResultType::ShieldHit,
        AttackResultType::Deflected,
        AttackResultType::HitSuccessful,
        AttackResultType::CriticalHit,
        AttackResultType::AutomaticHit,
    };
    ArrayRef<AttackResultType> sortedByScore(sorted);

    unsigned bestIndex = 0;

    for (const Attack &attack : _attacks) {
        for (unsigned i = 0; i < sortedByScore.size(); ++i) {
            if (sortedByScore[i] == attack.result) {
                bestIndex = std::max(bestIndex, i);
            }
        }
    }

    return sortedByScore[bestIndex];
}

AttackSchedule::State AttackSchedule::update(
    const CombatRound &round, Action &action, float dt) {

    if (round.suspends(action)) return _state;
    dt = round.actionDelta(dt);
    _time += dt;
    if (clocked() && _state != AttackSchedule::WaitAttack) {
        float elapsedMilliseconds =
            dt * 1000.0f + _elapsedRemainderMilliseconds;
        int wholeMilliseconds = static_cast<int>(elapsedMilliseconds);
        _elapsedRemainderMilliseconds =
            elapsedMilliseconds - wholeMilliseconds;
        _elapsedMilliseconds += wholeMilliseconds;
    }

    switch (_state) {
    case AttackSchedule::WaitAttack: {
        if (round.canExecute(action)) {
            _state = AttackSchedule::Attack;
        }
        break;
    }
    case AttackSchedule::Attack: {
        if (clocked() &&
            _elapsedMilliseconds >= _completionMilliseconds) {
            _state = AttackSchedule::Damage;
        } else {
            _state = AttackSchedule::WaitDamage;
        }
        break;
    }
    case AttackSchedule::WaitDamage: {
        if ((clocked() &&
             _elapsedMilliseconds >= _completionMilliseconds) ||
            (!clocked() && _time >= kAttackDamageDelay)) {
            _state = AttackSchedule::Damage;
        }
        break;
    }
    case AttackSchedule::Damage: {
        _state = AttackSchedule::WaitFinish;
        break;
    }
    case AttackSchedule::WaitFinish: {
        if (round.state == CombatRound::Finished) {
            _state = AttackSchedule::Finish;
        }
        break;
    }
    case AttackSchedule::Finish: {
        break;
    }
    }

    return _state;
}

void AttackSchedule::startClock(int completionMilliseconds) {
    assert(_state == AttackSchedule::Attack &&
           "physical attack schedule has not started");

    _elapsedMilliseconds = 0;
    _completionMilliseconds = completionMilliseconds;
    _elapsedRemainderMilliseconds = 0.0f;
}

void AttackSchedule::startMelee() {
    // Hits are released on the attack pause clock; its end releases the rest.
    _melee = true;
    startClock(kPhysicalAttackPauseMilliseconds);
}

void AttackSchedule::startRanged() {
    // Discharges are released on the attack pause clock.
    _ranged = true;
    startClock(kPhysicalAttackPauseMilliseconds);
}

static bool isChoreographedExchange(const Creature &attacker, const Creature &target);

AttackApproachStep approachAttackTarget(Creature &attacker, Object &target, float dt, AttackApproach &approach,
                                        Game &game, const Action &parent, bool mayLeap) {
    if (approach.reached) return AttackApproachStep::Reached;

    auto module = game.module();
    auto area = module ? module->area() : nullptr;
    auto *targetCreature = dyn_cast<Creature>(&target);
    const bool door = target.type() == ObjectType::Door;
    const bool usedObject = door || target.type() == ObjectType::Placeable;
    const bool choreographed = targetCreature && isChoreographedExchange(attacker, *targetCreature);
    const float maxRange = attacker.maxAttackRange(target, choreographed);
    float desired = attacker.desiredAttackRange(target, choreographed);
    // A door or placeable is attacked at the attacker's use point for it.
    const glm::vec3 &targetPosition = target.position();
    const glm::vec3 targetPoint = usedObject ? attacker.useRange(target).point : targetPosition;

    // Nothing is in sight outside an area.
    bool sight = false;
    if (area) {
        const glm::vec3 eye(attacker.position() + glm::vec3(0.0f, 0.0f, kAttackEyeHeight + kAttackEyeOffset));
        const glm::vec3 targetEye(targetPoint + glm::vec3(0.0f, 0.0f, kAttackEyeHeight - kAttackEyeOffset));
        // A ranged attacker sees through a see-through door.
        const auto rightHand = attacker.getEquippedItem(InventorySlots::rightWeapon);
        sight = area->isEyeLineClear(eye, targetEye, &attacker, &target, rightHand && rightHand->isRanged());
        if (sight) {
            attacker.clearBlockedAttackSight();
        } else if (attacker.noteBlockedAttackSight() && game.party().getLeader().get() == &attacker) {
            game.addFeedbackMessage(game.isTSL() ? kNoAttackSightStrRefTSL : kNoAttackSightStrRef);
            attacker.clearBlockedAttackSight();
            attacker.clearCurrentAttackTarget();
            auto enemy = Combat::findNearestEnemy(*area, attacker, attacker,
                                                  attacker.maxAttackRange(attacker, false, true), &target);
            if (!enemy) {
                attacker.cancelAllCombatModes();
                return AttackApproachStep::Ended;
            }
            game.combat().takeNewAttackTarget(attacker, enemy);
            approach.holdsGround = true;
            return AttackApproachStep::Approaching;
        }
    }

    const glm::vec2 toTarget(glm::vec2(targetPoint) - glm::vec2(attacker.position()));
    const float distance2 = glm::dot(toTarget, toTarget);
    // Standing nearer than the desired distance, the attacker makes for the
    // point that distance from the target on its own side, or for the wall
    // that cuts the way there, and then wants only to be that far away. In
    // TSL a door is always attacked from beyond its use point, twice the
    // desired distance out from the door, and the attacker makes for that
    // point until it is within the desired distance of it.
    const bool doorStandOff = door && game.isTSL();
    glm::vec3 standOff(targetPoint);
    float standOffDistance2 = 0.0f;
    if (distance2 < desired * desired - kAttackStandOffAllowance || doorStandOff) {
        if (doorStandOff) {
            const glm::vec3 outward(targetPoint - targetPosition);
            const float length = glm::length(outward);
            standOff = targetPoint + (length < 1e-9f ? glm::vec3(0.0f) : outward / length) * (2.0f * desired);
        } else {
            const float length = glm::length(toTarget);
            const glm::vec2 direction(length < 1e-9f ? glm::vec2(0.0f) : toTarget / length);
            standOff = glm::vec3(glm::vec2(targetPosition) - direction * desired, targetPosition.z);
        }
        // A wall or a creature on the way cuts the way short where it stands.
        glm::vec3 wall;
        if (area && area->testDirectLine(attacker, attacker.position(), standOff, nullptr, nullptr, &wall) !=
                        Area::DirectLine::Clear) {
            standOff = wall;
            desired = glm::distance(targetPosition, standOff);
        }
        if (doorStandOff) {
            const glm::vec2 toStandOff(glm::vec2(standOff) - glm::vec2(attacker.position()));
            standOffDistance2 = glm::dot(toStandOff, toStandOff);
        }
    }

    const float reach = maxRange + (game.isTSL() ? kAttackReachAllowance : 0.0f);
    const bool withinReach = distance2 <= reach * reach;
    // In TSL an attacker that cannot move takes up its attack on a creature
    // where it stands, and swings only at one within reach.
    if (game.isTSL() && targetCreature && attacker.isImmobile()) {
        approach.reached = true;
        approach.withoutSwing = distance2 >= reach * reach || boost::iequals(attacker.tag(), kNonSwingingTurretTag);
        return AttackApproachStep::Reached;
    }

    // A party member may attack from nearer than the desired distance. An
    // attacker short of a door's stand-off point is as good as too near.
    const bool nearer = distance2 < desired * desired - kAttackStandOffAllowance;
    const bool shortOfStandOff = doorStandOff && standOffDistance2 > desired * desired;
    const bool tooClose = nearer || shortOfStandOff;
    // The target must be in the attacker's own area.
    const bool sameArea = target.spatialArea() == attacker.spatialArea();
    if (sameArea && withinReach && (!nearer || attacker.isPartyMember()) && sight && !shortOfStandOff) {
        attacker.setMovementType(Creature::MovementType::None);
        attacker.clearPath();
        approach.reached = true;
        return AttackApproachStep::Reached;
    }

    if (approach.holdsGround) {
        attacker.cancelAllCombatModes();
        return AttackApproachStep::Ended;
    }
    // A target in no area is in transit between areas. Only a creature is
    // followed there, and not by one an encounter spawned; it is sought where
    // it was last seen.
    if (!target.spatialArea() && (!targetCreature || attacker.isEncounterCreature())) {
        attacker.cancelAllCombatModes();
        return AttackApproachStep::Ended;
    }
    if (mayLeap && sight) {
        approach.forceJump = resolveForceJumpAttack(attacker, target);
        if (approach.forceJump != FeatType::Invalid) {
            approach.reached = true;
            return AttackApproachStep::Reached;
        }
    }
    if (!area) return AttackApproachStep::Approaching;
    // In TSL a party member set to fight at range or stand its ground, unless
    // controlled, only turns to face a target it is not too near.
    const NPCAIStyle style = attacker.aiStyle();
    if (!tooClose && game.isTSL() && (style == NPCAIStyle::PartyRanged || style == NPCAIStyle::PartyStationary) &&
        game.party().getLeader().get() != &attacker) {
        attacker.setDesiredFacingToward(targetPosition);
        return AttackApproachStep::Approaching;
    }
    // A party member moving to attack from beyond half a metre says so.
    if (attacker.isPartyMember() && distance2 > kMovingToAttackMessageDistance2) {
        game.party().setCombatMessage(attacker, kMovingToAttackCombatMessage);
    }
    if (tooClose) {
        // Backing off is walked; a door's stand-off point is run to from
        // farther than the square root of three metres.
        attacker.navigateTo(standOff, doorStandOff && standOffDistance2 > kDoorStandOffRunDistance2, 0.0f, dt);
        return AttackApproachStep::Approaching;
    }
    // A ranged attacker closes to its reach, a melee one to the desired
    // distance; one already that near turns toward the target and waits for
    // sight where it stands. A door
    // or placeable is closed on to the attacker's use range for it at its use
    // point, which for a door, the target of the attacker's own path, is the
    // desired distance and the attacker's personal space.
    auto weapon = attacker.getEquippedItem(InventorySlots::rightWeapon);
    const bool ranged = weapon && weapon->isRanged();
    float closeTo = ranged ? maxRange : desired;
    if (door) {
        closeTo = attacker.desiredAttackRange(target) + attacker.personalSpace();
    } else if (usedObject) {
        closeTo = attacker.useRange(target).range;
    }
    if (distance2 <= closeTo * closeTo) {
        attacker.setDesiredFacingToward(targetPosition);
        return AttackApproachStep::Approaching;
    }
    // Navigation actions inherit the parent action group and use the existing
    // range and pathfinding rules. The move ends once the target is within the
    // attacker's use range for it lengthened to its reach, in a clear line. A
    // parent removed by a callback must not continue navigating off-queue.
    if (auto targetObject = game.getObjectById(target.id())) {
        auto move = game.newAction<MoveToObjectAction>(
            std::move(targetObject), true, closeTo, false, -1.0f,
            !ranged, maxRange);
        attacker.addActionBefore(parent, std::move(move));
    }
    return AttackApproachStep::Approaching;
}

FeatType resolveForceJumpAttack(Creature &attacker, Object &target) {
    auto *targetCreature = dyn_cast<Creature>(&target);
    if (!targetCreature) return FeatType::Invalid;
    const glm::vec3 offset = attacker.position() - target.position();
    if (glm::dot(offset, offset) < kForceJumpMinimumDistance2) return FeatType::Invalid;
    if (!attacker.hasEffectiveFeat(FeatType::ForceJump)) return FeatType::Invalid;
    const auto right = attacker.getEquippedItem(InventorySlots::rightWeapon);
    const auto left = attacker.getEquippedItem(InventorySlots::leftWeapon);
    if (!(right && right->isLightsaber()) && !(left && left->isLightsaber())) return FeatType::Invalid;
    auto module = attacker.game().module();
    auto area = module ? module->area() : nullptr;
    if (!area) return FeatType::Invalid;
    // Only the target itself may stand on the straight line to it.
    if (area->testDirectLine(attacker, attacker.position(), target.position(), nullptr, targetCreature) !=
        Area::DirectLine::Clear) return FeatType::Invalid;
    attacker.applyEffect(attacker.game().newEffect<ForceJumpEffect>(attacker.game().getObjectById(target.id()), 0),
                         DurationType::Instant);
    if (attacker.hasEffectiveFeat(FeatType::ForceJumpMastery)) return FeatType::ForceJumpMastery;
    if (attacker.hasEffectiveFeat(FeatType::ForceJumpAdvanced)) return FeatType::ForceJumpAdvanced;
    return FeatType::ForceJump;
}

bool isCreatureCombat(
    const Creature &attacker,
    const Object &target) {

    if (attacker.modelType() == Creature::ModelType::Creature) {
        return true;
    }

    const auto *targetCreature = dyn_cast<Creature>(&target);
    return targetCreature &&
           targetCreature->modelType() == Creature::ModelType::Creature;
}

static bool hasAnim(const graphics::Model &model, const std::string &anim) {
    if (model.animations().count(anim)) {
        return true;
    }

    if (std::shared_ptr<graphics::Model> super = model.superModel()) {
        return hasAnim(*super, anim);
    }

    return false;
}

static bool modelHasAnimation(const Creature &creature, const std::string &name) {
    auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(creature.sceneNode());
    return model && !name.empty() && hasAnim(model->model(), name);
}

static int pickRow(std::initializer_list<int> rows) {
    return *(rows.begin() + randomInt(0, static_cast<int>(rows.size()) - 1));
}

// A choreographed exchange needs a held weapon that is neither ranged, a
// stun baton nor of the unarmed class.
static bool holdsChoreographedWeapon(const Creature &creature) {
    const auto weapon = creature.getEquippedItem(InventorySlots::rightWeapon);
    if (!weapon || weapon->isRanged()) return false;
    const int wield = static_cast<int>(weapon->weaponWield());
    return wield != 1 && wield != 8;
}

static bool isChoreographedExchange(const Creature &attacker, const Creature &target) {
    const bool attackerArmed = static_cast<bool>(attacker.getEquippedItem(InventorySlots::rightWeapon));
    const bool targetArmed = static_cast<bool>(target.getEquippedItem(InventorySlots::rightWeapon));
    if (attackerArmed && targetArmed) return holdsChoreographedWeapon(attacker) && holdsChoreographedWeapon(target);
    return !attackerArmed && !targetArmed &&
           attacker.hasEffectiveFeat(FeatType::ComplexUnarmedAnims) &&
           target.hasEffectiveFeat(FeatType::ComplexUnarmedAnims);
}

bool usesEngagedAttackPlaceholder(const Creature &attacker, const Object &target, bool engaged) {
    if (!engaged) return false;
    if (isCreatureCombat(attacker, target)) return true;
    const auto *targetCreature = dyn_cast<Creature>(&target);
    return targetCreature && isChoreographedExchange(attacker, *targetCreature);
}

// The special attack type a swing shows: the feat with a held weapon, feat
// 220, or an unarmed feat for a holder of the complex unarmed animations.
static uint16_t presentedAttackType(const Creature &attacker, FeatType feat) {
    if (feat == FeatType::Invalid) return 0;
    const auto type = static_cast<uint16_t>(feat);
    if (attacker.getEquippedItem(InventorySlots::rightWeapon)) return type;
    if (type == 220) return type;
    return attacker.hasEffectiveFeat(FeatType::ComplexUnarmedAnims) ? type : 0;
}

// The row of a melee swing for the attack variant \p idx. The random tables
// are drawn on every call.
static int meleeAttackRow(const Creature &attacker, const Object &target, bool engagedPlaceholder,
                          uint16_t attackType, int idx, const IAnimations &animations) {
    const bool tsl = attacker.game().isTSL();
    const bool creatureModel = attacker.modelType() == Creature::ModelType::Creature;
    const bool creatureCombat = isCreatureCombat(attacker, target);
    auto wield = attacker.getWieldType();
    if (wield == CreatureWieldType::HandToHandComplex) wield = CreatureWieldType::HandToHand;
    const bool complexUnarmed = tsl && attacker.hasEffectiveFeat(FeatType::ComplexUnarmedAnims);

    if (attackType != 0 && !creatureModel) {
        auto family = [&](int baton, std::initializer_list<int> single, std::initializer_list<int> twin,
                          std::initializer_list<int> dual, std::initializer_list<int> unarmed) {
            switch (wield) {
            case CreatureWieldType::StunBaton: return baton;
            case CreatureWieldType::SingleSword: return tsl ? pickRow(single) : *single.begin();
            case CreatureWieldType::DoubleBladedSword: return tsl ? pickRow(twin) : *twin.begin();
            case CreatureWieldType::DualSwords: return tsl ? pickRow(dual) : *dual.begin();
            case CreatureWieldType::HandToHand: return tsl ? pickRow(unarmed) : 0;
            default: return 0;
            }
        };
        switch (attackType) {
        case 8: case 19: case 81:
            return family(87, {113, 392, 393}, {154, 413, 414}, {195, 434, 435}, {494, 495, 496});
        case 11: case 53: case 91:
            return family(88, {114, 394, 395}, {155, 415, 416}, {196, 436, 437}, {497, 498, 499});
        case 17: case 28: case 83:
            return family(87, {115, 396, 397}, {156, 417, 418}, {197, 438, 439}, {500, 501, 502});
        case 101: case 102: case 103:
            switch (wield) {
            case CreatureWieldType::SingleSword: return 386;
            case CreatureWieldType::DoubleBladedSword: return 387;
            case CreatureWieldType::DualSwords: return 388;
            default: return 0;
            }
        case 220:
            return tsl ? 562 : 0;
        default:
            return 0;
        }
    }

    if (creatureModel) {
        if (engagedPlaceholder && modelHasAnimation(attacker, animations.getNameById(282))) return 282 + idx;
        return 276 + idx;
    }
    if (!engagedPlaceholder) {
        switch (wield) {
        case CreatureWieldType::StunBaton: return 87 + idx;
        case CreatureWieldType::SingleSword: return tsl ? pickRow({122, 123, 410, 411, 412}) : 122 + idx;
        case CreatureWieldType::DoubleBladedSword: return tsl ? pickRow({163, 164, 431, 432, 433}) : 163 + idx;
        case CreatureWieldType::DualSwords: return tsl ? pickRow({204, 205, 452, 453, 454}) : 204 + idx;
        case CreatureWieldType::HandToHand: return complexUnarmed ? pickRow({521, 522, 523, 524, 525}) : 247 + idx;
        default: return 0;
        }
    }
    if (creatureCombat) {
        switch (wield) {
        case CreatureWieldType::StunBaton: return 87 + idx;
        case CreatureWieldType::SingleSword: return 125 + idx;
        case CreatureWieldType::DoubleBladedSword: return 166 + idx;
        case CreatureWieldType::DualSwords: return 207 + idx;
        case CreatureWieldType::HandToHand: return complexUnarmed ? 527 + idx : 247 + idx;
        default: return 0;
        }
    }
    switch (wield) {
    case CreatureWieldType::StunBaton: return 87 + idx % 2;
    case CreatureWieldType::SingleSword: return 94 + idx;
    case CreatureWieldType::DoubleBladedSword: return 135 + idx;
    case CreatureWieldType::DualSwords: return 176 + idx;
    case CreatureWieldType::HandToHand: {
        if (!complexUnarmed) return 247 + idx % 2;
        const int level = attacker.attributes().getAggregateLevel();
        const int count = level > 9 ? 5 : level > 6 ? 4 : level > 3 ? 3 : 2;
        return 477 + idx % count;
    }
    default: return 0;
    }
}

std::optional<PhysicalAttackSwing> beginPhysicalAttack(
    Creature &attacker, Object &target,
    const IAnimations &animations, AttackBuffer &attacks, FeatType feat, bool engages, bool swings) {

    // The exchange is engaged, and its partner held, before the round is rolled.
    const bool engaged = engages && attacker.game().combat().engagePhysicalTarget(
        attacker, target, kPhysicalAttackPauseMilliseconds);
    PhysicalAttackSwing swing;
    swing.engaged = engaged;
    // An attack taken up without a swing rolls nothing.
    if (!swings) return swing;
    if (!attacks.addPhysicalAttacks(attacker, target, feat)) return std::nullopt;
    // A ranged main-hand weapon makes the swing ranged, whatever the off hand holds.
    const auto main = attacker.getEquippedItem(InventorySlots::rightWeapon);
    swing.ranged = main && main->isRanged();
    // A ranged animation plays even when its row resolves no discharge.
    if (!swing.ranged && attacks.attackCount() == 0) return swing;

    swing.engagedPlaceholder = usesEngagedAttackPlaceholder(attacker, target, engaged);
    const uint16_t attackType = presentedAttackType(attacker, feat);
    swing.attackType = attackType;
    if (swing.ranged) {
        swing.animations.push_back(animations.getNameById(static_cast<uint32_t>(rangedAttackAnimation(
            attackType, attacker.getWieldType(), attacker.modelType() == Creature::ModelType::Creature))));
        swing.clip = swing.animations.front();
        return swing;
    }
    // A cutscene attack swings its forced row, times every hit by it and shows it.
    if (attacks.cutscene()) {
        const auto forced = animations.getNameById(static_cast<uint32_t>(attacks.cutscene()->animation));
        swing.animations.assign(attacks.attackCount(), forced);
        swing.clip = forced;
        return swing;
    }
    // Each logical melee attack draws its own variant and row, which only
    // time its hit. The one swing shown takes the variant the last attack
    // drew, its random table drawn again.
    const bool cinematic = swing.engagedPlaceholder && !isCreatureCombat(attacker, target);
    int variant = 0;
    for (size_t index = 0; index < attacks.attackCount(); ++index) {
        variant = attacker.selectMeleeAttackVariant(cinematic) - 1;
        const int row = meleeAttackRow(attacker, target, swing.engagedPlaceholder, attackType, variant, animations);
        swing.animations.push_back(animations.getNameById(static_cast<uint32_t>(row)));
    }
    swing.clip = animations.getNameById(static_cast<uint32_t>(
        meleeAttackRow(attacker, target, swing.engagedPlaceholder, attackType, variant, animations)));
    return swing;
}

uint16_t resolveAttackReaction(const Creature *target, bool ranged, AttackResultType result,
                               bool anyHit, bool engaged, bool targetEngaged,
                               bool targetHasRoom, bool forced, bool &committed) {
    committed = false;
    if (!target) return 10001;
    const bool debilitated = target->isUnableToReact();
    uint16_t reaction = 10001;
    if (ranged) {
        bool saber = false;
        if (const auto weapon = target->getEquippedItem(InventorySlots::rightWeapon)) saber = weapon->isLightsaber();
        if (const auto weapon = target->getEquippedItem(InventorySlots::leftWeapon)) saber = saber || weapon->isLightsaber();
        if (!anyHit && !saber && targetEngaged) reaction = 10011;
        if (debilitated) return 10001;
        // A reaction without room or engagement stays on the record uncommitted.
        committed = targetHasRoom && engaged;
        return reaction;
    }
    switch (result) {
    case AttackResultType::HitSuccessful:
    case AttackResultType::CriticalHit:
    case AttackResultType::AutomaticHit:
        reaction = 10014;
        break;
    case AttackResultType::Parried:
    case AttackResultType::Deflected:
    case AttackResultType::ShieldHit:
        reaction = 10012;
        break;
    case AttackResultType::Miss:
    case AttackResultType::AttackResisted:
    case AttackResultType::AttackFailed:
        reaction = 10011;
        break;
    default:
        break;
    }
    // A forced attack skips the room test.
    if (debilitated || !(targetHasRoom || forced) || !engaged) return 10001;
    committed = true;
    return reaction;
}

// The rate that stretches the attacker's swing over the round's animation
// length.
static float swingRate(const Creature &attacker, const std::string &swing) {
    if (auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(attacker.sceneNode())) {
        if (auto anim = model->model().getAnimation(swing))
            return anim->length() * 1000.0f / static_cast<float>(kPhysicalAttackPauseMilliseconds);
    }
    return 0.0f;
}

void showAttackReaction(Creature &attacker, Object &target, const std::string &swing,
                        uint16_t reaction, bool engagedPlaceholder, bool ranged,
                        const IAnimations &animations) {
    if (reaction == 10001) return;
    auto *targetCreature = dyn_cast<Creature>(&target);
    if (!targetCreature) return;
    // The reaction column follows the target's wield: none for a creature
    // model; the unarmed column becomes the complex one in TSL when the
    // attacker has the complex unarmed animations.
    CreatureWieldType wield = targetCreature->modelType() == Creature::ModelType::Creature
        ? CreatureWieldType::None : targetCreature->getWieldType();
    if (wield == CreatureWieldType::HandToHand || wield == CreatureWieldType::HandToHandComplex) {
        wield = attacker.game().isTSL() && attacker.hasEffectiveFeat(FeatType::ComplexUnarmedAnims)
            ? CreatureWieldType::HandToHandComplex : CreatureWieldType::HandToHand;
    }
    const auto clip = animations.getReactionAnimation(swing, wield, reaction);
    if (clip.empty()) return;
    // The reaction lasts the round's animation length at the rate that
    // stretches the attacker's swing over it.
    const float rate = swingRate(attacker, swing);
    // An engaged melee exchange ends the target's current one-shot and takes
    // its layers off first; otherwise the reaction is queued only over a
    // looping animation.
    if (!ranged && engagedPlaceholder) {
        targetCreature->abortFireForgetAnimation(true);
    } else if (!targetCreature->currentClipLoops()) {
        return;
    }
    targetCreature->addFireForgetAnimation(clip, kPhysicalAttackPauseMilliseconds, rate, false);
}

// The visual effect a special attack shows on its weapons, by the weapon
// class: the melee feats show one for a single blade or two blades and
// another for a double blade, and nothing with any other weapon; the ranged
// feats show theirs whatever the weapon. The first ranks of Power Attack,
// Critical Strike, Flurry, Power Blast and Rapid Shot show none, nor does
// any other attack.
static int specialAttackVisual(uint16_t attackType, int weaponClass) {
    auto byClass = [weaponClass](int blade, int doubleBlade) {
        if (weaponClass == 3) return doubleBlade;
        return weaponClass == 2 || weaponClass == 4 ? blade : 0;
    };
    switch (static_cast<FeatType>(attackType)) {
    case FeatType::ImprovedPowerAttack: return byClass(4027, 4012);
    case FeatType::MasterPowerAttack: return byClass(4028, 4019);
    case FeatType::ImprovedCriticalStrike: return byClass(4025, 4014);
    case FeatType::MasterCriticalStrike: return byClass(4026, 4018);
    case FeatType::ImprovedFlurry: return byClass(4030, 4021);
    case FeatType::MasterFlurry: return byClass(4031, 4017);
    case FeatType::ImprovedPowerBlast: return 4013;
    case FeatType::MasterPowerBlast: return 4029;
    case FeatType::SniperShot: return 4038;
    case FeatType::ImprovedSniperShot: return 4015;
    case FeatType::MasterSniperShot: return 4020;
    case FeatType::ImprovedRapidShot: return 4022;
    case FeatType::MultiShot: return 4016;
    default: return 0;
    }
}

SpecialAttackVisuals::~SpecialAttackVisuals() { clear(); }

void SpecialAttackVisuals::detach(Attached &attached) {
    if (attached.model && attached.hook) attached.hook->removeChild(*attached.model);
    attached = Attached {};
}

static constexpr const char *kSpecialAttackHands[] = {"rhand", "lhand"};
static constexpr int kSpecialAttackSlots[] = {InventorySlots::rightWeapon, InventorySlots::leftWeapon};

// The effect's impact model hangs from the weapon's bullet hook for a ranged
// attack and from the weapon's root otherwise.
static scene::ModelNodeSceneNode *specialAttackHook(scene::ModelSceneNode &weapon, bool ranged) {
    return ranged ? weapon.getNodeByName("bullethook") : weapon.getNodeByNumber(weapon.model().rootNode()->number());
}

// A weapon without its node shows nothing.
void SpecialAttackVisuals::show(Creature &attacker, uint16_t attackType, bool ranged) {
    const int weaponClass = attacker.getReadyWeaponClass();
    const int row = specialAttackVisual(attackType, weaponClass);
    if (row == 0) return;
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(attacker.sceneNode());
    if (!body) return;
    const auto desc = attacker.services().game.visualEffects.get(static_cast<uint32_t>(row));
    if (!desc || !(*desc)->impactModel) return;
    const bool bothHands = weaponClass == 4 || weaponClass == 6;
    for (size_t hand = 0; hand < (bothHands ? 2 : 1); ++hand) {
        auto item = attacker.getEquippedItem(kSpecialAttackSlots[hand]);
        auto *weapon = dynamic_cast<scene::ModelSceneNode *>(body->getAttachment(kSpecialAttackHands[hand]));
        if (!item || !weapon) continue;
        auto &attached = _hands[hand];
        detach(attached);
        auto *hook = specialAttackHook(*weapon, ranged);
        if (!hook) continue;
        attached.weapon = item;
        attached.ranged = ranged;
        attached.hook = hook;
        attached.model = body->graph().newModel(*(*desc)->impactModel, scene::ModelUsage::Projectile);
        hook->addChild(*attached.model);
        attached.model->playAnimation("impact");
    }
}

void SpecialAttackVisuals::clear() {
    for (auto &attached : _hands) detach(attached);
}

void SpecialAttackVisuals::update() {
    for (auto &attached : _hands)
        if (attached.model && attached.model->isAnimationFinished()) detach(attached);
}

void SpecialAttackVisuals::detachFromBody() {
    for (auto &attached : _hands) {
        if (attached.model && attached.hook) attached.hook->removeChild(*attached.model);
        attached.hook = nullptr;
    }
}

void SpecialAttackVisuals::reattachToBody(const Creature &attacker, scene::ModelSceneNode &body) {
    for (size_t hand = 0; hand < _hands.size(); ++hand) {
        auto &attached = _hands[hand];
        if (!attached.model) continue;
        auto item = attacker.getEquippedItem(kSpecialAttackSlots[hand]);
        auto *weapon = dynamic_cast<scene::ModelSceneNode *>(body.getAttachment(kSpecialAttackHands[hand]));
        attached.hook = item && item == attached.weapon.lock() && weapon ? specialAttackHook(*weapon, attached.ranged)
                                                                          : nullptr;
        if (attached.hook) {
            attached.hook->addChild(*attached.model);
        } else {
            detach(attached);
        }
    }
}

void presentPhysicalAttack(
    Creature &attacker, Object &target,
    const IAnimations &animations, AttackBuffer &attacks,
    const PhysicalAttackSwing &swing) {

    auto *targetCreature = dyn_cast<Creature>(&target);
    auto &combat = attacker.game().combat();
    // A partner engaged by this attack takes part in an engaged exchange.
    const uint16_t reaction = attacks.resolveReaction(targetCreature, swing.engaged,
        targetCreature && (swing.engaged || combat.isEngaged(*targetCreature)),
        !targetCreature || combat.hasReactionRoom(*targetCreature, attacker), attacks.cutscene().has_value());
    if (swing.animations.empty()) return;
    // The round's one swing ends the attacker's one-shot and takes its layers
    // off, then waits in its queue to last the round's animation length,
    // carrying the attack its hit shows.
    attacker.markAnimationChosen();
    attacker.abortFireForgetAnimation(true);
    attacker.addFireForgetAnimation(swing.clip, kPhysicalAttackPauseMilliseconds, swingRate(attacker, swing.clip),
        false, AnimationSource(),
        SwingAttack {target.id(), attacks.recordResult(), swing.ranged,
                     static_cast<PhysicalAttackKind>(attacks.recordWeaponAttackType()), attacks.recordKillingBlow()});
    // A special attack's visual on the weapons starts with the swing.
    attacker.specialAttackVisuals().show(attacker, swing.attackType, swing.ranged);
    // A melee attacker grunts as it swings: the leader always, anyone else
    // one time in five.
    if (!swing.ranged && (attacker.game().party().getLeader().get() == &attacker || randomInt(0, 4) == 0)) {
        attacker.playSound(static_cast<resource::SoundSetEntry>(
            static_cast<int>(resource::SoundSetEntry::AttackGrunt1) + randomInt(0, 2)));
    }
    // An engaged exchange blocks the leader's rest pose drive until its next
    // swing that is not engaged, or until it moves by input.
    attacker.setEngagedExchange(swing.engagedPlaceholder);
    // Its one reaction is paired with the swing.
    showAttackReaction(attacker, target, swing.clip, reaction,
        swing.engagedPlaceholder, swing.ranged, animations);
}

DamageBreakdown::DamageBreakdown() {
    rawDamageSlots.fill(-1);
}

void DamageBreakdown::addRawDamage(int amount, DamageType type) {
    const int damageFlags = static_cast<int>(type);
    if (damageFlags <= 0) {
        throw std::invalid_argument("Damage breakdown type must be positive");
    }
    const auto slot = getDirectDamageSlot(damageFlags);
    if (slot >= 14) {
        throw std::invalid_argument("Damage breakdown type is out of range");
    }
    int &current = rawDamageSlots[slot];
    int result;
    if (current > 0) {
        result = current + amount;
        if (result <= 0) {
            result = 1;
        }
    } else {
        result = std::max(amount, 0);
    }
    current = result;
}

bool isMeleeWieldType(CreatureWieldType type) {
    switch (type) {
    case CreatureWieldType::SingleSword:
    case CreatureWieldType::DoubleBladedSword:
    case CreatureWieldType::DualSwords:
        return true;
    default:
        return false;
    }
}

bool isAttackSuccessful(AttackResultType result) {
    switch (result) {
    case AttackResultType::HitSuccessful:
    case AttackResultType::CriticalHit:
    case AttackResultType::AutomaticHit:
        return true;
    default:
        return false;
    }
}

} // namespace game

} // namespace reone
