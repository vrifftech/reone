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

#include "reone/game/effect/lightsaberthrow.h"
#include "reone/game/effect/forceshield.h"
#include "reone/game/effect/forcepushed.h"
#include "reone/game/effect/forcepushtargeted.h"
#include "reone/game/effect/forceresisted.h"
#include "reone/game/effect/forcefizzle.h"
#include "reone/game/effect/beam.h"
#include "reone/game/effect/spellimmunity.h"
#include "reone/game/effect/forceresistanceincrease.h"
#include "reone/game/effect/forceresistancedecrease.h"
#include "reone/game/effect.h"
#include "reone/game/effect/forcebody.h"
#include "reone/game/effect/bodyfuel.h"
#include "reone/game/effect/assureddeflection.h"
#include "reone/game/effect/blasterdeflectionincrease.h"
#include "reone/game/effect/blasterdeflectiondecrease.h"
#include "reone/game/effect/healforcepoints.h"
#include "reone/game/effect/temporaryforcepoints.h"
#include "reone/game/effect/temporaryhitpoints.h"
#include "reone/game/effect/modifyattacks.h"
#include "reone/game/effect/assuredhit.h"
#include "reone/game/effect/poison.h"
#include "reone/game/effect/haste.h"
#include "reone/game/effect/resurrection.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/paralyze.h"
#include "reone/game/effect/sleep.h"
#include "reone/game/effect/stunned.h"
#include "reone/game/effect/movementspeedincrease.h"
#include "reone/game/effect/movementspeeddecrease.h"
#include "reone/game/effect/damageshield.h"
#include "reone/game/effect/damageforcepoints.h"
#include "reone/game/effect/damage.h"
#include "reone/game/effect/heal.h"
#include "reone/game/effect/regenerate.h"
#include "reone/game/effect/death.h"

#include <stdexcept>

#include "reone/game/game.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/abilityincrease.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/acincrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/attackincrease.h"
#include "reone/game/effect/bonusfeat.h"
#include "reone/game/effect/disguise.h"
#include "reone/game/effect/concealment.h"
#include "reone/game/effect/damagedecrease.h"
#include "reone/game/effect/damageimmunitydecrease.h"
#include "reone/game/effect/damageimmunityincrease.h"
#include "reone/game/effect/damageincrease.h"
#include "reone/game/effect/damagereduction.h"
#include "reone/game/effect/damageresistance.h"
#include "reone/game/effect/immunity.h"
#include "reone/game/effect/invisibility.h"
#include "reone/game/effect/savingthrowdecrease.h"
#include "reone/game/effect/savingthrowincrease.h"
#include "reone/game/effect/seeinvisible.h"
#include "reone/game/effect/skilldecrease.h"
#include "reone/game/effect/skillincrease.h"
#include "reone/game/effect/trueseeing.h"
#include "reone/game/effect/ultravision.h"
#include "reone/game/location.h"
#include "reone/game/object.h"
#include "reone/game/object/creature.h"
#include "reone/resource/gff.h"
#include "reone/script/variable.h"
#include "reone/system/logutil.h"
#include "reone/game/effect/blind.h"
#include "reone/game/effect/misschance.h"
#include "reone/game/effect/hitpointchangewhendying.h"
#include "reone/game/effect/entangle.h"
#include "reone/game/effect/fury.h"
#include "reone/game/effect/factionmodifier.h"


namespace reone {

namespace game {

namespace {

struct VersusParameterIndices {
    size_t race;
    size_t lawChaos;
    size_t goodEvil;
};

uint16_t serializedEffectType(EffectType type) {
    switch (type) {
    case EffectType::Haste: return 1;
    case EffectType::Regenerate: return 7;
    case EffectType::TemporaryHitpoints: return 15;
    case EffectType::ModifyAttacks: return 44;
    case EffectType::Disguise: return 62;
    case EffectType::TimeStop: return 64;
    case EffectType::SpellLevelAbsorption: return 65;
    case EffectType::MissChance: return 75;
    case EffectType::ForceDrain: return 90;
    case EffectType::TemporaryForcePoints: return 91;
    case EffectType::BodyFuel: return 98;
    case EffectType::AssuredHit: return 101;
    case EffectType::HealForcePoints: return 96;

    case EffectType::DamageForcePoints: return 95;
    case EffectType::Death: return 19;
    case EffectType::Resurrection: return 4;
    case EffectType::Beam: return 32;
    case EffectType::HitPointChangeWhenDying: return 57;
    case EffectType::Heal: return 39;
    case EffectType::Damage: return 38;
    case EffectType::Visual: return 30;
    case EffectType::DamageShield: return 61;
    case EffectType::MovementSpeedIncrease: return 28;
    case EffectType::DamageResistance: return 2;
    case EffectType::AbilityIncrease: return 36;
    case EffectType::AbilityDecrease: return 37;
    case EffectType::AttackIncrease: return 10;
    case EffectType::AttackDecrease: return 11;
    case EffectType::DamageReduction: return 12;
    case EffectType::DamageIncrease: return 13;
    case EffectType::DamageDecrease: return 14;
    case EffectType::DamageImmunityIncrease: return 16;
    case EffectType::DamageImmunityDecrease: return 17;
    case EffectType::Immunity: return 22;
    case EffectType::SavingThrowIncrease: return 26;
    case EffectType::SavingThrowDecrease: return 27;
    case EffectType::Invisibility: return 47;
    case EffectType::ACIncrease: return 48;
    case EffectType::ACDecrease: return 49;
    case EffectType::SkillIncrease: return 55;
    case EffectType::SkillDecrease: return 56;
    case EffectType::Sanctuary: return 63;
    case EffectType::SeeInvisible: return 70;
    case EffectType::Ultravision: return 71;
    case EffectType::TrueSeeing: return 72;
    case EffectType::Blindness: return 73;
    case EffectType::Concealment: return 76;
    case EffectType::BonusFeat: return 83;
    case EffectType::Confused:
    case EffectType::Frightened:
    case EffectType::Stunned:
    case EffectType::Paralyze:
    case EffectType::Sleep:
    case EffectType::DroidStun:
    case EffectType::Choke:
    case EffectType::Horrified:
    case EffectType::WhirlWind: return 8;
    case EffectType::Slow: return 3;
    case EffectType::Entangle: return 18;
    case EffectType::MovementSpeedDecrease: return 29;
    case EffectType::ForceResistanceIncrease: return 33;
    case EffectType::SpellImmunity: return 50;
    case EffectType::ForcePushTargeted: return 60;
    case EffectType::ForceResistanceDecrease: return 34;
    case EffectType::Poison: return 35;
    case EffectType::LinkEffects: return 40;
    case EffectType::ForcePushed: return 60;
    case EffectType::BlasterDeflectionIncrease: return 92;
    case EffectType::BlasterDeflectionDecrease: return 93;
    case EffectType::AssuredDeflection: return 104;
    case EffectType::ForceFizzle: return 106;
    case EffectType::ForceResisted: return 105;
    case EffectType::LightsaberThrow: return 100;
    case EffectType::ForceShield: return 107;
    case EffectType::PureGoodPowers: return 108;
    case EffectType::PureEvilPowers: return 109;
    case EffectType::ForceBody: return 110;
    case EffectType::Fury: return 111;
    case EffectType::FPRegenModifier: return 112;
    case EffectType::VPRegenModifier: return 113;
    case EffectType::ForceSight: return 114;
    case EffectType::FactionModifier: return 115;
    case EffectType::DestroyShields: return 116;
    case EffectType::Assassinate: return 117;
    case EffectType::Disease: return 5;
    case EffectType::Knockdown: return 20;
    case EffectType::Deaf: return 21;
    case EffectType::EnemyAttackBonus: return 24;
    case EffectType::ArcaneSpellFailure: return 25;
    case EffectType::AreaOfEffect: return 31;
    case EffectType::Curse: return 45;
    case EffectType::Silence: return 46;
    case EffectType::DispelMagicAll: return 51;
    case EffectType::DispelMagicBest: return 52;
    case EffectType::Darkness: return 74;
    case EffectType::NegativeLevel: return 82;
    case EffectType::PsychicStatic: return 99;
    case EffectType::ForceJump: return 102;
    default: return 0; // No identity; never reinterpret a script enum ordinal.
    }
}

EffectType runtimeEffectType(uint16_t serializedType, int state) {
    switch (serializedType) {
    case 1: return EffectType::Haste;
    case 7: return EffectType::Regenerate;
    case 15: return EffectType::TemporaryHitpoints;
    case 44: return EffectType::ModifyAttacks;
    case 62: return EffectType::Disguise;
    case 64: return EffectType::TimeStop;
    case 65: return EffectType::SpellLevelAbsorption;
    case 75: return EffectType::MissChance;
    case 90: return EffectType::ForceDrain;
    case 91: return EffectType::TemporaryForcePoints;
    case 98: return EffectType::BodyFuel;
    case 101: return EffectType::AssuredHit;
    case 96: return EffectType::HealForcePoints;

    case 3: return EffectType::Slow;
    case 9: case 23: case 67: return EffectType::Invalid;
    case 95: return EffectType::DamageForcePoints;
    case 19: return EffectType::Death;
    case 4: return EffectType::Resurrection;
    case 32: return EffectType::Beam;
    case 57: return EffectType::HitPointChangeWhenDying;
    case 39: return EffectType::Heal;
    case 38: return EffectType::Damage;
    case 30: return EffectType::Visual;
    case 61: return EffectType::DamageShield;
    case 28: return EffectType::MovementSpeedIncrease;
    case 8:
        switch (state) {
        case 1: return EffectType::Confused;
        case 2: return EffectType::Frightened;
        case 3: return EffectType::DroidStun;
        case 7: return EffectType::Choke;
        case 10: return EffectType::WhirlWind;
        case 15: return EffectType::Crush;
        case 4: return EffectType::Stunned;
        case 5: return EffectType::Paralyze;
        case 6: return EffectType::Sleep;
        case 9: return EffectType::ForcePushed;
        case 8: return EffectType::Horrified;
        case 18: return EffectType::MindTrick;
        case 19: return EffectType::DroidScramble;
        default: return EffectType::Invalid;
        }
    case 2: return EffectType::DamageResistance;
    case 36: return EffectType::AbilityIncrease;
    case 37: return EffectType::AbilityDecrease;
    case 10: return EffectType::AttackIncrease;
    case 11: return EffectType::AttackDecrease;
    case 12: return EffectType::DamageReduction;
    case 13: return EffectType::DamageIncrease;
    case 14: return EffectType::DamageDecrease;
    case 16: return EffectType::DamageImmunityIncrease;
    case 17: return EffectType::DamageImmunityDecrease;
    case 22: return EffectType::Immunity;
    case 26: return EffectType::SavingThrowIncrease;
    case 27: return EffectType::SavingThrowDecrease;
    case 47: return EffectType::Invisibility;
    case 48: return EffectType::ACIncrease;
    case 49: return EffectType::ACDecrease;
    case 55: return EffectType::SkillIncrease;
    case 56: return EffectType::SkillDecrease;
    case 63: return EffectType::Sanctuary;
    case 70: return EffectType::SeeInvisible;
    case 71: return EffectType::Ultravision;
    case 72: return EffectType::TrueSeeing;
    case 73: return EffectType::Blindness;
    case 76: return EffectType::Concealment;
    case 83: return EffectType::BonusFeat;
    case 18: return EffectType::Entangle;
    case 29: return EffectType::MovementSpeedDecrease;
    case 33: return EffectType::ForceResistanceIncrease;
    case 50: return EffectType::SpellImmunity;
    case 34: return EffectType::ForceResistanceDecrease;
    case 35: return EffectType::Poison;
    case 40: return EffectType::LinkEffects;
    case 60: return EffectType::ForcePushed;
    case 92: return EffectType::BlasterDeflectionIncrease;
    case 93: return EffectType::BlasterDeflectionDecrease;
    case 104: return EffectType::AssuredDeflection;
    case 100: return EffectType::LightsaberThrow;
    case 105: return EffectType::ForceResisted;
    case 106: return EffectType::ForceFizzle;
    case 107: return EffectType::ForceShield;
    case 108: return EffectType::PureGoodPowers;
    case 109: return EffectType::PureEvilPowers;
    case 110: return EffectType::ForceBody;
    case 111: return EffectType::Fury;
    case 112: return EffectType::FPRegenModifier;
    case 113: return EffectType::VPRegenModifier;
    case 114: return EffectType::ForceSight;
    case 115: return EffectType::FactionModifier;
    case 116: return EffectType::DestroyShields;
    case 117: return EffectType::Assassinate;
    case 5: return EffectType::Disease;
    case 20: return EffectType::Knockdown;
    case 21: return EffectType::Deaf;
    case 24: return EffectType::EnemyAttackBonus;
    case 25: return EffectType::ArcaneSpellFailure;
    case 31: return EffectType::AreaOfEffect;
    case 45: return EffectType::Curse;
    case 46: return EffectType::Silence;
    case 51: return EffectType::DispelMagicAll;
    case 52: return EffectType::DispelMagicBest;
    case 74: return EffectType::Darkness;
    case 82: return EffectType::NegativeLevel;
    case 99: return EffectType::PsychicStatic;
    case 102: return EffectType::ForceJump;
    default: return EffectType::Invalid; // The raw identity remains in EffectInstance.
    }
}

std::optional<VersusParameterIndices> versusParameterIndices(uint16_t serializedType) {
    switch (serializedType) {
    case 10: // Attack Increase
    case 11: // Attack Decrease
    case 13: // Damage Increase
    case 14: // Damage Decrease
    case 48: // AC Increase
    case 49: // AC Decrease
    case 55: // Skill Increase
    case 56: // Skill Decrease
        return VersusParameterIndices {2, 3, 4};
    case 22: // Immunity
    case 76: // Concealment
        return VersusParameterIndices {1, 2, 3};
    case 26: // Saving Throw Increase
    case 27: // Saving Throw Decrease
        return VersusParameterIndices {3, 4, 5};
    case 47: // Invisibility
    case 63: // Sanctuary
        return VersusParameterIndices {1, 2, 3};
    default:
        return std::nullopt;
    }
}

std::shared_ptr<Effect> executableEffect(const EffectInstance &instance) {
    auto integer = [&instance](size_t index, int32_t fallback = 0) {
        return instance.integerParameter(index, fallback);
    };
    switch (instance.serializedType) {
    case 7: return std::make_shared<RegenerateEffect>(integer(0), integer(1), integer(4));
    case 15: return std::make_shared<TemporaryHitPointsEffect>(integer(0));
    case 91: return std::make_shared<TemporaryForcePointsEffect>(integer(0));
    case 96: return std::make_shared<HealForcePointsEffect>(integer(0));
    case 98: return std::make_shared<BodyFuelEffect>();
    case 110: return std::make_shared<ForceBodyEffect>(integer(0));
    case 1: case 3: return std::make_shared<HasteSlowEffect>(instance.serializedType == 1);
    case 41: return std::make_shared<HasteSlowInternalEffect>(true);
    case 42: return std::make_shared<HasteSlowInternalEffect>(false);
    case 8:
        if (integer(0) == 9) return std::make_shared<ForcePushStateEffect>();
        switch (static_cast<CreatureState>(integer(0))) {
        case CreatureState::Stun: return std::make_shared<StunnedEffect>();
        case CreatureState::Paralysis: return std::make_shared<ParalyzeEffect>();
        case CreatureState::Sleep: return std::make_shared<SleepEffect>();
        default:
            return std::make_shared<CreatureStateEffect>(
                static_cast<CreatureState>(integer(0)));
        }
    case 9: return std::make_shared<CreatureStateInternalEffect>(static_cast<CreatureState>(integer(0)));
    case 23: return std::make_shared<CreatureAIStateEffect>(integer(0));
    case 59: return std::make_shared<LimitMovementSpeedEffect>();
    case 67: return std::make_shared<EffectIconMarkerEffect>(integer(0));
    case 44:
        return std::make_shared<ModifyAttacksEffect>(integer(0));
    case 101:
        return std::make_shared<AssuredHitEffect>();
    case 28:
        return std::make_shared<MovementSpeedIncreaseEffect>(integer(0));
    case 29:
        return std::make_shared<MovementSpeedDecreaseEffect>(integer(0));
    case 61:
        return std::make_shared<DamageShieldEffect>(integer(0), integer(1), static_cast<DamageType>(integer(2)));
    case 30:
        return std::make_shared<VisualEffectMarkerEffect>(integer(0));
    case 32:
        return std::make_shared<BeamEffect>(integer(0), instance.boundObjectParameter(0), static_cast<BodyNode>(integer(1)), integer(2) != 0);
    case 60:
        if (integer(0) == 1) {
            auto centre = std::make_shared<Location>(glm::vec3(instance.floatParameters[0],
                instance.floatParameters[1], instance.floatParameters[2]), 0.0f);
            return std::make_shared<ForcePushTargetedEffect>(centre, integer(1) != 0);
        }
        return std::make_shared<ForcePushedEffect>();
    case 107:
        return std::make_shared<ForceShieldEffect>(integer(0));
    case 100:
        return std::make_shared<LightsaberThrowEffect>(instance.boundObjectParameter(0),
            instance.boundObjectParameter(1), instance.boundObjectParameter(2), integer(0));
    case 106:
        return std::make_shared<ForceFizzleEffect>();
    case 105:
        return std::make_shared<ForceResistedEffect>(instance.boundObjectParameter(0));
    case 33:
        return std::make_shared<ForceResistanceIncreaseEffect>(integer(0));
    case 34:
        return std::make_shared<ForceResistanceDecreaseEffect>(integer(0));
    case 50:
        return std::make_shared<SpellImmunityEffect>(static_cast<SpellType>(integer(0)));
    case 92: return std::make_shared<BlasterDeflectionIncreaseEffect>(integer(0));
    case 93: return std::make_shared<BlasterDeflectionDecreaseEffect>(integer(0));
    case 104: return std::make_shared<AssuredDeflectionEffect>(integer(0));
    case 95:
        return std::make_shared<DamageForcePointsEffect>(integer(0));
    case 38:
        return std::make_shared<DamageEffect>(instance);
    case 39:
        return std::make_shared<HealEffect>(integer(0));
    case 19:
        return std::make_shared<DeathEffect>(integer(0) != 0, integer(1) != 0, integer(2) != 0);
    case 35:
        return std::make_shared<PoisonEffect>(static_cast<Poison>(integer(0)));
    case 4:
        return std::make_shared<ResurrectionEffect>(integer(0));
    case 36:
        return std::make_shared<AbilityIncreaseEffect>(
            static_cast<Ability>(integer(0)), integer(1));
    case 37:
        return std::make_shared<AbilityDecreaseEffect>(
            static_cast<Ability>(integer(0)), integer(1));
    case 62:
        return std::make_shared<DisguiseEffect>(integer(0));
    case 83:
        return std::make_shared<BonusFeatEffect>(
            static_cast<FeatType>(integer(0)));
    case 10:
        return std::make_shared<AttackIncreaseEffect>(
            integer(0), static_cast<AttackBonus>(integer(1)));
    case 11:
        return std::make_shared<AttackDecreaseEffect>(
            integer(0), static_cast<AttackBonus>(integer(1)));
    case 13:
        return std::make_shared<DamageIncreaseEffect>(
            integer(0), static_cast<DamageType>(integer(1)));
    case 14:
        return std::make_shared<DamageDecreaseEffect>(
            integer(0), static_cast<DamageType>(integer(1)));
    case 48:
        return std::make_shared<ACIncreaseEffect>(
            integer(1), static_cast<ACBonus>(integer(0)),
            integer(5, kPhysicalDamageTypeFlags));
    case 49:
        return std::make_shared<ACDecreaseEffect>(
            integer(1), static_cast<ACBonus>(integer(0)),
            integer(5, kPhysicalDamageTypeFlags));
    case 26:
        return std::make_shared<SavingThrowIncreaseEffect>(
            integer(1), integer(0),
            static_cast<SavingThrowType>(integer(2)));
    case 27:
        return std::make_shared<SavingThrowDecreaseEffect>(
            integer(1), integer(0),
            static_cast<SavingThrowType>(integer(2)));
    case 55:
        return std::make_shared<SkillIncreaseEffect>(
            static_cast<SkillType>(integer(0)), integer(1));
    case 56:
        return std::make_shared<SkillDecreaseEffect>(
            static_cast<SkillType>(integer(0)), integer(1));
    case 22:
        return std::make_shared<ImmunityEffect>(
            static_cast<ImmunityType>(integer(0)));
    case 76:
        return std::make_shared<ConcealmentEffect>(integer(0));
    case 16:
        return std::make_shared<DamageImmunityIncreaseEffect>(
            static_cast<DamageType>(integer(0)), integer(1));
    case 17:
        return std::make_shared<DamageImmunityDecreaseEffect>(
            static_cast<DamageType>(integer(0)), integer(1));
    case 2:
        return std::make_shared<DamageResistanceEffect>(
            static_cast<DamageType>(integer(0)), integer(1), integer(2), integer(3));
    case 12:
        return std::make_shared<DamageReductionEffect>(
            integer(0), static_cast<DamagePower>(integer(1)), integer(2));
    case 47:
        return std::make_shared<InvisibilityEffect>(
            static_cast<InvisibilityType>(integer(0)));
    case 57:
        return std::make_shared<HitPointChangeWhenDyingEffect>(instance.floatParameters[0]);
    case 73:
        return std::make_shared<BlindEffect>();
    case 75:
        return std::make_shared<MissChanceEffect>(integer(0));
    case 69:
        return std::make_shared<VisionEffect>(integer(0));
    case 70:
        return std::make_shared<SeeInvisibleEffect>();
    case 71:
        return std::make_shared<UltravisionEffect>();
    case 72:
        return std::make_shared<TrueSeeingEffect>();
    case 18: return std::make_shared<EntangleEffect>();
    case 111: return std::make_shared<FuryEffect>();
    case 115: return std::make_shared<FactionModifierEffect>(integer(0));
    default:
        return nullptr;
    }
}

} // namespace

EffectId EffectIdNamespace::allocate() {
    while (_ids.count(_nextId) != 0) {
        if (_nextId == std::numeric_limits<EffectId>::max()) {
            throw std::overflow_error("Effect ID namespace exhausted");
        }
        ++_nextId;
    }
    if (_nextId == kUnassignedEffectId || _nextId == std::numeric_limits<EffectId>::max()) {
        throw std::overflow_error("Effect ID namespace exhausted");
    }
    EffectId id = _nextId++;
    _ids.insert(id);
    return id;
}

EffectIdImportResult EffectIdNamespace::importId(EffectId id) {
    if (id == kUnassignedEffectId) {
        return EffectIdImportResult::Unassigned;
    }
    auto [_, inserted] = _ids.insert(id);
    return inserted ? EffectIdImportResult::Imported : EffectIdImportResult::Existing;
}

bool EffectIdNamespace::setNextId(EffectId id) {
    if (id == kUnassignedEffectId || id == std::numeric_limits<EffectId>::max()) {
        return false;
    }
    _nextId = id;
    return true;
}

void EffectIdNamespace::reset() {
    _nextId = kFirstId;
    _ids.clear();
}

std::shared_ptr<script::EngineType> Effect::cloneForScript() const {
    return std::make_shared<Effect>(*this);
}

EffectApplicationResult Effect::onApply(Object &, EffectInstance &) {
    debug("Unsupported effect type: " + std::to_string(static_cast<int>(_type)));
    return EffectApplicationResult::Retained;
}

EffectRemovalResult Effect::onRemove(Object &object, const EffectInstance &) {

    return EffectRemovalResult::Removed;
}

void Effect::onUpdate(Object &, const EffectInstance &, float) {
}

void Effect::setSubType(uint16_t category) {
    _saveFacingSubType = static_cast<uint16_t>((_saveFacingSubType & ~0x18u) | (category & 0x18u));
}

EffectInstance Effect::saveFacingInstance() const {
    if (!_saveFacingRepresentable) {
        throw std::runtime_error(
            "live effect has no serializable value");
    }
    EffectInstance result;
    result.id = _saveFacingId;
    result.serializedType = serializedEffectType(_type);
    // Duration and effect category occupy independent parts of this field.
    result.subType = _saveFacingSubType;
    result.exposed = 1;
    result.creatorId = kSavedEffectInvalidObjectId;
    result.spellId = _saveFacingSpellId;
    result.integerParameters = _saveFacingIntegers;
    // Script state effects share serialized type 8; the semantic state belongs in
    // its first integer, not in the type field.
    if (result.serializedType == 8) {
        result.integerParameters.resize(std::max<size_t>(1, result.integerParameters.size()));
        switch (_type) {
        case EffectType::Confused: result.integerParameters[0] = 1; break;
        case EffectType::Frightened: result.integerParameters[0] = 2; break;
        case EffectType::Stunned: result.integerParameters[0] = 4; break;
        case EffectType::Paralyze: result.integerParameters[0] = 5; break;
        case EffectType::Sleep: result.integerParameters[0] = 6; break;
        case EffectType::DroidStun: result.integerParameters[0] = 3; break;
        case EffectType::Choke: result.integerParameters[0] = 7; break;
        case EffectType::Horrified: result.integerParameters[0] = 8; break;
        case EffectType::WhirlWind: result.integerParameters[0] = 10; break;
        default: break;
        }
    }
    result.floatParameters = _saveFacingFloats;
    result.stringParameters = _saveFacingStrings;
    // Preserve the exact candidate binding even while its owner graph remains
    // Constructing. Resolution intentionally begins to succeed only after the
    // candidate incarnation is published Live.
    result.creator = _saveFacingCreator;
    if (auto creator = _saveFacingCreator.resolve()) {
        result.creatorId = creator->id();
    }
    for (size_t index = 0; index < _saveFacingObjects.size(); ++index) {
        if (auto object = _saveFacingObjects[index].resolve()) {
            result.objectParameters[index] = object->id();
            result.objectParameterObjects[index] = object;
        }
    }
    return result;
}

void Effect::setSaveFacingCreator(const std::shared_ptr<Object> &creator) {
    _saveFacingCreator = creator;
}

void Effect::setSaveFacingSpellId(int32_t spellId) {
    // The game uses an all-bits-set SpellId for an independently applied effect;
    // zero is a valid semantic grouping value.
    _saveFacingSpellId = static_cast<uint32_t>(spellId);
}

namespace {
bool setAlignmentParameters(uint16_t type, std::vector<int32_t> &parameters,
                            int lawChaos, int goodEvil) {
    const auto indices = versusParameterIndices(type);
    if (!indices) return false;
    const bool missingRace = parameters.size() <= indices->race;
    parameters.resize(std::max(parameters.size(), indices->goodEvil + 1));
    if (missingRace) parameters[indices->race] = static_cast<int>(RacialType::All);
    parameters[indices->lawChaos] = lawChaos;
    parameters[indices->goodEvil] = goodEvil;
    return true;
}
bool setRaceParameter(uint16_t type, std::vector<int32_t> &parameters, int race) {
    const auto indices = versusParameterIndices(type);
    if (!indices) return false;
    parameters.resize(std::max(parameters.size(), indices->race + 1));
    parameters[indices->race] = race;
    return true;
}
} // namespace

bool Effect::setVersusAlignment(int lawChaos, int goodEvil) {
    return setAlignmentParameters(serializedEffectType(_type), _saveFacingIntegers,
                                  lawChaos, goodEvil);
}

bool Effect::setVersusRacialType(int racialType) {
    return setRaceParameter(serializedEffectType(_type), _saveFacingIntegers, racialType);
}

void Effect::captureSaveFacingScriptArguments(
    const std::vector<script::Variable> &arguments,
    const Game &game) {
    switch (_type) {
    case EffectType::Invalid:
    case EffectType::Regenerate:
    case EffectType::Damage:
    case EffectType::DamageResistance:
    case EffectType::DamageReduction:
    case EffectType::DamageImmunityIncrease:
    case EffectType::DamageImmunityDecrease:
    case EffectType::ACIncrease:
    case EffectType::ACDecrease:
        // These constructors author the canonical payload themselves.
        // Re-capturing raw arguments would undo their normalization/defaults.
        return;
    default:
        break;
    }
    if (_type == EffectType::LinkEffects) {
        // Linked values serialize as a flat type-40 record. Child pointers remain
        // runtime-only and are not recursively encoded here.
        return;
    }

    size_t integerIndex = 0;
    size_t floatIndex = 0;
    size_t stringIndex = 0;
    size_t objectIndex = 0;
    for (const auto &argument : arguments) {
        switch (argument.type) {
        case script::VariableType::Int:
            if (_type == EffectType::Visual && integerIndex == 1) {
                setSaveFacingInteger(2, argument.intValue);
            } else if ((_type == EffectType::SavingThrowIncrease ||
                        _type == EffectType::SavingThrowDecrease) &&
                       integerIndex < 3) {
                static constexpr std::array<size_t, 3> kSaveParameterOrder {1, 0, 2};
                setSaveFacingInteger(
                    kSaveParameterOrder[integerIndex], argument.intValue);
                setSaveFacingInteger(3, static_cast<int>(RacialType::All));
            } else if (_type == EffectType::ForcePushTargeted) {
                setSaveFacingInteger(1, argument.intValue);
            } else if (_type != EffectType::LightsaberThrow &&
                       _type != EffectType::DamageShield &&
                       _type != EffectType::Damage) {
                setSaveFacingInteger(integerIndex, argument.intValue);
            }
            ++integerIndex;
            break;
        case script::VariableType::Float:
            if (floatIndex < _saveFacingFloats.size()) {
                setSaveFacingFloat(floatIndex++, argument.floatValue);
            }
            break;
        case script::VariableType::String:
            if (stringIndex < _saveFacingStrings.size()) {
                setSaveFacingString(stringIndex++, argument.strValue);
            }
            break;
        case script::VariableType::Object:
            if (objectIndex < _saveFacingObjects.size()) {
                setSaveFacingObject(
                    objectIndex++, game.getObjectById(argument.objectId));
            }
            break;
        case script::VariableType::Location: {
            auto location = std::dynamic_pointer_cast<Location>(
                argument.engineType);
            if (!location) {
                _saveFacingRepresentable = false;
                break;
            }
            const auto &position = location->position();
            for (float value : {position.x, position.y, position.z}) {
                if (floatIndex < _saveFacingFloats.size()) {
                    setSaveFacingFloat(floatIndex++, value);
                }
            }
            break;
        }
        case script::VariableType::Effect:
            _saveFacingRepresentable = false;
            break;
        default:
            break;
        }
    }
}

void Effect::setSaveFacingInteger(size_t index, int32_t value) {
    if (_saveFacingIntegers.size() <= index) {
        _saveFacingIntegers.resize(index + 1);
    }
    _saveFacingIntegers[index] = value;
}

void Effect::setSaveFacingFloat(size_t index, float value) {
    if (index >= _saveFacingFloats.size()) {
        throw std::out_of_range("effect float parameter index");
    }
    _saveFacingFloats[index] = value;
}

void Effect::setSaveFacingString(size_t index, std::string value) {
    if (index >= _saveFacingStrings.size()) {
        throw std::out_of_range("effect string parameter index");
    }
    _saveFacingStrings[index] = std::move(value);
}

void Effect::setSaveFacingObject(
    size_t index,
    const std::shared_ptr<Object> &object) {
    if (index >= _saveFacingObjects.size()) {
        throw std::out_of_range("effect object parameter index");
    }
    _saveFacingObjects[index] = object;
}

EffectInstance EffectInstance::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    EffectInstance result;
    result.serializedReferenceContext = identityContext;
    result.id = gff.getUint64("Id");
    result.serializedType = static_cast<uint16_t>(gff.getUint("Type"));
    result.subType = static_cast<uint16_t>(gff.getUint("SubType"));
    result.duration = gff.getFloat("Duration");
    result.skipOnLoad = gff.getBool("SkipOnLoad");
    result.expiryDay = gff.getUint("ExpireDay");
    result.expiryTime = gff.getUint("ExpireTime");
    if (result.durationType() == DurationType::Temporary) {
        result.expiryOrigin = EffectExpiryOrigin::LoadedAbsoluteGameTime;
    }
    result.creatorId = gff.getUint("CreatorId");
    result.spellId = gff.getUint("SpellId", std::numeric_limits<uint32_t>::max());
    result.exposed = gff.getInt("IsExposed");

    int32_t integerCount = glm::max(0, gff.getInt("NumIntegers"));
    result.integerParameters.assign(static_cast<size_t>(integerCount), 0);
    auto integers = gff.getList("IntList");
    size_t copyIntegerCount = glm::min(result.integerParameters.size(), integers.size());
    for (size_t i = 0; i < copyIntegerCount; ++i) {
        result.integerParameters[i] = integers[i]->getInt("Value");
    }

    auto floats = gff.getList("FloatList");
    for (size_t i = 0; i < glm::min(result.floatParameters.size(), floats.size()); ++i) {
        result.floatParameters[i] = floats[i]->getFloat("Value");
    }

    auto strings = gff.getList("StringList");
    for (size_t i = 0; i < glm::min(result.stringParameters.size(), strings.size()); ++i) {
        result.stringParameters[i] = strings[i]->getString("Value");
    }

    auto objects = gff.getList("ObjectList");
    for (size_t i = 0; i < glm::min(result.objectParameters.size(), objects.size()); ++i) {
        result.objectParameters[i] = objects[i]->getUint("Value", kSavedEffectInvalidObjectId);
    }
    // Executable construction happens after the saved graph binds creators
    // and object parameters, when applyEffect/restoreEffect materializes it.
    return result;
}

std::shared_ptr<resource::Gff> EffectInstance::toGff() const {
    using resource::Gff;
    auto result = Gff::Builder().type(2)
        .field(Gff::Field::newDword64("Id", id))
        .field(Gff::Field::newWord("Type", serializedType))
        .field(Gff::Field::newWord("SubType", subType))
        .field(Gff::Field::newFloat("Duration", duration))
        .field(Gff::Field::newByte("SkipOnLoad", skipOnLoad))
        .field(Gff::Field::newDword("ExpireDay", expiryDay))
        .field(Gff::Field::newDword("ExpireTime", expiryTime))
        .field(Gff::Field::newDword("CreatorId", creatorId))
        .field(Gff::Field::newDword("SpellId", spellId))
        .field(Gff::Field::newInt("IsExposed", exposed))
        .field(Gff::Field::newInt("NumIntegers", static_cast<int32_t>(integerParameters.size())))
        .build();

    std::vector<std::shared_ptr<Gff>> ints;
    for (int32_t value : integerParameters) {
        ints.push_back(Gff::Builder().type(3).field(Gff::Field::newInt("Value", value)).build());
    }
    std::vector<std::shared_ptr<Gff>> floats;
    for (float value : floatParameters) {
        floats.push_back(Gff::Builder().type(4).field(Gff::Field::newFloat("Value", value)).build());
    }
    std::vector<std::shared_ptr<Gff>> strings;
    for (const auto &value : stringParameters) {
        strings.push_back(Gff::Builder().type(5).field(Gff::Field::newCExoString("Value", value)).build());
    }
    std::vector<std::shared_ptr<Gff>> objects;
    for (uint32_t value : objectParameters) {
        objects.push_back(Gff::Builder().type(6).field(Gff::Field::newDword("Value", value)).build());
    }
    result->fields().push_back(Gff::Field::newList("IntList", std::move(ints)));
    result->fields().push_back(Gff::Field::newList("FloatList", std::move(floats)));
    result->fields().push_back(Gff::Field::newList("StringList", std::move(strings)));
    result->fields().push_back(Gff::Field::newList("ObjectList", std::move(objects)));
    return result;
}

DurationType EffectInstance::durationType() const {
    switch (subType & 0x7) {
    case 0:
        return DurationType::Instant;
    case 1:
        return DurationType::Temporary;
    case 2:
        return DurationType::Permanent;
    case 3:
        return DurationType::Equipped;
    case 4:
        return DurationType::Innate;
    default:
        return DurationType::Invalid;
    }
}

void EffectInstance::setDuration(DurationType type, float seconds) {
    subType = static_cast<uint16_t>((subType & ~uint16_t(7)) |
                                   static_cast<uint16_t>(type));
    duration = seconds;
    remainingDuration = type == DurationType::Temporary
                            ? std::optional<float>(seconds) : std::nullopt;
    expiryOrigin = type == DurationType::Temporary
                       ? EffectExpiryOrigin::RuntimeCountdown : EffectExpiryOrigin::None;
    expiryDay = 0;
    expiryTime = 0;
}

void EffectInstance::setIntegerParameter(size_t index, int32_t value) {
    if (integerParameters.size() <= index) integerParameters.resize(index + 1);
    integerParameters[index] = value;
}

bool EffectInstance::shouldRestoreOnLoad() const {
    if (skipOnLoad || durationType() == DurationType::Equipped) return false;
    // These are operations, never retained EffectList applications. Duration
    // alone is not a retention policy: accepted Instant modifiers are valid.
    switch (serializedType) {
    case 4: case 19: case 38: case 39: case 69: case 95: case 96:
        return false;
    default:
        return true;
    }
}

void EffectInstance::materialize() {
    if (!effect) effect = executableEffect(*this);
}

EffectInstance EffectInstance::linkedChild(const std::shared_ptr<Effect> &child) const {
    EffectInstance result = child->saveFacingInstance();
    if (!std::dynamic_pointer_cast<SavedEffectValue>(child)) result.effect = child;
    result.id = id;
    result.creatorId = creatorId;
    result.creator = creator;
    result.spellId = spellId;
    result.serializedReferenceContext = serializedReferenceContext;
    result._runtimeSession = _runtimeSession;
    result._savedGraph = _savedGraph;
    result.subType = subType;
    result.duration = duration;
    result.remainingDuration = remainingDuration;
    result.expiryOrigin = expiryOrigin;
    result.expiryDay = expiryDay;
    result.expiryTime = expiryTime;
    result.restoring = restoring;
    return result;
}

EffectType EffectInstance::type() const {
    return runtimeEffectType(serializedType, integerParameter(0));
}

int32_t EffectInstance::integerParameter(
    size_t index, int32_t defaultValue) const {
    return index < integerParameters.size()
               ? integerParameters[index]
               : defaultValue;
}

std::shared_ptr<Object> EffectInstance::boundCreator() const {
    return creator.resolve();
}

std::shared_ptr<Object> EffectInstance::boundObjectParameter(
    size_t index) const {
    return index < objectParameterObjects.size()
               ? objectParameterObjects[index].resolve()
               : nullptr;
}

bool EffectInstance::appliesVersus(const Creature *creature) const {
    auto indices = versusParameterIndices(serializedType);
    if (!indices) {
        return true;
    }

    int race = integerParameter(
        indices->race, static_cast<int>(RacialType::All));
    int goodEvil = integerParameter(
        indices->goodEvil, static_cast<int>(Alignment::All));
    // Serialized effect records may use 28 for the script-facing RACIAL_TYPE_ALL
    // selector, while reone's compact internal RacialType enum uses 7.
    // Accept both supported selectors so Entangle's generated
    // Attack Decrease remains unconditional.
    const bool allRaces = race == static_cast<int>(RacialType::All) || race == 28;
    bool hasRace = serializedType == 76
                       ? race != 0
                       : !allRaces;
    bool hasAlignment = goodEvil != static_cast<int>(Alignment::All);
    if (!hasRace && !hasAlignment) {
        return true;
    }
    if (!creature) {
        return false;
    }
    if (hasRace && race != static_cast<int>(creature->racialType())) {
        return false;
    }
    return !hasAlignment || goodEvil == static_cast<int>(creature->alignment());
}

bool EffectInstance::hasLiveRuntimeSource() const {
    return durationType() != DurationType::Equipped ||
           static_cast<bool>(boundCreator());
}

bool EffectInstance::bindCreator(const std::shared_ptr<Object> &object) {
    creator.reset();
    if (!object) {
        return false;
    }
    creator = object;
    return true;
}

bool EffectInstance::bindObjectParameter(
    size_t index, const std::shared_ptr<Object> &object) {
    if (index >= objectParameterObjects.size()) {
        return false;
    }
    objectParameterObjects[index].reset();
    if (!object) {
        return false;
    }
    objectParameterObjects[index] = object;
    return true;
}

void EffectInstance::retireAreaRuntimeBindings(
    const std::set<const Object *> &retainedObjects) {
    auto retain = [&retainedObjects](
                      RuntimeObjectRef<Object> &binding,
                      uint32_t &identity) {
        auto object = binding.resolve();
        if (!object || retainedObjects.count(object.get()) == 0) {
            binding.reset();
            identity = kSavedEffectInvalidObjectId;
            return;
        }
        identity = object->id();
    };

    retain(creator, creatorId);
    for (size_t index = 0; index < objectParameters.size(); ++index) {
        retain(objectParameterObjects[index], objectParameters[index]);
    }
    serializedReferenceContext.reset();
    _savedGraph.reset();
    _runtimeSession.reset();
}

SavedEffectValue::SavedEffectValue(EffectInstance instance) :
    CopyableEffect(instance.type()),
    _instance(std::move(instance)) {
    // Script values copy the current canonical application, not its constructor.
    // A future application decodes these current parameters after reference binding.
    _instance.effect.reset();
}

void SavedEffectValue::setSubType(uint16_t category) {
    _instance.subType = static_cast<uint16_t>((_instance.subType & ~0x18u) | (category & 0x18u));
}

bool SavedEffectValue::setVersusAlignment(int lawChaos, int goodEvil) {
    return setAlignmentParameters(_instance.serializedType, _instance.integerParameters,
                                  lawChaos, goodEvil);
}

bool SavedEffectValue::setVersusRacialType(int racialType) {
    return setRaceParameter(_instance.serializedType, _instance.integerParameters, racialType);
}

} // namespace game

} // namespace reone
