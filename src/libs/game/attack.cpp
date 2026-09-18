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
#include "reone/game/action/movetoobject.h"
#include "reone/game/onhit.h"

#include "reone/game/animations.h"
#include "reone/game/d20/feats.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/immunity.h"
#include "reone/game/effect/forcepushed.h"
#include "reone/game/effect/stunned.h"
#include "reone/game/game.h"
#include "reone/game/mitigationfeedback.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/projectiles.h"
#include "reone/scene/collision.h"
#include "reone/scene/graph.h"
#include "reone/system/arrayref.h"
#include "reone/system/randomutil.h"

#include "physicalcombatrules.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <stdexcept>

namespace reone {

namespace game {

static constexpr char kModelEventDetonate[] = "detonate";
static constexpr float kProjectileSpeed = 16.0f;
static constexpr int kUnarmedCriticalThreat = 1;
static constexpr float kSpecialAttackDefensePenaltyDuration = 3.0f;
static constexpr float kCriticalStrikeStunDuration = 6.0f;
static constexpr float kPowerAttackKnockdownDuration = 0.1f;

namespace {

std::string attackAnimation(
    char prefix,
    CreatureWieldType wield,
    int variant) {

    return str(boost::format("%c%da%d") %
               prefix %
               static_cast<int>(wield) %
               variant);
}

std::string formatPhysicalMeleeAttackAnimation(
    CreatureWieldType wield,
    int variant,
    bool creatureModel,
    bool cinematic) {

    if (creatureModel) {
        if (wield == CreatureWieldType::None) {
            throw std::logic_error("Monster attacks are not supported");
        }
        return attackAnimation('g', CreatureWieldType::None, variant);
    }

    switch (wield) {
    case CreatureWieldType::SingleSword:
    case CreatureWieldType::DoubleBladedSword:
    case CreatureWieldType::DualSwords:
        return attackAnimation(cinematic ? 'c' : 'm', wield, variant);
    case CreatureWieldType::StunBaton:
    case CreatureWieldType::HandToHand:
    case CreatureWieldType::HandToHandComplex:
        return attackAnimation('g', wield, variant);
    case CreatureWieldType::None:
        throw std::logic_error("Monster attacks are not supported");
    default:
        throw std::logic_error("Invalid melee wield type");
    }
}

} // namespace

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
        return true;
    default:
        return false;
    }
}

static bool hasActiveItemProperty(
    const Item &item,
    ItemProperty type,
    int subtype = -1) {

    for (const Item::PropertyEntry &property : item.properties()) {
        if (!item.isPropertyActive(property) ||
            property.propertyName != static_cast<uint16_t>(type) ||
            (subtype >= 0 && property.subtype != subtype)) {
            continue;
        }
        return true;
    }
    return false;
}

static int getBaseCriticalThreat(const Item *weapon) {
    return weapon ? weapon->criticalThreat() : kUnarmedCriticalThreat;
}

static int getCriticalThreat(const Item *weapon, int threatBonus) {
    int threat = getBaseCriticalThreat(weapon);
    if (weapon && hasActiveItemProperty(*weapon, ItemProperty::Keen)) {
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
};

static AttackResolution computeAttack(
    const Creature &attacker,
    const Object &target,
    int attackBonus,
    int criticalThreat,
    int damageFlags,
    bool ranged) {

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

    if (!ranged &&
        targetCreature &&
        targetCreature->isDebilitated() &&
        targetCreature->attributes().getAggregateLevel() <= 4 &&
        !targetCreature->isPartyMember()) {
        resolution.result = AttackResultType::AutomaticHit;
        resolution.roll = 20;
        resolution.coupDeGrace = true;
        return resolution;
    }

    if (attacker.hasAssuredHit()) {
        resolution.result = AttackResultType::HitSuccessful;
        debug(str(boost::format("computeAttack: assured hit: roll(%d)") % resolution.roll),
              LogChannel::Combat);
        return resolution;
    }

    if (ranged && targetCreature) {
        resolution.result = targetCreature->resolveRangedDefense(attacker, damageFlags, resolution.roll + attackBonus);
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
    AttackBuffer::Source source, AttackResultType result,
    bool criticalConfirmed, int criticalMultiplier, int damageBonus,
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
    if (!hasActiveItemProperty(weapon, ItemProperty::NoDamage)) {
        for (int multiple = 0; multiple < multiplier; ++multiple) {
            baseDamage += rollDamageDice(weapon.numDice(), weapon.dieToRoll());
        }
        baseDamage *= attacker.getPhysicalDamageAutoBalanceFactor();
    }
    amount += baseDamage;

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
        multiplier * (damageBonus + physicalBonus.furyDamage) + massiveCriticalDamage;
    breakdown.criticalMultiplier = criticalConfirmed
                                       ? criticalMultiplier
                                       : 0;

    DamageType type = getPrimaryDamageType(weapon.damageFlags());
    if (baseDamage > 0) {
        breakdown.addRawDamage(baseDamage, type);
    }
    damage.add(std::max(amount, 1), type);
    damage.setDamageFlags(weapon.damageFlags());
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
    if (amount <= 0) {
        breakdown.addRawDamage(1, type);
    }

    debug(str(boost::format("computeWeaponDamage: %s -> %s (%d)") % attacker.tag() % target.tag() % damage.total()),
          LogChannel::Combat);
}

static void computeUnarmedDamage(
    const Creature &attacker, const Object &target,
    AttackResultType result, bool criticalConfirmed, int criticalMultiplier,
    int damageBonus, DamagePacket &damage, DamageBreakdown &breakdown) {

    int multiplier = result == AttackResultType::CriticalHit
                         ? criticalMultiplier
                         : 1;
    PhysicalDamageBonus physicalBonus = attacker.getPhysicalDamageBonus(
        nullptr,
        false);

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
        multiplier * (damageBonus + physicalBonus.furyDamage) + massiveCriticalDamage;
    breakdown.criticalMultiplier = criticalConfirmed
                                       ? criticalMultiplier
                                       : 0;

    if (baseDamage > 0) {
        breakdown.addRawDamage(baseDamage, DamageType::Bludgeoning);
    }
    damage.add(std::max(amount, 1), DamageType::Bludgeoning);
    damage.setDamageFlags(static_cast<int>(DamageType::Bludgeoning));
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
    if (amount <= 0) {
        breakdown.addRawDamage(1, DamageType::Bludgeoning);
    }

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
        multiplier = 1;
        break;
    case FeatType::ImprovedCriticalStrike:
        multiplier = 2;
        break;
    case FeatType::MasterCriticalStrike:
        multiplier = 3;
        break;
    default:
        break;
    }
    return multiplier * getBaseCriticalThreat(weapon);
}

void AttackBuffer::addPhysicalAttacks(const Creature &attacker, const Object &target,
                                      FeatType feat) {
    _feat = feat;

    bool tsl = attacker.game().isTSL();
    // K2 caps the raw bonus at consumption, without a negative floor. K1's
    // effect callbacks have already saturated its stored value.
    int bonusAttacks = tsl ? std::min(attacker.modifiedAttacks(), 2)
                          : attacker.modifiedAttacks();
    int mainHandAttacks = 1 + bonusAttacks;
    if (grantsExtraMainHandAttack(feat)) {
        ++mainHandAttacks;
    }

    int attackRollBonus = getMeleeSpecialAttackRollBonus(tsl, feat);
    int damageBonus = getMeleeSpecialAttackDamageBonus(tsl, feat);

    auto main = attacker.getEquippedItem(InventorySlots::rightWeapon);
    if (grantsJuyoExtraOnHandAttack(
            attacker.game().isTSL(),
            main && main->isLightsaber(),
            attacker.currentForm())) {
        ++mainHandAttacks;
    }
    if (!main) {
        auto gloves = attacker.getEquippedItem(InventorySlots::hands);
        int attackThreatBonus = getSpecialAttackThreatBonus(feat, gloves.get());
        for (int i = 0; i < mainHandAttacks; ++i) {
            addPhysicalAttack(
                attacker,
                target,
                nullptr,
                Source::Main,
                attackRollBonus,
                attackThreatBonus,
                damageBonus);
        }
        return;
    }

    int mainThreatBonus = getSpecialAttackThreatBonus(feat, main.get());
    for (int i = 0; i < mainHandAttacks; ++i) {
        addPhysicalAttack(
            attacker,
            target,
            main.get(),
            Source::Main,
            attackRollBonus,
            mainThreatBonus,
            damageBonus);
    }

    auto offhand = attacker.getOffhandAttackWeapon();
    if (offhand) {
        int offhandThreatBonus = getSpecialAttackThreatBonus(feat, offhand.get());
        addPhysicalAttack(
            attacker,
            target,
            offhand.get(),
            Source::Offhand,
            attackRollBonus,
            offhandThreatBonus,
            damageBonus);
    }
}

void AttackBuffer::resolveMeleeSpecialAttack(
    FeatType feat,
    Creature &attacker,
    Object &target,
    Game &game) {

    if (_attacks.empty() || _attacks.front().ranged) {
        return;
    }

    bool tsl = attacker.game().isTSL();
    int defensePenalty = getMeleeSpecialAttackDefensePenalty(tsl, feat);
    if (defensePenalty != 0) {
        auto effect = game.newEffect<ACDecreaseEffect>(
            defensePenalty,
            ACBonus::Dodge,
            kPhysicalDamageTypeFlags);
        attacker.applyEffect(
            std::move(effect),
            DurationType::Temporary,
            kSpecialAttackDefensePenaltyDuration);
    }

    auto *targetCreature = dyn_cast<Creature>(&target);
    if (!targetCreature) {
        return;
    }

    // Melee special-attack state belongs to the first physical
    // subattack in the action. Retain the generated effect there so it lands
    // at that subattack's impact rather than when the action is queued.
    Attack &attack = _attacks.front();
    if (attack.ranged) {
        return;
    }

    int attackerLevel = attacker.attributes().getAggregateLevel();
    int strengthModifier =
        attacker.getEffectiveAbilityModifier(Ability::Strength);

    if (shouldAttemptPowerAttackKnockdown(
            tsl,
            feat,
            attack.result)) {
        int difficultyClass = getMeleeSpecialAttackSaveDC(
            tsl,
            feat,
            attackerLevel,
            strengthModifier);
        auto secondary = game.newEffect<ForcePushedEffect>();
        secondary->setSaveFacingCreator(game.getObjectById(attacker.id()));
        if (targetCreature->isEffectLinkImmune(*secondary) ||
            targetCreature->rollSavingThrow(SavingThrow::Fortitude, difficultyClass,
                SavingThrowType::All, &attacker) != SavingThrowResult::Failed) {
            return;
        }

        attack.secondaryEffect = std::move(secondary);
        attack.secondaryEffectDurationType = DurationType::Temporary;
        attack.secondaryEffectDuration = kPowerAttackKnockdownDuration;
        return;
    }

    bool attemptCriticalStrikeStun = shouldAttemptCriticalStrikeStun(feat, attack.result);
    if (!attemptCriticalStrikeStun) {
        return;
    }

    int difficultyClass = getMeleeSpecialAttackSaveDC(
        tsl, feat, attackerLevel, strengthModifier);
    auto secondary = game.newEffect<StunnedEffect>();
    secondary->setSaveFacingCreator(game.getObjectById(attacker.id()));
    if (targetCreature->isEffectLinkImmune(*secondary) ||
        targetCreature->hasEffectiveFeat(FeatType::ForceImmunityStun) ||
        targetCreature->hasEffectiveFeat(
            FeatType::ForceImmunityParalysis) ||
        targetCreature->rollSavingThrow(SavingThrow::Fortitude, difficultyClass,
                SavingThrowType::All, &attacker) != SavingThrowResult::Failed) {
        return;
    }

    attack.secondaryEffect = std::move(secondary);
    attack.secondaryEffectDurationType = DurationType::Temporary;
    attack.secondaryEffectDuration = kCriticalStrikeStunDuration;
}

void AttackBuffer::addPhysicalAttack(
    const Creature &attacker,
    const Object &target,
    const Item *weapon,
    Source source,
    int attackRollBonus,
    int attackThreatBonus,
    int damageBonus) {

    bool offHand = source == Source::Offhand;
    const auto *targetCreature = dyn_cast<Creature>(&target);
    AttackBonusBreakdown attackBonusBreakdown = attacker.getAttackBonusBreakdown(
        targetCreature,
        weapon,
        offHand);
    attackBonusBreakdown.featBonus = attackRollBonus;
    attackRollBonus = attackBonusBreakdown.total();

    auto handItem = weapon
                        ? std::shared_ptr<Item>()
                        : attacker.getEquippedItem(InventorySlots::hands);
    const Item *criticalWeapon = weapon ? weapon : handItem.get();
    int criticalThreat = getCriticalThreat(criticalWeapon, attackThreatBonus);
    int damageFlags = weapon
                          ? weapon->damageFlags()
                          : static_cast<int>(DamageType::Bludgeoning);

    AttackResolution resolution = computeAttack(
        attacker,
        target,
        attackRollBonus,
        criticalThreat,
        damageFlags,
        weapon && weapon->isRanged());

    if (weapon && weapon->isRanged() && targetCreature &&
        (resolution.result == AttackResultType::Miss ||
         resolution.result == AttackResultType::AttackResisted ||
         resolution.result == AttackResultType::AttackFailed)) {
        const auto interception = targetCreature->resolveRangedMiss(attacker, *weapon);
        if (interception != AttackResultType::Invalid) resolution.result = interception;
    }

    Attack &attack = _attacks.emplace_back(
        source,
        weapon && weapon->isRanged(),
        std::move(attackBonusBreakdown));
    attack.sourceActor = attacker.game().getObjectById(attacker.id());
    for (const auto &[slot, item] : attacker.equipment()) {
        if (item.get() == criticalWeapon) {
            attack.sourceItem = item;
            break;
        }
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
    if (resolution.criticalConfirmed) {
        attack.criticalThreat.multiplier = getResolvedCriticalMultiplier(
            attacker,
            weapon,
            _feat);
    }

    if (!isAttackSuccessful(resolution.result) && resolution.result != AttackResultType::Deflected) {
        return;
    }

    if (attacker.game().isConversationActive() &&
        attacker.isPartyMember()) {
        return;
    }

    if (weapon) {
        computeWeaponDamage(
            attacker,
            target,
            *weapon,
            source,
            resolution.result,
            resolution.criticalConfirmed,
            attack.criticalThreat.multiplier,
            damageBonus,
            attack.damage,
            attack.damageBreakdown);
    } else {
        computeUnarmedDamage(
            attacker,
            target,
            resolution.result,
            resolution.criticalConfirmed,
            attack.criticalThreat.multiplier,
            damageBonus,
            attack.damage,
            attack.damageBreakdown);
    }
}

void AttackBuffer::saveContinuation(SavedPhysicalAction &saved, const Game &game) const {
    using G = resource::Gff; using F = G::Field;
    std::vector<std::shared_ptr<G>> attacks;
    saved.sources.clear(); saved.secondaryEffects.clear(); saved.histories.clear();
    for (const auto &a : _attacks) {
        std::vector<std::shared_ptr<G>> values, slots;
        const int fields[] = {static_cast<int>(a.source), static_cast<int>(a.ranged), static_cast<int>(a.result), static_cast<int>(a.roll), static_cast<int>(a.naturalTwenty), static_cast<int>(a.naturalOne), static_cast<int>(a.coupDeGrace), static_cast<int>(a.criticalThreat.threshold), static_cast<int>(a.criticalThreat.threatened), static_cast<int>(a.criticalThreat.confirmationRoll), static_cast<int>(a.criticalThreat.confirmationBonus), static_cast<int>(a.criticalThreat.confirmed), static_cast<int>(a.criticalThreat.multiplier), static_cast<int>(a.impactTimeMilliseconds), static_cast<int>(a.meleeSignaled), static_cast<int>(a.onHitResolved), static_cast<int>(a.secondaryEffectDurationType), static_cast<int>(a.attackBonusBreakdown.baseAttackBonus), static_cast<int>(a.attackBonusBreakdown.strengthModifier), static_cast<int>(a.attackBonusBreakdown.dexterityModifier), static_cast<int>(a.attackBonusBreakdown.dualWieldPenalty), static_cast<int>(a.attackBonusBreakdown.smallOffhandBonus), static_cast<int>(a.attackBonusBreakdown.featBonus), static_cast<int>(a.attackBonusBreakdown.duelingFeat), static_cast<int>(a.attackBonusBreakdown.duelingBonus), static_cast<int>(a.attackBonusBreakdown.closeProximityRangedBonus), static_cast<int>(a.attackBonusBreakdown.meleeOnRangedBonus), static_cast<int>(a.attackBonusBreakdown.weaponFocusBonus), static_cast<int>(a.attackBonusBreakdown.targetingBonus), static_cast<int>(a.attackBonusBreakdown.superiorWeaponFocusBonus), static_cast<int>(a.attackBonusBreakdown.formBonus), static_cast<int>(a.attackBonusBreakdown.dualStrikeBonus), static_cast<int>(a.attackBonusBreakdown.effectBonus), static_cast<int>(a.defenseBreakdown.total), static_cast<int>(a.defenseBreakdown.armor), static_cast<int>(a.defenseBreakdown.dexterity), static_cast<int>(a.defenseBreakdown.classDefense), static_cast<int>(a.defenseBreakdown.natural), static_cast<int>(a.defenseBreakdown.dodgeAndDeflection), static_cast<int>(a.defenseBreakdown.feat), static_cast<int>(a.defenseBreakdown.stance), static_cast<int>(a.defenseBreakdown.form), static_cast<int>(a.defenseBreakdown.debilitationPenalty), static_cast<int>(a.damageBreakdown.strengthModifier), static_cast<int>(a.damageBreakdown.otherSpecialBonus), static_cast<int>(a.damageBreakdown.sneakAttack), static_cast<int>(a.damageBreakdown.weaponSpecialization), static_cast<int>(a.damageBreakdown.combatFeatDamage), static_cast<int>(a.damageBreakdown.preciseShotDamage), static_cast<int>(a.damageBreakdown.formDamage), static_cast<int>(a.damageBreakdown.unarmedFeatDamage209), static_cast<int>(a.damageBreakdown.unarmedFeatDamage212), static_cast<int>(a.damageBreakdown.criticalMultiplier)};
        for (int value : fields) values.push_back(G::Builder().type(0).field(F::newInt("Value", value)).build());
        for (int value : a.damageBreakdown.rawDamageSlots) slots.push_back(G::Builder().type(0).field(F::newInt("Value", value)).build());
        SavedWeaponImpact onHit; onHit.applications = a.onHitApplications; onHit.feedback = a.deferredFeedback;
        auto g = G::Builder().type(0).field(F::newList("Values", std::move(values)))
            .field(F::newList("DamageSlots", std::move(slots))).field(F::newStruct("Packet", a.damage.saveContinuation()))
            .field(F::newFloat("EffectDuration", a.secondaryEffectDuration)).field(F::newStruct("OnHit", onHit.toGff())).build();
        attacks.push_back(std::move(g));
        for (const auto &object : {std::static_pointer_cast<Object>(a.sourceItem.resolve()), a.sourceActor.resolve()}) {
            auto ref = SavedObjectReference::fromRuntimeId(object ? object->id() : script::kObjectInvalid);
            game.bindSavedObjectReference(ref); saved.sources.push_back(std::move(ref));
        }
        saved.secondaryEffects.push_back(a.secondaryEffect ? a.secondaryEffect->saveFacingInstance() : EffectInstance {});
        SavedCombatAttack history; history.history = std::make_shared<AttackHistory>(*a.history);
        history.fields = std::make_shared<AttackEventFields>(*a.eventFields); saved.histories.push_back(std::move(history));
    }
    saved.state = G::Builder().type(0).field(F::newInt("Feat", static_cast<int>(_feat)))
        .field(F::newInt("Pending", static_cast<int>(_pendingMeleeAttacks)))
        .field(F::newByte("Prepared", _meleeSequencePrepared)).field(F::newList("Attacks", std::move(attacks))).build();
}

void AttackBuffer::restoreContinuation(const SavedPhysicalAction &saved) {
    _attacks.clear();
    _feat = static_cast<FeatType>(saved.state->getInt("Feat", -1));
    _pendingMeleeAttacks = saved.state->getInt("Pending"); _meleeSequencePrepared = saved.state->getBool("Prepared");
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
        a.meleeSignaled = static_cast<decltype(a.meleeSignaled)>(value(14));
        a.onHitResolved = static_cast<decltype(a.onHitResolved)>(value(15));
        a.secondaryEffectDurationType = static_cast<decltype(a.secondaryEffectDurationType)>(value(16));
        a.attackBonusBreakdown.baseAttackBonus = static_cast<decltype(a.attackBonusBreakdown.baseAttackBonus)>(value(17));
        a.attackBonusBreakdown.strengthModifier = static_cast<decltype(a.attackBonusBreakdown.strengthModifier)>(value(18));
        a.attackBonusBreakdown.dexterityModifier = static_cast<decltype(a.attackBonusBreakdown.dexterityModifier)>(value(19));
        a.attackBonusBreakdown.dualWieldPenalty = static_cast<decltype(a.attackBonusBreakdown.dualWieldPenalty)>(value(20));
        a.attackBonusBreakdown.smallOffhandBonus = static_cast<decltype(a.attackBonusBreakdown.smallOffhandBonus)>(value(21));
        a.attackBonusBreakdown.featBonus = static_cast<decltype(a.attackBonusBreakdown.featBonus)>(value(22));
        a.attackBonusBreakdown.duelingFeat = static_cast<decltype(a.attackBonusBreakdown.duelingFeat)>(value(23));
        a.attackBonusBreakdown.duelingBonus = static_cast<decltype(a.attackBonusBreakdown.duelingBonus)>(value(24));
        a.attackBonusBreakdown.closeProximityRangedBonus = static_cast<decltype(a.attackBonusBreakdown.closeProximityRangedBonus)>(value(25));
        a.attackBonusBreakdown.meleeOnRangedBonus = static_cast<decltype(a.attackBonusBreakdown.meleeOnRangedBonus)>(value(26));
        a.attackBonusBreakdown.weaponFocusBonus = static_cast<decltype(a.attackBonusBreakdown.weaponFocusBonus)>(value(27));
        a.attackBonusBreakdown.targetingBonus = static_cast<decltype(a.attackBonusBreakdown.targetingBonus)>(value(28));
        a.attackBonusBreakdown.superiorWeaponFocusBonus = static_cast<decltype(a.attackBonusBreakdown.superiorWeaponFocusBonus)>(value(29));
        a.attackBonusBreakdown.formBonus = static_cast<decltype(a.attackBonusBreakdown.formBonus)>(value(30));
        a.attackBonusBreakdown.dualStrikeBonus = static_cast<decltype(a.attackBonusBreakdown.dualStrikeBonus)>(value(31));
        a.attackBonusBreakdown.effectBonus = static_cast<decltype(a.attackBonusBreakdown.effectBonus)>(value(32));
        a.defenseBreakdown.total = static_cast<decltype(a.defenseBreakdown.total)>(value(33));
        a.defenseBreakdown.armor = static_cast<decltype(a.defenseBreakdown.armor)>(value(34));
        a.defenseBreakdown.dexterity = static_cast<decltype(a.defenseBreakdown.dexterity)>(value(35));
        a.defenseBreakdown.classDefense = static_cast<decltype(a.defenseBreakdown.classDefense)>(value(36));
        a.defenseBreakdown.natural = static_cast<decltype(a.defenseBreakdown.natural)>(value(37));
        a.defenseBreakdown.dodgeAndDeflection = static_cast<decltype(a.defenseBreakdown.dodgeAndDeflection)>(value(38));
        a.defenseBreakdown.feat = static_cast<decltype(a.defenseBreakdown.feat)>(value(39));
        a.defenseBreakdown.stance = static_cast<decltype(a.defenseBreakdown.stance)>(value(40));
        a.defenseBreakdown.form = static_cast<decltype(a.defenseBreakdown.form)>(value(41));
        a.defenseBreakdown.debilitationPenalty = static_cast<decltype(a.defenseBreakdown.debilitationPenalty)>(value(42));
        a.damageBreakdown.strengthModifier = static_cast<decltype(a.damageBreakdown.strengthModifier)>(value(43));
        a.damageBreakdown.otherSpecialBonus = static_cast<decltype(a.damageBreakdown.otherSpecialBonus)>(value(44));
        a.damageBreakdown.sneakAttack = static_cast<decltype(a.damageBreakdown.sneakAttack)>(value(45));
        a.damageBreakdown.weaponSpecialization = static_cast<decltype(a.damageBreakdown.weaponSpecialization)>(value(46));
        a.damageBreakdown.combatFeatDamage = static_cast<decltype(a.damageBreakdown.combatFeatDamage)>(value(47));
        a.damageBreakdown.preciseShotDamage = static_cast<decltype(a.damageBreakdown.preciseShotDamage)>(value(48));
        a.damageBreakdown.formDamage = static_cast<decltype(a.damageBreakdown.formDamage)>(value(49));
        a.damageBreakdown.unarmedFeatDamage209 = static_cast<decltype(a.damageBreakdown.unarmedFeatDamage209)>(value(50));
        a.damageBreakdown.unarmedFeatDamage212 = static_cast<decltype(a.damageBreakdown.unarmedFeatDamage212)>(value(51));
        a.damageBreakdown.criticalMultiplier = static_cast<decltype(a.damageBreakdown.criticalMultiplier)>(value(52));
        const auto &slots = g->getList("DamageSlots");
        for (size_t n = 0; n < std::min(slots.size(), a.damageBreakdown.rawDamageSlots.size()); ++n)
            a.damageBreakdown.rawDamageSlots[n] = slots[n]->getInt("Value");
        a.sourceItem = std::dynamic_pointer_cast<Item>(saved.sources[index * 2].boundObject());
        a.sourceActor = saved.sources[index * 2 + 1].boundObject();
        auto secondary = saved.secondaryEffects[index];
        if (secondary.serializedType) { secondary.restoring = false; secondary.materialize(); a.secondaryEffect = secondary.effect; }
        a.secondaryEffectDuration = g->getFloat("EffectDuration");
        a.history = std::make_shared<AttackHistory>(*saved.histories[index].history);
        a.eventFields = std::make_shared<AttackEventFields>(*saved.histories[index].fields);
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
    g.fields().push_back(F::newByte("Melee", _melee)); g.fields().push_back(F::newInt("MeleeElapsed", _meleeElapsedMilliseconds));
    g.fields().push_back(F::newInt("MeleeEnd", _meleeCompletionMilliseconds));
    g.fields().push_back(F::newFloat("MeleeRemainder", _meleeElapsedRemainderMilliseconds));
}
void AttackSchedule::restoreContinuation(const resource::Gff &g) {
    _state = static_cast<State>(g.getInt("Phase")); _time = g.getFloat("Time"); _melee = g.getBool("Melee");
    _meleeElapsedMilliseconds = g.getInt("MeleeElapsed"); _meleeCompletionMilliseconds = g.getInt("MeleeEnd");
    _meleeElapsedRemainderMilliseconds = g.getFloat("MeleeRemainder");
    // Entry states have already executed before the snapshot was taken.
    if (_state == Attack) _state = WaitDamage;
    else if (_state == Damage) _state = _melee && _meleeElapsedMilliseconds < _meleeCompletionMilliseconds ? WaitDamage : WaitFinish;
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

void AttackBuffer::resolveDamage(
    const Creature &attacker,
    Object &target) {

    for (Attack &attack : _attacks) {
        if (attack.result == AttackResultType::Deflected) {
            // A returned shot must not consume the original defender's finite pools.
            continue;
        }
        if (!attack.damage.empty() && !attack.damage.isResolved()) attack.damage.resolve(target);
        resolveItemOnHitProperties(attacker, target, attack);
    }
}

void AttackBuffer::resolve(Creature &attacker, Object &target) {
    assert(!_attacks.empty() && "Physical attack buffer is empty");

    attacker.beginCombatAttack(target, _feat);
    auto *targetCreature = dyn_cast<Creature>(&target);
    const int targetHpBefore = targetCreature ? targetCreature->currentHitPoints() : 0;
    resolveDamage(attacker, target);
    if (targetCreature && targetCreature->currentHitPoints() <= targetHpBefore) {
        attacker.incrementFuryDamageBonus();
    }
    _attacks.front().history->type = static_cast<uint16_t>(_feat);
    for (auto &attack : _attacks) {
        // Populate only fields with an existing represented producer. The
        // remaining reaction/animation/debug fields retain clear state.
        auto &fields = *attack.eventFields;
        fields.result = static_cast<uint8_t>(attack.result);
        fields.deflected = attack.result == AttackResultType::Deflected;
        fields.ranged = attack.ranged ? 1 : 0;
        if (attack.ranged && (isAttackSuccessful(attack.result) ||
            attack.result == AttackResultType::Parried || attack.result == AttackResultType::Deflected ||
            attack.result == AttackResultType::ShieldHit)) {
            const auto position = target.position();
            fields.rangedTarget = {position.x, position.y, position.z};
            fields.reactionAnimation = isAttackSuccessful(attack.result) ? 10014 : 10012;
            fields.reactionDelay = static_cast<uint16_t>(glm::distance(attacker.position(), position) * 1000.0f / 42.0f);
        }
        // The serialized weapon-attack byte is 1 for the main hand and 2 for the off hand.
        fields.weaponAttackType = attack.source == Source::Offhand ? 2 : 1;
        fields.coupDeGrace = attack.coupDeGrace ? 1 : 0;
        std::copy(attack.damageBreakdown.rawDamageSlots.begin(),
                  attack.damageBreakdown.rawDamageSlots.end(), fields.damage.begin());
        fields.damage.back() = attack.damage.isResolved() ? attack.damage.resolvedDamage() : -1;
    }
}

static void addMitigationFeedback(
    Game &game,
    ServicesView &services,
    const Creature &attacker,
    const Object &target,
    const DamagePacket &damage);

void AttackBuffer::applyEffects(
    Attack &attack,
    Creature &attacker,
    Object &target,
    Game &game) {

    if (!attack.ranged && !isAttackSuccessful(attack.result)) {
        game.floatingText().addMiss(attacker, target);
    }
    if (!attack.damage.empty()) {
        DamageEffect::ApplicationContext context;
        std::copy(
            attack.damageBreakdown.rawDamageSlots.begin(),
            attack.damageBreakdown.rawDamageSlots.end(),
            context.damageAmounts.begin());
        context.damageAmounts.back() = attack.damage.resolvedDamage();
        context.suppressDamageShields = attack.ranged;

        auto effect = game.newEffect<DamageEffect>(
            std::move(attack.damage),
            std::move(context));
        effect->setSaveFacingCreator(attack.sourceActor.resolve());
        target.applyEffect(std::move(effect), DurationType::Instant);
    }
    if (attack.secondaryEffect) {
        target.applyEffect(
            attack.secondaryEffect,
            attack.secondaryEffectDurationType,
            attack.secondaryEffectDuration);
        attack.secondaryEffect.reset();
    }
}

void AttackBuffer::signalAttack(
    Attack &attack,
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    if (attack.meleeSignaled) return;
    attack.meleeSignaled = true;
    // Emit event 15 for logical subattacks, including misses. Cosmetic projectile
    // discharges do not emit attack-history events.
    if (isa<Creature>(target)) {
        auto module = game.module();
        if (module) {
            SavedCombatAttack payload;
            payload.data.type = 0x2222;
            payload.history = attack.history;
            payload.fields = attack.eventFields;
            payload.reactionObject = SavedObjectReference::fromRuntimeId(target.id());
            const auto weapon = attack.sourceItem.resolve();
            payload.ammoItem = SavedObjectReference::fromRuntimeId(weapon ? weapon->id() : script::kObjectInvalid);
            SavedEventRecord event;
            const uint64_t when = game.worldTimeMilliseconds() +
                static_cast<uint64_t>(&attack - &_attacks.front());
            event.day = static_cast<uint32_t>(when / game.millisecondsPerWorldDay());
            event.time = static_cast<uint32_t>(when % game.millisecondsPerWorldDay());
            event.object = SavedObjectReference::fromRuntimeId(target.id());
            event.caller = SavedObjectReference::fromRuntimeId(attacker.id());
            event.eventId = static_cast<uint32_t>(SavedEventType::OnMeleeAttacked);
            event.payload = std::move(payload);
            module->enqueueSaveEvent(std::move(event));
        }
    }
    addCombatFeedback(game, services, attacker, target, attack);
    if (attack.result == AttackResultType::Deflected && !attack.damage.empty()) {
        auto *deflector = dyn_cast<Creature>(&target);
        if (!deflector) return;
        attack.sourceActor = game.getObjectById(deflector->id());
        resolveItemOnHitProperties(*deflector, attacker, attack);
        SavedWeaponImpact impact;
        impact.source = SavedObjectReference::fromRuntimeId(deflector->id());
        game.bindSavedObjectReference(impact.source);
        impact.damage.serializedType = 38;
        impact.damage.setDuration(DurationType::Instant, 0.0f);
        impact.damage.exposed = 1;
        impact.damage.creator = game.getObjectById(deflector->id());
        impact.damage.creatorId = deflector->id();
        impact.damage.integerParameters.assign(21, -1);
        std::copy(attack.damageBreakdown.rawDamageSlots.begin(), attack.damageBreakdown.rawDamageSlots.end(),
            impact.damage.integerParameters.begin());
        impact.damage.integerParameters[14] = attack.damage.total();
        impact.damage.integerParameters[15] = 0;
        impact.damage.integerParameters[16] = 0;
        impact.damage.integerParameters[17] = attack.damage.damageFlags();
        impact.damage.integerParameters[18] = static_cast<int>(attack.damage.power());
        impact.damage.integerParameters[19] = 0;
        impact.damage.integerParameters[20] = 1;
        impact.applications = std::move(attack.onHitApplications);
        impact.feedback = std::move(attack.deferredFeedback);
        const uint32_t delay = static_cast<uint32_t>(glm::length(attacker.position() - target.position()) * 1000.0f / 42.0f);
        const uint64_t when = game.worldTimeMilliseconds() + delay;
        SavedEventRecord event;
        event.day = static_cast<uint32_t>(when / game.millisecondsPerWorldDay());
        event.time = static_cast<uint32_t>(when % game.millisecondsPerWorldDay());
        event.eventId = static_cast<uint32_t>(SavedEventType::ItemOnHitSpellImpact);
        event.object = SavedObjectReference::fromRuntimeId(attacker.id());
        event.caller = impact.source;
        event.payload = std::move(impact);
        if (auto module = game.module()) module->enqueueSaveEvent(std::move(event));
        if (auto weapon = attack.sourceItem.resolve()) {
            if (auto module = game.module()) {
                SavedCombatAttack presentation;
                presentation.data.type = 0x2222;
                presentation.history = std::make_shared<AttackHistory>(*attack.history);
                presentation.fields = std::make_shared<AttackEventFields>(*attack.eventFields);
                presentation.fields->result = static_cast<uint8_t>(AttackResultType::HitSuccessful);
                presentation.fields->reactionDelay = static_cast<uint16_t>(delay);
                presentation.reactionObject = SavedObjectReference::fromRuntimeId(attacker.id());
                presentation.ammoItem = SavedObjectReference::fromRuntimeId(weapon->id());
                SavedEventRecord visual;
                const uint64_t now = game.worldTimeMilliseconds();
                visual.day = static_cast<uint32_t>(now / game.millisecondsPerWorldDay());
                visual.time = static_cast<uint32_t>(now % game.millisecondsPerWorldDay());
                visual.object = SavedObjectReference::fromRuntimeId(deflector->id());
                visual.caller = visual.object;
                visual.eventId = static_cast<uint32_t>(SavedEventType::BroadcastSafeProjectile);
                visual.payload = std::move(presentation);
                module->enqueueSaveEvent(std::move(visual));
            }
        }
        return;
    }
    addDeferredCombatFeedback(game, services, attack.sourceActor.resolve(), target, attack.deferredFeedback);
    attack.deferredFeedback.clear();
    addMitigationFeedback(
        game, services, attacker, target, attack.damage);
    applyEffects(attack, attacker, target, game);
    applyItemOnHitApplications(std::move(attack.onHitApplications), target, game, services);
    attack.onHitApplications.clear();
}

void AttackBuffer::signal(
    Game &game,
    ServicesView &services,
    Creature &attacker,
    Object &target) {

    for (Attack &attack : _attacks) {
        signalAttack(attack, game, services, attacker, target);
    }
}

void AttackBuffer::prepareMeleeSequence(
    const IAnimations &animations,
    const std::vector<std::string> &attackAnimations) {

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
        attack.impactTimeMilliseconds =
            animations.getMeleeImpactTime(attackAnimations[index], index);
        attack.meleeSignaled = false;
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
    if (!hasPendingMelee()) {
        return 0;
    }

    std::vector<size_t> ready;
    ready.reserve(_pendingMeleeAttacks);
    for (size_t index = 0; index < _attacks.size(); ++index) {
        const Attack &attack = _attacks[index];
        if (attack.meleeSignaled ||
            elapsedMilliseconds < attack.impactTimeMilliseconds) {
            continue;
        }
        ready.push_back(index);
    }
    std::stable_sort(
        ready.begin(),
        ready.end(),
        [this](size_t left, size_t right) {
            return _attacks[left].impactTimeMilliseconds <
                   _attacks[right].impactTimeMilliseconds;
        });

    for (size_t index : ready) {
        Attack &attack = _attacks[index];
        signalAttack(attack, game, services, attacker, target);
        attack.meleeSignaled = true;
        --_pendingMeleeAttacks;
    }
    return ready.size();
}

int AttackBuffer::latestMeleeImpactMilliseconds() const {
    assert(_meleeSequencePrepared &&
           "melee attack sequence is not prepared");

    int latest = 0;
    for (const Attack &attack : _attacks) {
        latest = std::max(latest, attack.impactTimeMilliseconds);
    }
    return latest;
}

void AttackBuffer::clearHistory() {
    for (auto &attack : _attacks) {
        *attack.history = {};
        *attack.eventFields = AttackEventFields {};
    }
}

void AttackBuffer::discardPendingMelee() {
    if (_meleeSequencePrepared) {
        for (Attack &attack : _attacks) {
            attack.meleeSignaled = true;
        }
        _pendingMeleeAttacks = 0;
    }
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
static constexpr int kStrRefAutomaticHit = 42390;
static constexpr int kStrRefAutomaticMiss = 42391;
static constexpr int kStrRefBaseAttackBonus = 42392;
static constexpr int kStrRefPoisonDamage = 41902;
static constexpr int kStrRefDefenseDebilitated = 42427;
static constexpr int kStrRefImprovedToughnessDamage = 42433;
static constexpr int kStrRefWookieeEnduranceDamage = 42434;

static std::string getFeedbackString(
    Game &game,
    ServicesView &services,
    int strRef,
    std::initializer_list<std::pair<int, std::string>> tokens) {

    std::string text = services.resource.strings.getText(strRef);
    for (const auto &[token, value] : tokens) {
        text = game.substituteCustomToken(
            std::move(text),
            token,
            value);
    }
    return text;
}

static void appendDefenseComponent(
    Game &game,
    ServicesView &services,
    std::string &breakdown,
    int strRef,
    int value) {

    if (value == 0) {
        return;
    }
    breakdown += getFeedbackString(
        game,
        services,
        strRef,
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
    breakdown += getFeedbackString(
        game,
        services,
        strRef,
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
    breakdown += getFeedbackString(
        game,
        services,
        strRef,
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
        finalText = getFeedbackString(
            game,
            services,
            kStrRefCriticalDamageMultiplier,
            {{0, std::to_string(values.criticalMultiplier)}});
    }
    finalText += std::to_string(finalDamage);

    return getFeedbackString(
        game,
        services,
        kStrRefDamageBreakdown,
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
            breakdown);
    }
}

static std::string getMitigationFeedback(
    Game &game,
    ServicesView &services,
    const Object &target,
    const MitigationFeedback &feedback) {

    MitigationFeedbackMessage message = buildMitigationFeedbackMessage(
        services.resource.strings,
        target.name(),
        feedback);
    std::string text = services.resource.strings.getText(message.strRef);
    for (std::size_t token = 0; token < message.customTokenCount; ++token) {
        text = game.substituteCustomToken(
            std::move(text),
            static_cast<int>(token),
            message.customTokens[token]);
    }
    return text;
}

static void addMitigationFeedback(
    Game &game,
    ServicesView &services,
    const Creature &attacker,
    const Object &target,
    const DamagePacket &damage) {

    if (damage.empty()) {
        return;
    }

    auto leader = game.party().getLeader();
    if (!leader) {
        return;
    }

    const auto *targetCreature = dyn_cast<Creature>(&target);
    if (!targetCreature) {
        return;
    }

    bool targetHasRecipient = targetCreature->id() == leader->id();
    bool attackerHasRecipient = attacker.id() == leader->id();
    if (!targetHasRecipient && !attackerHasRecipient) {
        return;
    }

    for (const MitigationFeedback &feedback :
         damage.resolution().mitigationFeedback) {
        std::string text = getMitigationFeedback(
            game,
            services,
            target,
            feedback);
        if (targetHasRecipient) {
            game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                text);
        }
        if (attackerHasRecipient) {
            game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                text);
        }
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

    bool successful = isAttackSuccessful(attack.result);
    std::string feedback = getFeedbackString(
        game,
        services,
        kStrRefAttackSummary,
        {
            {0, attackerName},
            {1, services.resource.strings.getText(
                    successful ? kStrRefAttackSuccessVerb : kStrRefAttackFailureVerb)},
            {2, targetName},
        });
    feedback += ". ";

    if (isPhysicalAttackFeat(_feat)) {
        auto feat = services.game.feats.get(_feat);
        feedback += getFeedbackString(
            game,
            services,
            kStrRefAttackFeat,
            {{0, feat->name}});
        feedback += ". ";
    }

    feedback += getFeedbackString(
        game,
        services,
        kStrRefAttackRoll,
        {
            {0, services.resource.strings.getText(
                    successful ? kStrRefAttackRollSuccess : kStrRefAttackRollFailure)},
            {1, std::to_string(
                    attack.roll + attack.attackBonusBreakdown.total())},
            {2, std::to_string(defense)},
            {3, std::to_string(
                    (attack.damage.empty() || !attack.damage.isResolved())
                        ? 0
                        : game.scaleDamageForDifficulty(
                              attack.damage.resolvedDamage(),
                              target))},
        });

    if (attack.coupDeGrace) {
        feedback += services.resource.strings.getText(kStrRefCoupDeGrace);
        feedback += " ";
    }
    if (attack.criticalThreat.confirmed) {
        feedback += services.resource.strings.getText(kStrRefConfirmedCritical);
        feedback += " ";
    }
    if (attack.naturalTwenty) {
        feedback += services.resource.strings.getText(kStrRefAutomaticHit);
        feedback += " ";
    }
    if (attack.naturalOne) {
        feedback += services.resource.strings.getText(kStrRefAutomaticMiss);
        feedback += " ";
    }

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Combat,
            feedback);
    }

    const AttackBonusBreakdown &bonus = attack.attackBonusBreakdown;
    std::string breakdown = getFeedbackString(
        game,
        services,
        kStrRefAttackBreakdown,
        {
            {0, services.resource.strings.getText(
                    attack.source == Source::Main
                        ? kStrRefMainhand
                        : kStrRefOffhand)},
            {1, std::to_string(
                    attack.roll + attack.attackBonusBreakdown.total())},
        });
    breakdown += getFeedbackString(
        game,
        services,
        kStrRefAttackRollComponent,
        {{0, std::to_string(attack.roll)}});

    if (attack.naturalTwenty) {
        breakdown += " ";
        breakdown += services.resource.strings.getText(kStrRefAutomaticHit);
    } else if (attack.naturalOne) {
        breakdown += " ";
        breakdown += services.resource.strings.getText(kStrRefAutomaticMiss);
    } else {
        breakdown += getFeedbackString(
            game,
            services,
            kStrRefBaseAttackBonus,
            {{0, std::to_string(bonus.baseAttackBonus)}});

        if (bonus.dualWieldPenalty != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefDualWieldPenalty,
                {{0, std::to_string(bonus.dualWieldPenalty)}});
        }
        if (bonus.smallOffhandBonus != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefSmallOffhandBonus,
                {{0, std::to_string(bonus.smallOffhandBonus)}});
        }
        if (_feat != FeatType::Invalid && bonus.featBonus != 0) {
            auto feat = services.game.feats.get(_feat);
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefFeatAttackBonus,
                {
                    {0, feat->name},
                    {1, std::to_string(bonus.featBonus)},
                });
        }
        if (bonus.duelingBonus != 0) {
            auto feat = services.game.feats.get(bonus.duelingFeat);
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefFeatAttackBonus,
                {
                    {0, feat->name},
                    {1, std::to_string(bonus.duelingBonus)},
                });
        }
        if (bonus.closeProximityRangedBonus != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefCloseProximityRangedBonus,
                {{0, std::to_string(bonus.closeProximityRangedBonus)}});
        }
        if (bonus.meleeOnRangedBonus != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefMeleeOnRangedBonus,
                {{0, std::to_string(bonus.meleeOnRangedBonus)}});
        }
        if (bonus.dexterityModifier != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefDexterityModifier,
                {{0, std::to_string(bonus.dexterityModifier)}});
        } else if (bonus.strengthModifier != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefStrengthModifier,
                {{0, std::to_string(bonus.strengthModifier)}});
        }
        if (bonus.weaponFocusBonus != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefWeaponFocusBonus,
                {{0, std::to_string(bonus.weaponFocusBonus)}});
        }
        if (bonus.effectBonus != 0) {
            breakdown += getFeedbackString(
                game,
                services,
                kStrRefEffectBonus,
                {{0, std::to_string(bonus.effectBonus)}});
        }
    }

    for (int broadcast = 0; broadcast < broadcasts; ++broadcast) {
        game.messageLog().add(
            MessageLog::kFeedbackMessageType,
            MessageLog::Style::Normal,
            breakdown);
    }

    if (attack.criticalThreat.threatened) {
        std::string criticalThreatBreakdown = getFeedbackString(
            game,
            services,
            kStrRefCriticalThreatBreakdown,
            {
                {0, std::to_string(attack.roll)},
                {1, std::to_string(attack.criticalThreat.threshold)},
                {2, services.resource.strings.getText(
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
                criticalThreatBreakdown);
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
    std::string defenseBreakdown = getFeedbackString(
        game,
        services,
        kStrRefDefenseBreakdown,
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
    appendDefenseComponent(
        game,
        services,
        defenseBreakdown,
        kStrRefDefenseEffects,
        defenseValues.dodgeAndDeflection);
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
            defenseBreakdown);
    }

    addDamageBreakdownFeedback(
        game,
        services,
        attack.damageBreakdown,
        attack.damage,
        broadcasts);
}

AttackResultType AttackBuffer::rangedResult(bool offHand, size_t index) const {
    AttackResultType result = AttackResultType::Invalid;
    for (const auto &attack : _attacks) {
        if (!attack.ranged || (attack.source == Source::Offhand) != offHand) continue;
        result = attack.result;
        if (index == 0) break;
        --index;
    }
    return result;
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

std::shared_ptr<Item> determineProjectileWeapon(Creature &attacker, Projectile::Source source) {
    int slot = (source == Projectile::Main)
                   ? InventorySlots::rightWeapon
                   : InventorySlots::leftWeapon;

    std::shared_ptr<Item> weapon(attacker.getEquippedItem(slot));
    if (!weapon) {
        slot = (source == Projectile::Main)
                   ? InventorySlots::leftWeapon
                   : InventorySlots::rightWeapon;

        weapon = attacker.getEquippedItem(slot);
    }

    return weapon;
}

static glm::vec3 determineProjectileOrigin(scene::ModelSceneNode &model, Projectile::Source source) {
    std::string attachment = (source == Projectile::Main) ? "rhand" : "lhand";
    auto weaponModel = static_cast<scene::ModelSceneNode *>(model.getAttachment(attachment));
    if (weaponModel) {
        auto bulletHook = weaponModel->getNodeByName("bullethook");
        if (bulletHook) {
            return bulletHook->origin();
        }
        return weaponModel->origin();
    }

    // Droids do not have weapon model, but they have hooks in the main (body) model.
    std::string directAttachment = (source == Projectile::Main) ? "rbullet" : "lbullet";
    if (scene::SceneNode *direct = model.getNodeByName(directAttachment)) {
        return direct->origin();
    }

    return model.origin();
}

static std::optional<glm::vec3> determineMuzzleFlashOrigin(scene::ModelSceneNode &model, Projectile::Source source) {
    std::string attachment = (source == Projectile::Main) ? "rhand" : "lhand";
    auto weaponModel = static_cast<scene::ModelSceneNode *>(model.getAttachment(attachment));
    if (weaponModel) {
        if (auto muzzleHook = weaponModel->getNodeByName("muzzlehook")) {
            return muzzleHook->origin();
        }
    }
    return std::nullopt;
}

void Projectile::fire(Creature &attacker, Object &target, scene::ISceneGraph &sceneGraph) {
    auto attackerModel = std::static_pointer_cast<scene::ModelSceneNode>(attacker.sceneNode());
    auto targetModel = std::static_pointer_cast<scene::ModelSceneNode>(target.sceneNode());
    if (!attackerModel || !targetModel)
        return;

    std::shared_ptr<Item> weapon = determineProjectileWeapon(attacker, _source);
    if (!weapon)
        return;

    std::shared_ptr<Item::AmmunitionType> ammunitionType(weapon->ammunitionType());
    if (!ammunitionType)
        return;

    glm::vec3 projectilePos = determineProjectileOrigin(*attackerModel, _source);

    // Determine projectile direction
    auto impact = targetModel->getNodeByName("impact");
    if (_result == AttackResultType::ShieldHit) {
        _target = target.position() + glm::vec3(0.0f, 0.0f, 1.25f);
    } else if (impact) {
        _target = impact->origin();
    } else {
        _target = targetModel->origin();
    }

    if (_miss) {
        float offsetRadius = 1.5f * glm::length(targetModel->origin() - _target);
        glm::vec3 offsetDir = glm::normalize(
            glm::vec3(randomFloat(0.0f, 1.0f),
                      randomFloat(0.0f, 1.0f),
                      randomFloat(0.0f, 1.0f)));
        glm::vec3 offsetTarget = _target + offsetDir * offsetRadius;
        glm::vec3 dir = _target - projectilePos;
        _target = projectilePos + dir * 1000.0f;

        scene::Collision collision;
        if (sceneGraph.testLineOfSight(projectilePos, _target, collision)) {
            _target = collision.intersection;
        }
    }

    // Create and add a projectile to the scene graph
    _model = sceneGraph.newModel(*ammunitionType->model, scene::ModelUsage::Projectile);
    _model->signalEvent(kModelEventDetonate);
    _model->setLocalTransform(glm::translate(projectilePos));
    sceneGraph.addRoot(_model);

    if (ammunitionType->muzzleFlash) {
        glm::vec3 origin = determineMuzzleFlashOrigin(*attackerModel, _source)
                               .value_or(projectilePos);

        _flash = sceneGraph.newModel(*ammunitionType->muzzleFlash, scene::ModelUsage::Projectile);
        _flash->setLocalTransform(glm::translate(projectilePos));
        _flash->signalEvent(kModelEventDetonate);
        sceneGraph.addRoot(_flash);
    }

    // Play shot sound, if any
    weapon->playShotSound(0, projectilePos);
}

bool Projectile::update(float dt) {
    if (!_model) {
        return false;
    }

    glm::vec3 position = _model->origin();
    glm::vec3 vec = _target - position;
    float length = glm::length(vec);

    float dist = dt * kProjectileSpeed;
    if (dist >= length) {
        _model->setLocalTransform(glm::translate(_target));
        return true;
    }

    glm::vec3 dir = vec / length;
    position += dir * dist;

    float facing = glm::half_pi<float>() - glm::atan(dir.x, dir.y);

    glm::mat4 transform(1.0f);
    transform = glm::translate(transform, position);
    transform *= glm::eulerAngleZ(facing);

    _model->setLocalTransform(transform);

    return false;
}

void Projectile::reset() {
    if (!_model) {
        return;
    }

    _model->graph().removeRoot(*_model);
    _model.reset();
    if (_flash) {
        _flash->graph().removeRoot(*_flash);
        _flash.reset();
    }
}

void ProjectileSequence::push_back(float time, Projectile::Source source, bool miss, AttackResultType result) {
    _projectiles.emplace_back(source, miss, result);
    _events.push_back(time, _projectiles.size());
}

void ProjectileSequence::update(float dt, Creature &attacker, Object &target,
                                scene::ISceneGraph &sceneGraph) {
    // Update projectiles in flight
    for (Projectile &proj : _projectiles) {
        if (proj.update(dt)) {
            // Projectile hit the target
            proj.reset();
        }
    }

    // Fire new projectiles
    _events.update(dt);
    while (TimeEvents::Event ev = _events.next()) {
        size_t index = ev - 1;
        _projectiles[index].fire(attacker, target, sceneGraph);
    }
}

void ProjectileSequence::reset() {
    for (Projectile &proj : _projectiles) {
        proj.reset();
    }
}

void addProjectilesFromSpec(ProjectileSequence &seq, const ProjectileSpec &spec, const AttackBuffer *attacks) {
    uint32_t remainingMisses = spec.misses;
    size_t handAttack[2] {};
    size_t numProjectiles = spec.projectiles.size();
    for (size_t i = 0; i < numProjectiles; ++i) {
        bool autoMiss = (i + remainingMisses) >= numProjectiles;
        bool miss = autoMiss || (remainingMisses && randomInt(0, 1));
        if (miss) {
            --remainingMisses;
        }
        const auto source = spec.projectiles[i].second == 0 ? Projectile::Main : Projectile::Offhand;
        auto result = AttackResultType::Invalid;
        if (!miss && attacks) {
            const bool offHand = source == Projectile::Offhand;
            result = attacks->rangedResult(offHand, handAttack[offHand]++);
            miss = result == AttackResultType::Miss || result == AttackResultType::AttackResisted ||
                result == AttackResultType::AttackFailed;
        }
        seq.push_back(spec.projectiles[i].first, source, miss, result);
    }
}

// Standard physical attack actions pass 1500 to SetPauseTimer.
static constexpr int kPhysicalAttackPauseMilliseconds = 1500;

AttackSchedule::State AttackSchedule::update(
    const CombatRound &round, Action &action, float dt) {

    if (round.suspends(action)) return _state;
    dt = round.actionDelta(dt);
    _time += dt;
    if (_melee && _state != AttackSchedule::WaitAttack) {
        float elapsedMilliseconds =
            dt * 1000.0f + _meleeElapsedRemainderMilliseconds;
        int wholeMilliseconds = static_cast<int>(elapsedMilliseconds);
        _meleeElapsedRemainderMilliseconds =
            elapsedMilliseconds - wholeMilliseconds;
        _meleeElapsedMilliseconds += wholeMilliseconds;
    }

    switch (_state) {
    case AttackSchedule::WaitAttack: {
        if (round.canExecute(action)) {
            _state = AttackSchedule::Attack;
        }
        break;
    }
    case AttackSchedule::Attack: {
        if (_melee &&
            _meleeElapsedMilliseconds >=
                _meleeCompletionMilliseconds) {
            _state = AttackSchedule::Damage;
        } else {
            _state = AttackSchedule::WaitDamage;
        }
        break;
    }
    case AttackSchedule::WaitDamage: {
        if ((_melee &&
             _meleeElapsedMilliseconds >=
                 _meleeCompletionMilliseconds) ||
            (!_melee && _time >= kAttackDamageDelay)) {
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

void AttackSchedule::startMelee(int latestImpactMilliseconds) {
    assert(_state == AttackSchedule::Attack &&
           "melee attack schedule has not started");

    _melee = true;
    _meleeElapsedMilliseconds = 0;
    _meleeCompletionMilliseconds = std::max(
        kPhysicalAttackPauseMilliseconds,
        latestImpactMilliseconds);
    _meleeElapsedRemainderMilliseconds = 0.0f;
}

bool navigateToAttackTarget(Creature &attacker, Object &target, float dt,
                            bool &reachedOnce, Game *game, const Action *parent) {
    if (reachedOnce) return true;

    const float range = attacker.getAttackRange();
    if (game && parent && game->module() && game->module()->area() &&
        attacker.getSquareDistanceTo(glm::vec2(target.position())) > range * range) {
        // Navigation actions inherit the parent action group and use the existing
        // range and pathfinding rules.
        auto targetObject = game->getObjectById(target.id());
        if (targetObject) {
            auto move = game->newAction<MoveToObjectAction>(
                std::move(targetObject), true, range, false, -1.0f,
                !isRangedWieldType(attacker.getWieldType()));
            if (attacker.addActionBefore(*parent, std::move(move))) return false;
        }
        // A parent removed by a callback must not continue navigating off-queue.
        return false;
    }

    if (!attacker.navigateTo(target.position(), true, range, dt)) {
        return false;
    }

    reachedOnce = true;
    return true;
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

std::string selectPhysicalMeleeAttackAnimation(
    Creature &attacker,
    const Object &target,
    CreatureWieldType wield) {

    bool creatureCombat = isCreatureCombat(attacker, target);
    bool cinematic = !creatureCombat && isMeleeWieldType(wield);
    int variant = attacker.selectMeleeAttackVariant(cinematic);
    return formatPhysicalMeleeAttackAnimation(
        wield,
        variant,
        attacker.modelType() == Creature::ModelType::Creature,
        cinematic);
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

std::string getRangedAttackAnim(Creature &attacker, int kind) {
    CreatureWieldType wield = attacker.getWieldType();
    assert(isRangedWieldType(wield) && "invalid wield");

    auto attackerModel = std::static_pointer_cast<scene::ModelSceneNode>(attacker.sceneNode());
    const graphics::Model &model = attackerModel->model();

    std::string animByWield = str(boost::format("b%da%d") % static_cast<int>(wield) % kind);
    if (hasAnim(model, animByWield)) {
        return animByWield;
    }

    std::string animBasic = str(boost::format("b0a%d") % kind);
    if (hasAnim(model, animBasic)) {
        return animBasic;
    }

    return "";
}

} // namespace game

} // namespace reone

namespace reone {
namespace game {

static int getDamageSlot(DamageType type) {
    int damageFlags = static_cast<int>(type);
    if (damageFlags <= 0) {
        throw std::invalid_argument("Damage breakdown type must be positive");
    }

    int slot = 0;
    while (damageFlags > 1) {
        damageFlags >>= 1;
        ++slot;
    }
    if (slot >= 14) {
        throw std::invalid_argument("Damage breakdown type is out of range");
    }
    return slot;
}

DamageBreakdown::DamageBreakdown() {
    rawDamageSlots.fill(-1);
}

void DamageBreakdown::addRawDamage(int amount, DamageType type) {
    int slot = getDamageSlot(type);
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

bool isRangedWieldType(CreatureWieldType type) {
    switch (type) {
    case CreatureWieldType::BlasterPistol:
    case CreatureWieldType::DualPistols:
    case CreatureWieldType::BlasterRifle:
    case CreatureWieldType::HeavyWeapon:
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
