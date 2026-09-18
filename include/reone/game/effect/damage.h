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

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "reone/system/smallvector.h"

#include "../effect.h"
#include "../damageslotrules.h"

namespace reone {

namespace game {

inline constexpr int kAllDamageTypeFlags = 0x3fff;
inline constexpr int kPhysicalDamageTypeFlags = 0x4007;

DamageType getPrimaryDamageType(int damageFlags);
bool damageTypeMatches(int modifierFlags, int damageFlags);

/**
 * Stable reone-owned identifiers for mitigation-feedback payloads.
 *
 * These IDs are independent of external feedback-message identifiers.
 */
enum class MitigationFeedbackType {
    DamageImmunity = 0,
    DamageResistance = 1,
    DamageReduction = 2,
    FiniteDamageResistance = 3,
    FiniteDamageReduction = 4,
};

/**
 * Semantic payload consumed by reone's mitigation-feedback presentation.
 *
 * Every type carries an amount. Finite resistance and reduction also carry
 * the remaining pool, while immunity uses damageFlags to select its label.
 */
struct MitigationFeedback {
    MitigationFeedbackType type;
    int amount {0};
    std::optional<int> remaining;
    int damageFlags {0};
};

/**
 * Values produced while resolving one damage packet against its target.
 *
 * This is an observational record of the existing mitigation calculation.
 * It retains both post-mutation pool state and producer-time feedback values.
 */
struct DamageResolution {
    int damageFlags {0};
    DamagePower damagePower {DamagePower::Normal};
    int rawDamage {0};
    bool plotSuppressed {false};

    int immunityPercent {0};
    int immunityPrevented {0};
    int vulnerabilityAdded {0};
    int damageAfterImmunity {0};

    int resistanceAmount {0};
    int resistancePrevented {0};
    int resistanceFeatPrevented {0};
    int percentageResistanceBonus {0};
    int improvedToughnessBonus {0};
    int wookieeEnduranceBonus {0};
    std::optional<int> resistancePoolRemaining;
    int damageAfterResistance {0};

    int reductionAmount {0};
    DamagePower reductionPower {DamagePower::Normal};
    bool reductionBypassed {false};
    int reductionPrevented {0};
    std::optional<int> reductionPoolRemaining;

    SmallVector<MitigationFeedback, 3> mitigationFeedback;
    int finalDamage {0};
};

/**
 * Typed damage caused by one hit.
 *
 * A packet may contain multiple damage types, but it is applied to the target
 * as one damage event.
 */
class DamagePacket {
public:
    explicit DamagePacket(DamagePower power = DamagePower::Normal) :
        _power(power) {
    }

    void add(int amount, DamageType type);
    void setDamageFlags(int damageFlags);
    void setPower(DamagePower power);
    void resolve(Object &object);
    void resolveLightsaberThrow(Object &object, const Creature &caster);

    int total() const;
    int damageFlags() const { return _damageFlags; }
    DamagePower power() const { return _power; }
    std::shared_ptr<resource::Gff> saveContinuation() const;
    static DamagePacket restoreContinuation(const resource::Gff &record);
    int resolvedDamage() const;
    const DamageResolution &resolution() const;
    bool empty() const { return _components.empty(); }
    bool isResolved() const { return _resolution.has_value(); }

private:
    struct Component {
        int amount;
        DamageType type;
    };

    void requireUnresolved() const;
    void addResolved(int amount, DamageType type);

    DamagePower _power;
    int _damageFlags {0};
    SmallVector<Component, 4> _components;
    std::optional<DamageResolution> _resolution;
};

/** Event slots: an absent atomic slot is -1; composite queries are zero. */
inline int getDamageAmountByType(const std::array<int, 15> &amounts, int flags) {
    if (flags <= 0 || (flags & (flags - 1)) != 0) return 0;
    size_t slot = 0;
    for (; flags > 1; flags >>= 1) ++slot;
    return slot < amounts.size() ? amounts[slot] : 0;
}

inline int getTotalDamageAmounts(const std::array<int, 15> &amounts) {
    int result = 0;
    for (int amount : amounts) {
        if (amount > 0) result += amount;
    }
    return result;
}

class DamageEffect : public CopyableEffect<DamageEffect> {
public:
    struct ApplicationContext {
        static constexpr int kAbsentDamageAmount = -1;

        ApplicationContext() {
            // DamageType::Universal uses slot 3; -1 marks an unpopulated slot.
            damageAmounts.fill(kAbsentDamageAmount);
        }

        std::array<int, 15> damageAmounts;
        bool suppressDamageShields {false};
        bool feedbackHandled {false};
    };

    // Damage payload: fifteen event slots, reaction metadata and flags.
    static constexpr size_t kTotal = 14;
    static constexpr size_t kReactionDelay = 16;
    static constexpr size_t kDamageFlags = 17;
    static constexpr size_t kPower = 18;
    static constexpr size_t kPreResolved = 19;
    static constexpr size_t kSuppressShields = 20;
    static constexpr size_t kFeedbackHandled = 21;

    // Typed damage already represented as a packet component keeps its direct slot mapping.
    DamageEffect(int amount, DamageType type, DamagePower power,
                 bool feedbackHandled = false) :
        DamageEffect(amount, static_cast<int>(type), power,
                     getDirectDamageSlot(static_cast<int>(type)), feedbackHandled) {}

    // NWScript accepts normalized raw flags, including composite values.
    // Use the shared script flag-to-slot conversion.
    DamageEffect(int amount, int damageFlags, DamagePower power) :
        DamageEffect(amount, damageFlags, power,
                     getScriptDamageSlot(damageFlags), false) {}

    DamageEffect(DamagePacket damage, ApplicationContext context) :
        CopyableEffect(EffectType::Damage) {
        assert(damage.isResolved() && "Damage packet has not been resolved");
        for (size_t i = 0; i < context.damageAmounts.size(); ++i)
            setSaveFacingInteger(i, context.damageAmounts[i]);
        const auto &resolution = damage.resolution();
        setSaveFacingInteger(kTotal, resolution.finalDamage);
        setSaveFacingInteger(kReactionDelay, 1000);
        setSaveFacingInteger(kDamageFlags, resolution.damageFlags);
        setSaveFacingInteger(kPower, static_cast<int>(resolution.damagePower));
        setSaveFacingInteger(kPreResolved, 1);
        setSaveFacingInteger(kSuppressShields, context.suppressDamageShields);
        if (context.feedbackHandled) setSaveFacingInteger(kFeedbackHandled, 1);
    }

    explicit DamageEffect(DamagePacket damage) :
        DamageEffect(std::move(damage), ApplicationContext()) {}

    explicit DamageEffect(const EffectInstance &instance) : CopyableEffect(EffectType::Damage) {
        for (size_t i = 0; i < instance.integerParameters.size(); ++i)
            setSaveFacingInteger(i, instance.integerParameters[i]);
    }

    EffectApplicationResult onApply(Object &object, EffectInstance &instance) override;

private:
    DamageEffect(int amount, int flags, DamagePower power,
                 size_t slot, bool feedbackHandled) :
        CopyableEffect(EffectType::Damage) {
        for (size_t i = 0; i < 15; ++i) setSaveFacingInteger(i, -1);
        setSaveFacingInteger(slot, amount);
        setSaveFacingInteger(kTotal, amount);
        setSaveFacingInteger(kReactionDelay, 1000);
        setSaveFacingInteger(kDamageFlags, flags);
        setSaveFacingInteger(kPower, static_cast<int>(power));
        setSaveFacingInteger(kPreResolved, 0);
        setSaveFacingInteger(kSuppressShields, 0);
        if (feedbackHandled) setSaveFacingInteger(kFeedbackHandled, 1);
    }

};

/** An application result, never stored back into a reusable effect descriptor. */
struct DamageEffectApplication {
    int amount {0};
    std::array<int, 15> damageAmounts {};
    bool preResolved {false};
    bool suppressDamageShields {false};
};

template <typename Resolve>
DamageEffectApplication resolveDamageEffect(const EffectInstance &instance, Resolve &&resolve) {
    DamageEffectApplication result;
    for (size_t i = 0; i < result.damageAmounts.size(); ++i)
        result.damageAmounts[i] = instance.integerParameter(i);
    result.amount = instance.integerParameter(DamageEffect::kTotal);
    result.preResolved = instance.integerParameter(DamageEffect::kPreResolved) != 0;
    result.suppressDamageShields = instance.integerParameter(DamageEffect::kSuppressShields) != 0;
    if (!result.preResolved) {
        result.amount = resolve(result.amount,
            instance.integerParameter(DamageEffect::kDamageFlags),
            static_cast<DamagePower>(instance.integerParameter(DamageEffect::kPower)));
        // The handler publishes the resolved scalar in each present slot.
        for (int &value : result.damageAmounts) if (value >= 0) value = result.amount;
    }
    return result;
}

} // namespace game

} // namespace reone
