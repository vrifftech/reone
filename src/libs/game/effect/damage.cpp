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
#include "reone/game/effect/damageabsorption.h"
#include "reone/game/object.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace reone {

namespace game {

std::shared_ptr<resource::Gff> DamagePacket::saveContinuation() const {
    using G = resource::Gff; using F = G::Field;
    std::vector<std::shared_ptr<G>> components, resolution, feedback;
    for (const auto &c : _components) components.push_back(G::Builder().type(0)
        .field(F::newInt("Amount", c.amount)).field(F::newInt("Type", static_cast<int>(c.type))).build());
    auto g = G::Builder().type(0).field(F::newInt("Power", static_cast<int>(_power)))
        .field(F::newInt("Flags", _damageFlags)).field(F::newList("Components", std::move(components))).build();
    if (_resolution) {
        const auto &r = *_resolution;
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
    for (const auto &v : g.getList("Components")) result._components.push_back({v->getInt("Amount"), static_cast<DamageType>(v->getInt("Type"))});
    const auto &values = g.getList("Resolution");
    if (values.size() == 20) {
        auto &r = result._resolution.emplace();
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

static constexpr int kDamageTypeCount = 15;

static int applyDamageImmunityDelta(int current, int delta) {
    return std::clamp(current + delta, -100, 100);
}

static int getDamageImmunity(const Object &object, DamageType damageType, const Creature *versus = nullptr) {
    int damageFlags = static_cast<int>(damageType);
    int result = 0;
    for (int bit = 0; bit < kDamageTypeCount; ++bit) {
        int typeFlag = 1 << bit;
        if ((damageFlags & typeFlag) == 0) continue;
        int immunity = 0;
        std::vector<const EffectInstance *> modifiers;
        for (const EffectInstance &applied : object.effects()) {
            if (!applied.hasLiveRuntimeSource() ||
                (versus && !applied.appliesVersus(versus)) ||
                (applied.type() != EffectType::DamageImmunityIncrease &&
                 applied.type() != EffectType::DamageImmunityDecrease) ||
                !damageTypeMatches(applied.integerParameter(0), typeFlag)) continue;
            if (applied.durationType() == DurationType::Equipped) {
                // Item properties are already materialized as effects on the target.
                // Sum those contributions before applying the single clamp.
                const int amount = applied.integerParameter(1);
                immunity += applied.type() == EffectType::DamageImmunityDecrease
                                ? -amount : amount;
            } else {
                modifiers.push_back(&applied);
            }
        }
        std::stable_sort(modifiers.begin(), modifiers.end(),
            [](const EffectInstance *left, const EffectInstance *right) {
                return left->applicationOrder < right->applicationOrder;
            });
        immunity = std::clamp(immunity, -100, 100);
        for (const EffectInstance *applied : modifiers) {
            int delta = applied->integerParameter(1);
            if (applied->type() == EffectType::DamageImmunityDecrease) delta = -delta;
            immunity = applyDamageImmunityDelta(immunity, delta);
        }
        if (result == 0 || immunity < result) result = immunity;
    }
    return std::clamp(result, -100, 100);
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

    int immunity = getDamageImmunity(object, damageType, versus);
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
    std::optional<int> &remaining) {
    EffectInstance *effect = object.findEffectApplication(order);
    assert(effect && "Selected mitigation application is absent");
    DamageAbsorption absorption(effect->integerParameter(amountParameter),
                                effect->integerParameter(2));
    int prevented = absorption.absorb(damage);
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
    DamageResolution &resolution, bool includeFeats = true, const Creature *versus = nullptr) {

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
    int consumptionDamage = damage * (secondaryQualifier ? 2 : 1);
    std::optional<int> preHitPool = remainingPool(selectedEffect);
    int effectiveResistance = resistance;
    if (preHitPool && *preHitPool <= consumptionDamage) effectiveResistance = *preHitPool;
    int reportedAmount = selectedEffect
        ? consumePool(object, selectedEffect->applicationOrder, consumptionDamage, 1,
                      resolution.resistancePoolRemaining)
        : std::min(consumptionDamage, resistance);

    // Query feat ownership after consuming or removing the selected resistance
    // effect, because removal can also retire linked bonus-feat children.
    int featBonus = 0;
    if (const auto *creature = dyn_cast<Creature>(&object); creature && includeFeats) {
        creature->getDamageResistanceFeatBonuses(damage, resolution);
        featBonus = resolution.percentageResistanceBonus +
                    resolution.improvedToughnessBonus +
                    resolution.wookieeEnduranceBonus;
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
    resolution.resistanceFeatPrevented = remaining - result;
    resolution.damageAfterResistance = result;
    return result;
}

static int applyDamageReduction(
    Object &object,
    DamageType damageType,
    DamagePower damagePower,
    int damage,
    DamageResolution &resolution) {

    if (!damageTypeMatches(
            kPhysicalDamageTypeFlags,
            static_cast<int>(damageType))) {
        return damage;
    }

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
                      resolution.reductionPoolRemaining)
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

void DamagePacket::resolve(Object &object) {
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
        result);
    amount = applyDamageReduction(
        object,
        damageType,
        _power,
        amount,
        result);
    result.finalDamage = amount;
    _resolution.emplace(std::move(result));
}

EffectApplicationResult DamageEffect::onApply(Object &object, EffectInstance &instance) {
    if (instance.restoring || object.isDead()) return EffectApplicationResult::Applied;
    if (auto *creature = dyn_cast<Creature>(&object); creature &&
        object.game().isTSL() && creature->isPC() && creature->currentHitPoints() <= 0)
        return EffectApplicationResult::Applied;
    // A physical subattack is already resolved for this impact. Script and
    // queued damage instead resolve a fresh local packet for every application.
    const bool preResolved = instance.integerParameter(kPreResolved) != 0;
    if (instance.integerParameter(kTotal) == 0 && !preResolved && !object.plotFlag())
        return EffectApplicationResult::Applied;

    auto application = resolveDamageEffect(instance, [&](int raw, int flags, DamagePower power) {
        DamagePacket packet(power);
        packet.add(raw, static_cast<DamageType>(flags));
        packet.setDamageFlags(flags);
        packet.resolve(object);
        return packet.resolvedDamage();
    });
    int amount = object.game().scaleDamageForDifficulty(application.amount, object);
    auto &damageAmounts = application.damageAmounts;
    if (application.preResolved && damageAmounts.back() >= 0) damageAmounts.back() = amount;
    if (object.plotFlag() || (!application.preResolved && amount == 0)) {
        for (int &value : damageAmounts) if (value >= 0) value = 0;
    }

    const auto creator = instance.boundCreator();
    // Nested shield callbacks must see this hit, not the preceding hit. Do not
    // republish afterward: a nested hit may have legitimately replaced it.
    object.setLastDamager(creator);
    object.setLastDamageAmounts(damageAmounts);
    debug(str(boost::format("Damage taken: %s %d") % object.tag() % amount));
    if (application.preResolved &&
        !application.suppressDamageShields) {
        auto attacker = std::dynamic_pointer_cast<Creature>(instance.boundCreator());
        auto *defender = dyn_cast<Creature>(&object);
        if (defender && attacker) defender->resolveDamageShields(*attacker);
    }
    if (amount > 0) {
        if (auto *creature = dyn_cast<Creature>(&object)) creature->removeMindTrickEffects();
    }
    if (object.isRuntimeLive()) object.applyDamageEffect(amount, creator);
    return EffectApplicationResult::Applied;
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
        amount = applyDamageResistance(object, type, amount, result, false, &caster);
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

void DamagePacket::addResolved(int amount, DamageType type) {
    for (Component &component : _components) {
        if (component.type == type) {
            component.amount = std::max(component.amount + amount, 1);
            return;
        }
    }
    if (amount > 0) {
        _components.push_back({amount, type});
    }
}

void DamagePacket::add(int amount, DamageType type) {
    requireUnresolved();
    if (amount == 0) {
        return;
    }

    addResolved(amount, type);
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

} // namespace game

} // namespace reone
