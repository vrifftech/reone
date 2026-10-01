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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "../effect.h"

namespace reone {

namespace game {

// Attack-data writes retain the low word; subsequent comparisons sign-extend
// it. Keep integer packet storage and express that operation without relying
// on an out-of-range conversion to a signed short. This is not saturation.
inline int narrowAttackDamage(int64_t amount) {
    const int lowWord = static_cast<uint16_t>(amount);
    return lowWord < 0x8000 ? lowWord : lowWord - 0x10000;
}

// Typed damage already represented as a packet component does not use the NWScript
// flag-to-slot conversion. Preserve its direct mapping.
inline std::size_t getDirectDamageSlot(int flags) {
    if (flags <= 0) return 0;
    std::size_t slot = 0;
    for (unsigned remaining = static_cast<unsigned>(flags); remaining > 1; remaining >>= 1) ++slot;
    return slot;
}

// Shared NWScript EffectDamage flag-to-slot conversion.
// The caller supplies normalized flags (0..8192).
inline std::size_t getScriptDamageSlot(int flags) {
    // Use slot zero for empty flags; avoid taking the logarithm of zero.
    if (flags <= 0) return 0;
    // Keep each intermediate in single precision and prevent reassociation.
    volatile float logarithm = std::log10(static_cast<float>(flags));
    volatile float scaled = logarithm * 0x1.a934f0p+1f; // single-precision multiplier
    volatile float shifted = scaled + 0.5f;
    return static_cast<std::size_t>(static_cast<int>(shifted)); // truncate toward zero
}

inline constexpr int kAllDamageTypeFlags = 0x3fff;
inline constexpr int kPhysicalDamageTypeFlags = 0x4007;

DamageType getPrimaryDamageType(int damageFlags);
bool damageTypeMatches(int modifierFlags, int damageFlags);

/**
 * The part of a typed damage bonus from \p source that would pass the
 * target's immunity and resistance, leaving every pool untouched.
 */
int simulateDamageBonusMitigation(Object &object, const Creature &source, int damageFlags, int damage);

/**
 * Stable identifiers for mitigation-feedback payloads.
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
 * Semantic payload consumed by the mitigation-feedback presentation.
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

    std::vector<MitigationFeedback> mitigationFeedback;
    int finalDamage {0};
    std::array<int, 15> damageAmounts = [] {
        std::array<int, 15> values;
        values.fill(-1);
        return values;
    }();
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
    // Physical base adjustments stay signed through miscellaneous addition
    // until the final pre-mitigation minimum.
    void addPhysicalBase(int amount, DamageType type);
    void addBonus(int amount, DamageType type);
    void setBaseDamageAmounts(const std::array<int, 14> &amounts) { _baseDamageAmounts = amounts; }
    void setDamageFlags(int damageFlags);
    void setPower(DamagePower power);
    void resolve(Object &object, const Creature *source = nullptr);
    /** A simulated resolution leaves the target's resistance and reduction pools untouched. */
    void resolvePhysical(Object &object, const Creature &source, bool simulate = false);
    /**
     * Resolve a deflected bolt's packet against its own shooter. The typed
     * slots keep their rolled amounts; the total meets the shooter's damage
     * resistance and then immunity for the packet's damage flags, without
     * damage reduction, and replaces the flag slot and the total.
     */
    void resolveReturned(Creature &shooter);
    void resolveLightsaberThrow(Object &object, const Creature &caster);
    /**
     * Take the packet as it stands, without immunity, resistance or
     * reduction: the typed slots keep their base amounts and the total is the
     * base damage. A plot target still takes nothing when the damage applies.
     */
    void resolveUnmitigated();

    int total() const;
    int damageFlags() const { return _damageFlags; }
    DamagePower power() const { return _power; }
    /** The weapon cuts doors: a door or placeable takes its maximum hit points as base damage. */
    void setCutsDoors(bool cuts) { _cutsDoors = cuts; }
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
        bool bonus {false};
    };

    void requireUnresolved() const;
    void addResolved(int amount, DamageType type, bool bonus = false);

    DamagePower _power;
    int _damageFlags {0};
    bool _cutsDoors {false};
    std::vector<Component> _components;
    std::optional<DamageResolution> _resolution;
    std::array<int, 14> _baseDamageAmounts = [] {
        std::array<int, 14> values; values.fill(-1); return values;
    }();
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

class Creature;

/**
 * Reports at once the mitigation lines of damage resolved outside a weapon
 * hit, to the damaged object and to the creature that caused it, if any.
 */
void showMitigationFeedback(Object &object, const std::shared_ptr<Creature> &source,
                            const DamageResolution &resolution);

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

    static std::shared_ptr<DamageEffect> fromResolvedAttackAmount(int amount, size_t slot);
    /**
     * A damage shield's retaliation: only the shield's typed slot holds the
     * amount, and the total stays absent, so it resolves to no damage.
     */
    static std::shared_ptr<DamageEffect> fromShieldRetaliation(int amount, int shieldFlags);

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
    /** The effect's own amounts before mitigation; a plot object's are zero. */
    std::array<int, 15> effectAmounts {};
    bool preResolved {false};
    bool suppressDamageShields {false};
};

template <typename Resolve>
std::optional<DamageEffectApplication> resolveDamageEffect(
    const EffectInstance &instance, bool plot, Resolve &&resolve) {
    DamageEffectApplication result;
    for (size_t i = 0; i < result.damageAmounts.size(); ++i) {
        const int value = instance.integerParameter(i);
        result.damageAmounts[i] = plot && value >= 0 ? 0 : value;
    }
    result.effectAmounts = result.damageAmounts;
    result.amount = result.damageAmounts[DamageEffect::kTotal];
    // Entry eligibility is independent of whether mitigation was already performed.
    // A positive input reduced to zero below still completes its application.
    if (result.amount == 0 && !plot) return std::nullopt;

    result.preResolved = instance.integerParameter(DamageEffect::kPreResolved) != 0;
    result.suppressDamageShields = instance.integerParameter(DamageEffect::kSuppressShields) != 0;
    if (!result.preResolved) {
        result.amount = resolve(result.amount,
            instance.integerParameter(DamageEffect::kDamageFlags));
        // The handler publishes the resolved scalar in each present slot.
        for (int &value : result.damageAmounts) if (value >= 0) value = result.amount;
    }
    return result;
}

} // namespace game

} // namespace reone
