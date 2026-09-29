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

#include "reone/game/effect/damage.h"

#include "reone/system/logutil.h"

#include "reone/game/effect/damageimmunitydecrease.h"
#include "reone/game/effect/damageimmunityincrease.h"
#include "reone/game/effect/damagereduction.h"
#include "reone/game/effect/damageresistance.h"
#include "reone/game/object.h"
#include "reone/game/combatfeedback.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <stdexcept>
#include <cmath>
#include "reone/game/effect/death.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/hitpointchangewhendying.h"
#include "reone/game/object/area.h"
#include "reone/game/object/module.h"
#include "reone/resource/provider/2das.h"
#include "reone/game/effect/heal.h"
#include "reone/game/effect/resurrection.h"
#include "reone/game/effect/damageforcepoints.h"
#include "reone/game/effect/forcedrain.h"
#include "reone/game/effect/healforcepoints.h"

namespace reone {

namespace game {

namespace {

class DamageAbsorption {
public:
    DamageAbsorption(int amount, int limit) :
        _amount(amount),
        _limit(limit),
        _limited(limit > 0) {
    }

    int amount() const { return _amount; }

    int absorb(int damage) {
        if (damage <= 0 || _amount <= 0) {
            return 0;
        }
        if (!_limited) {
            return std::min(damage, _amount);
        }

        int remaining = _limit;
        _limit = std::max(0, _limit - damage);
        return std::min(damage, _limit == 0 ? remaining : _amount);
    }

    bool exhausted() const { return _limited && _limit == 0; }
    std::optional<int> remainingLimit() const {
        if (!_limited) {
            return std::nullopt;
        }
        return _limit;
    }

private:
    int _amount;
    int _limit;
    bool _limited;
};

} // namespace

std::shared_ptr<resource::Gff> DamagePacket::saveContinuation() const {
    using G = resource::Gff; using F = G::Field;
    std::vector<std::shared_ptr<G>> components, resolution, feedback, baseAmounts;
    for (int amount : _baseDamageAmounts)
        baseAmounts.push_back(G::Builder().type(0).field(F::newInt("Amount", amount)).build());
    for (const auto &c : _components) components.push_back(G::Builder().type(0)
        .field(F::newInt("Amount", c.amount)).field(F::newInt("Type", static_cast<int>(c.type)))
        .field(F::newByte("Bonus", c.bonus)).build());
    auto g = G::Builder().type(0).field(F::newInt("Power", static_cast<int>(_power)))
        .field(F::newInt("Flags", _damageFlags)).field(F::newList("Components", std::move(components)))
        .field(F::newList("BaseAmounts", std::move(baseAmounts))).field(F::newByte("CutsDoors", _cutsDoors)).build();
    if (_resolution) {
        const auto &r = *_resolution;
        std::vector<std::shared_ptr<G>> amounts;
        for (int amount : r.damageAmounts)
            amounts.push_back(G::Builder().type(0).field(F::newInt("Amount", amount)).build());
        g->fields().push_back(F::newList("ResolvedAmounts", std::move(amounts)));
        const int values[] = {static_cast<int>(r.damageFlags), static_cast<int>(r.damagePower), static_cast<int>(r.rawDamage), static_cast<int>(r.plotSuppressed), static_cast<int>(r.immunityPercent), static_cast<int>(r.immunityPrevented), static_cast<int>(r.vulnerabilityAdded), static_cast<int>(r.damageAfterImmunity), static_cast<int>(r.resistanceAmount), static_cast<int>(r.resistancePrevented), static_cast<int>(r.resistanceFeatPrevented), static_cast<int>(r.percentageResistanceBonus), static_cast<int>(r.improvedToughnessBonus), static_cast<int>(r.wookieeEnduranceBonus), static_cast<int>(r.damageAfterResistance), static_cast<int>(r.reductionAmount), static_cast<int>(r.reductionPower), static_cast<int>(r.reductionBypassed), static_cast<int>(r.reductionPrevented), static_cast<int>(r.finalDamage)};
        for (int v : values) resolution.push_back(G::Builder().type(0).field(F::newInt("Value", v)).build());
        for (const auto &f : r.mitigationFeedback) {
            auto v = G::Builder().type(0).field(F::newInt("Type", static_cast<int>(f.type)))
                .field(F::newInt("Amount", f.amount)).field(F::newInt("Flags", f.damageFlags)).build();
            if (f.remaining) v->fields().push_back(F::newInt("Remaining", *f.remaining));
            feedback.push_back(std::move(v));
        }
        g->fields().push_back(F::newList("Resolution", std::move(resolution)));
        g->fields().push_back(F::newList("Feedback", std::move(feedback)));
        if (r.resistancePoolRemaining) g->fields().push_back(F::newInt("ResistPool", *r.resistancePoolRemaining));
        if (r.reductionPoolRemaining) g->fields().push_back(F::newInt("ReducePool", *r.reductionPoolRemaining));
    }
    return g;
}
DamagePacket DamagePacket::restoreContinuation(const resource::Gff &g) {
    DamagePacket result(static_cast<DamagePower>(g.getInt("Power")));
    result._damageFlags = g.getInt("Flags");
    result._cutsDoors = g.getBool("CutsDoors");
    const auto &baseAmounts = g.getList("BaseAmounts");
    for (size_t i = 0; i < result._baseDamageAmounts.size(); ++i)
        result._baseDamageAmounts[i] = baseAmounts.at(i)->getInt("Amount");
    for (const auto &v : g.getList("Components")) result._components.push_back({v->getInt("Amount"), static_cast<DamageType>(v->getInt("Type")), v->getBool("Bonus")});
    const auto &values = g.getList("Resolution");
    if (values.size() == 20) {
        auto &r = result._resolution.emplace();
        const auto &amounts = g.getList("ResolvedAmounts");
        for (size_t i = 0; i < r.damageAmounts.size(); ++i)
            r.damageAmounts[i] = amounts.at(i)->getInt("Amount");
        r.damageFlags = static_cast<int>(values[0]->getInt("Value"));
        r.damagePower = static_cast<DamagePower>(values[1]->getInt("Value"));
        r.rawDamage = static_cast<int>(values[2]->getInt("Value"));
        r.plotSuppressed = static_cast<bool>(values[3]->getInt("Value"));
        r.immunityPercent = static_cast<int>(values[4]->getInt("Value"));
        r.immunityPrevented = static_cast<int>(values[5]->getInt("Value"));
        r.vulnerabilityAdded = static_cast<int>(values[6]->getInt("Value"));
        r.damageAfterImmunity = static_cast<int>(values[7]->getInt("Value"));
        r.resistanceAmount = static_cast<int>(values[8]->getInt("Value"));
        r.resistancePrevented = static_cast<int>(values[9]->getInt("Value"));
        r.resistanceFeatPrevented = static_cast<int>(values[10]->getInt("Value"));
        r.percentageResistanceBonus = static_cast<int>(values[11]->getInt("Value"));
        r.improvedToughnessBonus = static_cast<int>(values[12]->getInt("Value"));
        r.wookieeEnduranceBonus = static_cast<int>(values[13]->getInt("Value"));
        r.damageAfterResistance = static_cast<int>(values[14]->getInt("Value"));
        r.reductionAmount = static_cast<int>(values[15]->getInt("Value"));
        r.reductionPower = static_cast<DamagePower>(values[16]->getInt("Value"));
        r.reductionBypassed = static_cast<bool>(values[17]->getInt("Value"));
        r.reductionPrevented = static_cast<int>(values[18]->getInt("Value"));
        r.finalDamage = static_cast<int>(values[19]->getInt("Value"));
        int value;
        if (g.readInt(value, "ResistPool")) r.resistancePoolRemaining = value;
        if (g.readInt(value, "ReducePool")) r.reductionPoolRemaining = value;
        for (const auto &v : g.getList("Feedback")) {
            MitigationFeedback f {static_cast<MitigationFeedbackType>(v->getInt("Type")), v->getInt("Amount"), std::nullopt, v->getInt("Flags")};
            if (v->readInt(value, "Remaining")) f.remaining = value;
            r.mitigationFeedback.push_back(std::move(f));
        }
    }
    return result;
}

static int applyDamageImmunity(
    const Object &object,
    DamageType damageType,
    int damage,
    DamageResolution &resolution, const Creature *versus = nullptr) {

    if (damage <= 0) {
        resolution.damageAfterImmunity = 0;
        return 0;
    }

    int immunity = object.damageImmunity(static_cast<int>(damageType));
    resolution.immunityPercent = immunity;

    int result;
    if (immunity > 0) {
        int prevented = std::max(1, damage * immunity / 100);
        result = std::max(0, damage - prevented);
        resolution.immunityPrevented = damage - result;
        resolution.mitigationFeedback.push_back({
            MitigationFeedbackType::DamageImmunity,
            resolution.immunityPrevented,
            std::nullopt,
            static_cast<int>(damageType),
        });
    } else {
        result = damage - damage * immunity / 100;
        resolution.vulnerabilityAdded = result - damage;
    }

    resolution.damageAfterImmunity = result;
    return result;
}

static std::optional<int> remainingPool(const EffectInstance *effect) {
    if (!effect || effect->integerParameter(2) <= 0) return std::nullopt;
    return effect->integerParameter(2);
}

static int consumePool(
    Object &object, uint64_t order, int damage, size_t amountParameter,
    std::optional<int> &remaining, bool simulate) {
    EffectInstance *effect = object.findEffectApplication(order);
    assert(effect && "Selected mitigation application is absent");
    DamageAbsorption absorption(effect->integerParameter(amountParameter),
                                effect->integerParameter(2));
    int prevented = absorption.absorb(damage);
    // A simulated hit leaves the pool and its effect as they are.
    if (simulate) return prevented;
    remaining = absorption.remainingLimit();
    if (remaining) {
        if (effect->integerParameters.size() < 3) effect->integerParameters.resize(3);
        effect->integerParameters[2] = *remaining;
    }
    const EffectId id = effect->id;
    if (absorption.exhausted()) object.removeEffectsById(id);
    return prevented;
}

static int applyDamageResistance(
    Object &object,
    DamageType damageType,
    int damage,
    DamageResolution &resolution, bool recordFeatReduction = true, const Creature *versus = nullptr,
    const Creature *source = nullptr, bool simulate = false) {

    int resistance = 0;

    bool secondaryQualifier = false;
    const EffectInstance *selectedEffect = nullptr;
    for (const EffectInstance &applied : object.effects()) {
        if (!applied.hasLiveRuntimeSource() || applied.type() != EffectType::DamageResistance ||
            (versus && !applied.appliesVersus(versus))) continue;
        if (damageTypeMatches(applied.integerParameter(3), static_cast<int>(damageType))) {
            secondaryQualifier = true;
        }
        if (!damageTypeMatches(applied.integerParameter(0), static_cast<int>(damageType)) ||
            applied.integerParameter(1) <= resistance) continue;
        resistance = applied.integerParameter(1);
        selectedEffect = &applied;
    }
    resolution.resistanceAmount = resistance;
    int consumptionFactor = secondaryQualifier ? 2 : 1;
    // For TSL, parameter 4 must equal one on the selected resistance effect,
    // and the source creature's current attack must be Power Attack to add this factor.
    if (object.game().isTSL() && selectedEffect && source &&
        selectedEffect->integerParameter(4) == 1) {
        switch (static_cast<FeatType>(source->currentCombatAttackType())) {
        case FeatType::PowerAttack:
        case FeatType::ImprovedPowerAttack:
        case FeatType::MasterPowerAttack:
            ++consumptionFactor;
            break;
        default:
            break;
        }
    }
    int consumptionDamage = damage * consumptionFactor;
    std::optional<int> preHitPool = remainingPool(selectedEffect);
    int effectiveResistance = resistance;
    if (preHitPool && *preHitPool <= consumptionDamage) effectiveResistance = *preHitPool;
    int reportedAmount = selectedEffect
        ? consumePool(object, selectedEffect->applicationOrder, consumptionDamage, 1,
                      resolution.resistancePoolRemaining, simulate)
        : std::min(consumptionDamage, resistance);

    // Query feat ownership after consuming or removing the selected resistance
    // effect, because removal can also retire linked bonus-feat children.
    int featBonus = 0;
    if (const auto *creature = dyn_cast<Creature>(&object)) {
        DamageResolution featReduction;
        creature->getDamageResistanceFeatBonuses(damage, featReduction);
        featBonus = featReduction.percentageResistanceBonus +
                    featReduction.improvedToughnessBonus +
                    featReduction.wookieeEnduranceBonus;
        if (recordFeatReduction) {
            resolution.percentageResistanceBonus = featReduction.percentageResistanceBonus;
            resolution.improvedToughnessBonus = featReduction.improvedToughnessBonus;
            resolution.wookieeEnduranceBonus = featReduction.wookieeEnduranceBonus;
        }
    }

    if (resistance > 0) {
        resolution.mitigationFeedback.push_back({
            preHitPool
                ? MitigationFeedbackType::FiniteDamageResistance
                : MitigationFeedbackType::DamageResistance,
            reportedAmount,
            preHitPool
                ? std::optional<int>(*preHitPool - reportedAmount)
                : std::nullopt,
            0,
        });
    }

    int prevented = std::min(std::max(damage, 0), effectiveResistance);
    resolution.resistancePrevented = prevented;
    int remaining = std::max(0, damage - prevented);
    int result = std::max(0, damage - prevented - featBonus);
    if (recordFeatReduction) resolution.resistanceFeatPrevented = remaining - result;
    resolution.damageAfterResistance = result;
    return result;
}

static int applyDamageReduction(
    Object &object,
    DamageType damageType,
    DamagePower damagePower,
    int damage,
    DamageResolution &resolution,
    bool simulate = false) {

    int reduction = 0;
    DamagePower requiredPower = DamagePower::Normal;
    const EffectInstance *selectedEffect = nullptr;
    for (const EffectInstance &applied : object.effects()) {
        if (!applied.hasLiveRuntimeSource() || applied.type() != EffectType::DamageReduction) continue;
        if (applied.integerParameter(0) <= reduction) continue;
        reduction = applied.integerParameter(0);
        requiredPower = static_cast<DamagePower>(applied.integerParameter(1));
        selectedEffect = &applied;
    }
    resolution.reductionAmount = reduction;
    resolution.reductionPower = requiredPower;
    std::optional<int> preHitPool = remainingPool(selectedEffect);
    int reportedAmount = selectedEffect
        ? consumePool(object, selectedEffect->applicationOrder, damage, 0,
                      resolution.reductionPoolRemaining, simulate)
        : std::min(damage, reduction);

    resolution.reductionBypassed =
        reduction > 0 &&
        static_cast<int>(damagePower) >= static_cast<int>(requiredPower);
    if (resolution.reductionBypassed) {
        return damage;
    }

    if (reduction > 0) {
        resolution.mitigationFeedback.push_back({
            preHitPool
                ? MitigationFeedbackType::FiniteDamageReduction
                : MitigationFeedbackType::DamageReduction,
            reportedAmount,
            preHitPool
                ? std::optional<int>(*preHitPool - reportedAmount)
                : std::nullopt,
            0,
        });
    }

    int result = std::max(0, damage - reportedAmount);
    resolution.reductionPrevented = damage - result;
    return result;
}

// A typed damage bonus meets the target's immunity and then its resistance.
static int applyDamageBonusMitigation(
    Object &object, DamageType damageType, int damage, DamageResolution &resolution,
    const Creature &source, bool simulate) {

    int amount = applyDamageImmunity(object, damageType, damage, resolution, &source);
    return applyDamageResistance(object, damageType, amount, resolution, false, &source, &source, simulate);
}

int simulateDamageBonusMitigation(Object &object, const Creature &source, int damageFlags, int damage) {
    DamageResolution resolution;
    return applyDamageBonusMitigation(object, static_cast<DamageType>(damageFlags), damage, resolution, source, true);
}

void DamagePacket::resolve(Object &object, const Creature *source) {
    requireUnresolved();

    DamageResolution result;
    result.damageFlags = _damageFlags;
    result.damagePower = _power;
    result.rawDamage = total();
    if (object.plotFlag()) {
        result.plotSuppressed = true;
        _resolution.emplace(std::move(result));
        return;
    }

    auto damageType = static_cast<DamageType>(_damageFlags);
    int amount = applyDamageImmunity(
        object,
        damageType,
        result.rawDamage,
        result);
    amount = applyDamageResistance(
        object,
        damageType,
        amount,
        result, true, nullptr, source);
    // Only physical script damage is reduced; a weapon hit is reduced whatever its type.
    if (damageTypeMatches(kPhysicalDamageTypeFlags, _damageFlags))
        amount = applyDamageReduction(object, damageType, _power, amount, result);
    result.finalDamage = amount;
    _resolution.emplace(std::move(result));
}

void DamagePacket::resolvePhysical(Object &object, const Creature &source, bool simulate) {
    requireUnresolved();
    DamageResolution result;
    result.damageFlags = _damageFlags;
    result.damagePower = _power;
    result.rawDamage = total();
    std::copy(_baseDamageAmounts.begin(), _baseDamageAmounts.end(), result.damageAmounts.begin());
    // A plot object takes no damage; measuring a hit against one still counts
    // its mitigation.
    if (!simulate && object.plotFlag()) {
        result.plotSuppressed = true;
        for (int &amount : result.damageAmounts) if (amount >= 0) amount = 0;
        result.damageAmounts[14] = 0;
        _resolution.emplace(std::move(result));
        return;
    }
    // Update the signed-word typed slot and base accumulator independently. Negative bonuses
    // are not mitigated.
    auto addTyped = [&](int slot, int amount) {
        int current = narrowAttackDamage(result.damageAmounts[slot]);
        result.damageAmounts[slot] = narrowAttackDamage(current > 0
            ? std::max(1, current + amount) : std::max(0, amount));
        current = narrowAttackDamage(result.damageAmounts[14]);
        result.damageAmounts[14] = narrowAttackDamage((current > 0 ? current : 0) + amount);
    };
    for (int slot = 0; slot < 15; ++slot) {
        const auto type = static_cast<DamageType>(1 << slot);
        for (const auto &component : _components) {
            if (!component.bonus || component.type != type || component.amount <= 0) continue;
            DamageResolution bonus;
            int amount = applyDamageBonusMitigation(object, type, component.amount, bonus, source, simulate);
            for (const auto &entry : bonus.mitigationFeedback) result.mitigationFeedback.push_back(entry);
            addTyped(slot, amount);
        }
        for (const auto &component : _components) {
            if (component.bonus && component.type == type && component.amount < 0) addTyped(slot, component.amount);
        }
    }
    // Resolve base damage after typed bonuses. Both stages apply resistance feats;
    // the base stage records their breakdown and also applies damage reduction.
    int base = 0;
    for (const auto &component : _components) if (!component.bonus) base += component.amount;
    const auto type = static_cast<DamageType>(_damageFlags);
    base = narrowAttackDamage(base);
    if (base <= 0) {
        // Record the minimum typed amount before base mitigation, without adding it to the
        // aggregate twice.
        const auto slot = getDirectDamageSlot(static_cast<int>(getPrimaryDamageType(_damageFlags)));
        const int current = narrowAttackDamage(result.damageAmounts[slot]);
        result.damageAmounts[slot] = narrowAttackDamage(current > 0 ? current + 1 : 1);
        base = 1;
    }
    base = applyDamageImmunity(object, type, base, result, &source);
    base = applyDamageResistance(object, type, narrowAttackDamage(base), result, true, &source, &source, simulate);
    base = applyDamageReduction(object, type, _power, narrowAttackDamage(base), result, simulate);
    // A door-cutting weapon takes a door or placeable's whole vitality.
    if (_cutsDoors && (isa<Door>(&object) || isa<Placeable>(&object))) base = object.maxHitPoints();
    base = std::max(0, narrowAttackDamage(base));
    const int accumulated = narrowAttackDamage(result.damageAmounts[14]);
    result.finalDamage = narrowAttackDamage(base + (accumulated > 0 ? accumulated : 0));
    result.damageAmounts[14] = result.finalDamage;
    _resolution.emplace(std::move(result));
}

void DamagePacket::resolveReturned(Creature &shooter) {
    requireUnresolved();
    DamageResolution result;
    result.damageFlags = _damageFlags;
    result.damagePower = _power;
    result.rawDamage = total();
    std::copy(_baseDamageAmounts.begin(), _baseDamageAmounts.end(), result.damageAmounts.begin());
    for (const auto &component : _components) {
        if (!component.bonus || component.amount <= 0) continue;
        const auto slot = getDirectDamageSlot(static_cast<int>(component.type));
        if (slot >= 14) continue;
        const int current = narrowAttackDamage(result.damageAmounts[slot]);
        result.damageAmounts[slot] = narrowAttackDamage((current > 0 ? current : 0) + component.amount);
    }
    const auto slot = getScriptDamageSlot(_damageFlags);
    if (shooter.plotFlag()) {
        result.plotSuppressed = true;
        for (int &amount : result.damageAmounts) if (amount >= 0) amount = 0;
        result.damageAmounts[slot] = 0;
        result.damageAmounts[14] = 0;
        _resolution.emplace(std::move(result));
        return;
    }
    const auto type = static_cast<DamageType>(_damageFlags);
    int amount = narrowAttackDamage(result.rawDamage);
    amount = applyDamageResistance(shooter, type, amount, result, true, &shooter, &shooter);
    amount = applyDamageImmunity(shooter, type, narrowAttackDamage(amount), result, &shooter);
    result.finalDamage = narrowAttackDamage(amount);
    if (slot < 14) result.damageAmounts[slot] = result.finalDamage;
    result.damageAmounts[14] = result.finalDamage;
    _resolution.emplace(std::move(result));
}

std::shared_ptr<DamageEffect> DamageEffect::fromShieldRetaliation(int amount, int shieldFlags) {
    EffectInstance payload;
    payload.integerParameters.assign(21, 0);
    for (size_t i = 0; i < 15; ++i) payload.setIntegerParameter(i, -1);
    // A shield of no type fills no slot.
    if (shieldFlags > 0) {
        const auto slot = getScriptDamageSlot(shieldFlags);
        if (slot < 15) payload.setIntegerParameter(slot, amount);
    }
    payload.setIntegerParameter(kReactionDelay, 1000);
    return std::make_shared<DamageEffect>(payload);
}

std::shared_ptr<DamageEffect> DamageEffect::fromResolvedAttackAmount(int amount, size_t slot) {
    EffectInstance payload;
    payload.integerParameters.assign(22, 0);
    for (size_t i = 0; i < 15; ++i) payload.setIntegerParameter(i, -1);
    payload.setIntegerParameter(slot, amount);
    payload.setIntegerParameter(kTotal, amount);
    payload.setIntegerParameter(kReactionDelay, 0);
    // These post-roll producers write Log_Base2(damage flag), i.e. the slot.
    payload.setIntegerParameter(kDamageFlags, static_cast<int>(slot));
    payload.setIntegerParameter(kPreResolved, 1);
    payload.setIntegerParameter(kSuppressShields, 1);
    payload.setIntegerParameter(kFeedbackHandled, 0);
    return std::make_shared<DamageEffect>(payload);
}

void showMitigationFeedback(Object &object, const std::shared_ptr<Creature> &source,
                            const DamageResolution &resolution) {
    if (resolution.mitigationFeedback.empty()) return;
    std::vector<DeferredCombatFeedback> records(
        resolution.mitigationFeedback.begin(), resolution.mitigationFeedback.end());
    showCombatFeedback(object.game(), object.services(), source, object, records);
}

EffectApplicationResult DamageEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring || object.isDead()) return EffectApplicationResult::Applied;
    if (auto *creature = dyn_cast<Creature>(&object); creature &&
        object.game().isTSL() && creature->isPC() && creature->currentHitPoints() <= 0)
        return EffectApplicationResult::Applied;
    // Normalize a per-application copy before reading its total. The stored
    // descriptor remains reusable; already-resolved hits do not repeat mitigation.
    auto application = resolveDamageEffect(instance, object.plotFlag(), [&](int raw, int flags) {
        DamagePacket packet(DamagePower::Normal);
        packet.add(raw, static_cast<DamageType>(flags));
        packet.setDamageFlags(flags);
        const auto source = std::dynamic_pointer_cast<Creature>(instance.boundCreator());
        packet.resolve(object, source.get());
        // Damage other than a weapon hit reports its mitigation at once, to
        // the damaged object and the creature that caused it.
        showMitigationFeedback(object, source, packet.resolution());
        return packet.resolvedDamage();
    });
    if (!application) return EffectApplicationResult::Applied;

    // Damage other than a weapon hit makes the target react: electrical or dark
    // side damage jolts it for the effect's reaction time, any other type makes
    // it flinch.
    auto &damageAmounts = application->damageAmounts;
    std::optional<DamageReaction> reaction;
    if (!application->preResolved && application->amount > 0)
        reaction = DamageReaction {
            damageAmounts[getDirectDamageSlot(static_cast<int>(DamageType::Electrical))] > 0 ||
                damageAmounts[getDirectDamageSlot(static_cast<int>(DamageType::DarkSide))] > 0,
            instance.integerParameter(kReactionDelay)};
    int amount = object.game().scaleDamageForDifficulty(application->amount, object);
    if (application->preResolved && damageAmounts.back() >= 0) damageAmounts.back() = amount;
    if (!application->preResolved && amount == 0) {
        for (int &value : damageAmounts) if (value >= 0) value = 0;
    }

    const auto creator = instance.boundCreator();
    // Nested shield callbacks must see this hit, not the preceding hit. Do not
    // republish afterward: a nested hit may have legitimately replaced it.
    object.setLastDamager(creator);
    object.setLastDamageAmounts(damageAmounts);
    debug(str(boost::format("Damage taken: %s %d") % object.tag() % amount));
    if (application->preResolved &&
        !application->suppressDamageShields) {
        auto attacker = std::dynamic_pointer_cast<Creature>(instance.boundCreator());
        auto *defender = dyn_cast<Creature>(&object);
        if (defender && attacker) defender->resolveDamageShields(*attacker);
    }
    if (amount > 0) {
        if (auto *creature = dyn_cast<Creature>(&object)) creature->removeMindTrickEffects();
    }
    if (object.isRuntimeLive()) object.applyDamageEffect(amount, creator, reaction);
    return EffectApplicationResult::Applied;
}

void DamagePacket::resolveUnmitigated() {
    requireUnresolved();
    DamageResolution result;
    result.damageFlags = _damageFlags;
    result.damagePower = _power;
    result.rawDamage = total();
    std::copy(_baseDamageAmounts.begin(), _baseDamageAmounts.end(), result.damageAmounts.begin());
    result.finalDamage = narrowAttackDamage(result.rawDamage);
    result.damageAmounts[14] = result.finalDamage;
    _resolution.emplace(std::move(result));
}

void DamagePacket::resolveLightsaberThrow(Object &object, const Creature &caster) {
    requireUnresolved();
    DamageResolution result;
    result.damageFlags = _damageFlags;
    result.damagePower = _power;
    result.rawDamage = total();
    auto type = static_cast<DamageType>(_damageFlags);
    int amount = result.rawDamage;
    if (dyn_cast<Creature>(&object)) {
        amount = applyDamageResistance(object, type, amount, result, true, &caster, &caster);
        amount = applyDamageImmunity(object, type, amount, result, &caster);
    }
    if (object.plotFlag()) { result.plotSuppressed = true; amount = 0; }
    result.finalDamage = amount;
    _resolution.emplace(std::move(result));
}

DamageType getPrimaryDamageType(int damageFlags) {
    assert(damageFlags > 0);

    int type = 1;
    while (damageFlags > 1) {
        damageFlags >>= 1;
        type <<= 1;
    }
    return static_cast<DamageType>(type);
}

bool damageTypeMatches(int modifierFlags, int damageFlags) {
    // Damage masks are bit sets: ALL excludes BASE_DAMAGE, PHYSICAL includes
    // it, and the old 0x2007 value must not become a wildcard for other types.
    return (modifierFlags & damageFlags) != 0;
}

void DamagePacket::requireUnresolved() const {
    assert(!isResolved() && "Damage packet has already been resolved");
}

void DamagePacket::addResolved(int amount, DamageType type, bool bonus) {
    for (Component &component : _components) {
        if (component.type == type && component.bonus == bonus) {
            component.amount = std::max(component.amount + amount, 1);
            return;
        }
    }
    if (amount > 0) {
        _components.push_back({amount, type, bonus});
    }
}

void DamagePacket::add(int amount, DamageType type) {
    requireUnresolved();
    if (amount == 0) {
        return;
    }

    addResolved(amount, type);
}

void DamagePacket::addPhysicalBase(int amount, DamageType type) {
    requireUnresolved();
    _components.push_back({amount, type, false});
}

void DamagePacket::addBonus(int amount, DamageType type) {
    requireUnresolved();
    if (amount == 0) return;
    // The effect-bonus reader retains increases and decreases independently;
    // a decrease is applied after the corresponding positive typed stage.
    for (auto &component : _components) {
        if (component.bonus && component.type == type && (component.amount < 0) == (amount < 0)) {
            component.amount += amount;
            return;
        }
    }
    _components.push_back({amount, type, true});
}

void DamagePacket::setDamageFlags(int damageFlags) {
    requireUnresolved();
    _damageFlags = damageFlags;
}

void DamagePacket::setPower(DamagePower power) {
    requireUnresolved();
    if (static_cast<int>(power) > static_cast<int>(_power)) {
        _power = power;
    }
}

const DamageResolution &DamagePacket::resolution() const {
    assert(_resolution && "Damage packet has not been resolved");
    return *_resolution;
}

int DamagePacket::resolvedDamage() const {
    return resolution().finalDamage;
}

int DamagePacket::total() const {
    int result = 0;
    for (const Component &component : _components) {
        result += component.amount;
    }
    return result;
}

EffectApplicationResult DeathEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    if (auto *creature = dyn_cast<Creature>(&object))
        return creature->applyDeathEffect(instance.boundCreator(), instance.integerParameter(2) != 0, &instance)
            ? EffectApplicationResult::Applied : EffectApplicationResult::Rejected;
    auto door = dyn_cast<Door>(&object);
    auto placeable = dyn_cast<Placeable>(&object);
    if (!door && !placeable) return EffectApplicationResult::Rejected;
    if (object.plotFlag()) return EffectApplicationResult::Applied;
    if (object.isMinOneHP()) {
        object.setCurrentHitPoints(1);
        return EffectApplicationResult::Applied;
    }
    object.clearAllActions(true);
    if (placeable) placeable->enterDestroyedState();
    if (door) {
        door->enterDestroyedState();
        // Link kinds 1 and 2 resolve the linked object in the loaded module.
        if ((door->linkedToFlags() == 1 || door->linkedToFlags() == 2) && object.spatialArea()) {
            auto linked = object.spatialArea()->getObjectByTag(door->linkedTo());
            auto linkedDoor = std::dynamic_pointer_cast<Door>(linked);
            if (linkedDoor && linkedDoor->state() != DoorState::Destroyed) {
                auto child = instance;
                child.effect = instance.effect->cloneEffect();
                child.setIntegerParameter(0, 0);
                child.setIntegerParameter(1, 1);
                linkedDoor->applyEffect(std::move(child));
            }
        }
    }
    // Destruction belongs to the existing module queue, not the presentation.
    auto module = object.game().module();
    const auto when = object.game().worldTimeMilliseconds();
    const auto dayLength = object.game().millisecondsPerWorldDay();
    SavedEventRecord signal;
    signal.day = static_cast<uint32_t>(when / dayLength);
    signal.time = static_cast<uint32_t>(when % dayLength);
    signal.object = SavedObjectReference::fromRuntimeId(object.id());
    signal.caller = SavedObjectReference::fromRuntimeId(instance.creatorId);
    signal.eventId = static_cast<uint32_t>(SavedEventType::SignalEvent);
    SavedScriptEvent deathEvent;
    deathEvent.type = 10;
    signal.payload = std::move(deathEvent);
    module->enqueueSaveEvent(std::move(signal));
    SavedEventRecord destroy;
    destroy.day = static_cast<uint32_t>((when + 2000) / dayLength);
    destroy.time = static_cast<uint32_t>((when + 2000) % dayLength);
    destroy.object = SavedObjectReference::fromRuntimeId(object.id());
    destroy.caller = SavedObjectReference::fromRuntimeId(instance.creatorId);
    destroy.eventId = static_cast<uint32_t>(SavedEventType::DestroyObject);
    module->enqueueSaveEvent(std::move(destroy));
    return EffectApplicationResult::Applied;
}

EffectApplicationResult HitPointChangeWhenDyingEffect::onApply(Object &object, EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature || !creature->isPC()) return EffectApplicationResult::Rejected;
    const float rate = instance.floatParameters[0];
    if (instance.durationType() == DurationType::Instant && rate >= 0.1f) {
        auto effect = std::make_shared<HitPointChangeWhenDyingEffect>(rate);
        auto child = effect->saveFacingInstance();
        child.effect = effect;
        child.setDuration(DurationType::Temporary, std::fabs(6.0f / rate));
        child.creatorId = instance.creatorId;
        child.creator = instance.creator;
        if (auto creator = instance.boundCreator()) child.spellId = creator->effectSpellId();
        child.restoring = instance.restoring;
        object.applyEffect(std::move(child));
    }
    return EffectApplicationResult::Retained;
}

EffectRemovalResult HitPointChangeWhenDyingEffect::onRemove(Object &object, const EffectInstance &instance) {
    auto *creature = dyn_cast<Creature>(&object);
    // Remove malformed loaded non-creature records safely after a failed cast.
    if (!creature) return EffectRemovalResult::Removed;
    if (creature->isPC()) {
        const float rate = instance.floatParameters[0];
        const int amount = creature->currentHitPointsWithoutTemporary() + (rate > 0.0f ? 1 : -1);
        creature->Object::setCurrentHitPoints(amount);
        if (!(rate > 0.0f)) {
            auto appearance = object.services().resource.twoDas.get("appearance");
            const auto blood = appearance ? appearance->getString(creature->appearance(), "bloodcolr") : "";
            const int visualId = blood == "R" ? 158 : blood == "G" ? 159 : blood == "Y" ? 160 : 0;
            auto effect = std::make_shared<VisualEffectMarkerEffect>(visualId);
            auto child = effect->saveFacingInstance();
            child.effect = effect;
            child.setDuration(DurationType::Instant, 0.0f);
            object.applyEffect(std::move(child));
        }
    }
    if (creature->isDead() ||
        (object.game().party().isMember(object) && creature->currentHitPoints() <= 0)) {
        auto effect = std::make_shared<DeathEffect>(false, true, false);
        auto child = effect->saveFacingInstance();
        child.effect = effect;
        child.setDuration(DurationType::Instant, 0.0f);
        child.creatorId = instance.creatorId;
        child.creator = instance.creator;
        if (auto creator = instance.boundCreator()) child.spellId = creator->effectSpellId();
        object.applyEffect(std::move(child));
    }
    return EffectRemovalResult::Removed;
}

EffectApplicationResult HealEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    if (instance.integerParameter(1) == 54) {
        if (auto *creature = dyn_cast<Creature>(&object))
            creature->regenerateForcePoints(instance.integerParameter(0));
    } else if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->applyHealingEffect(instance.integerParameter(0), instance.boundCreator(),
                                     instance.integerParameter(2) != 0);
    }
    return EffectApplicationResult::Applied;
}

EffectApplicationResult ResurrectionEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    auto *creature = dyn_cast<Creature>(&object);
    return creature && creature->applyResurrectionEffect(instance.integerParameter(0))
        ? EffectApplicationResult::Applied : EffectApplicationResult::Rejected;
}

EffectApplicationResult DamageForcePointsEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Rejected;
    creature->damageForcePoints(instance.integerParameter(0));
    return EffectApplicationResult::Applied;
}

EffectApplicationResult ForceDrainEffect::onApply(Object &object, EffectInstance &instance) {
    if (auto *creature = dyn_cast<Creature>(&object)) {
        creature->damageForcePoints(instance.integerParameter(0));
    }
    return EffectApplicationResult::Applied;
}

EffectApplicationResult HealForcePointsEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring) return EffectApplicationResult::Applied;
    auto *creature = dyn_cast<Creature>(&object);
    if (!creature) return EffectApplicationResult::Rejected;
    creature->healForcePoints(instance.integerParameter(0));
    return EffectApplicationResult::Applied;
}

} // namespace game

} // namespace reone
