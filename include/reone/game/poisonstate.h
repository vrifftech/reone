/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "integerarithmetic.h"
#include "effect.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace reone::game {

inline std::int32_t poisonAbilityAmount(int base, float factor) {
    return base * truncateToInteger32(factor);
}
inline std::int32_t poisonAbilityDuration(int duration, int period, float factor) {
    return duration - (truncateToInteger32(factor) - 1) * period;
}

// Working view of the canonical root payload; it is decoded and committed
// at each callback, never maintained as a second live timer.
struct PoisonState {
    enum class Step { None, Damage, Conversation, Expired };
    std::uint64_t startMilliseconds {0};
    std::uint64_t tickMilliseconds {0};
    std::int32_t durationSeconds {0};
    std::int32_t periodSeconds {0};
    float factor {2.0f};

    void start(std::uint64_t now, int duration, int period) {
        startMilliseconds = tickMilliseconds = now;
        durationSeconds = duration;
        periodSeconds = period;
        factor = 2.0f;
    }
    Step step(std::uint64_t now, std::uint32_t millisecondsPerDay, bool conversation) {
        // SubtractWorldTimes returns a day count and a remainder. The poison
        // branch deliberately compares the remainder, not total elapsed days.
        const auto age = static_cast<std::uint32_t>((now - startMilliseconds) % millisecondsPerDay);
        if (age > std::uint32_t(durationSeconds) * 1000U) {
            return Step::Expired;
        }
        const auto elapsed = static_cast<std::uint32_t>((now - tickMilliseconds) % millisecondsPerDay);
        if (elapsed <= std::uint32_t(periodSeconds) * 1000U) return Step::None;
        if (conversation) {
            tickMilliseconds = now; // No external tick callback on this branch.
            durationSeconds += periodSeconds;
            return Step::Conversation;
        }
        return Step::Damage;
    }
    void completeTick(std::uint64_t now) {
        factor += 1.0f;
        tickMilliseconds = now; // Commit after damage, not before callbacks.
    }
};

// integer slots: row, start day/time, tick day/time, duration, period,
// damage mode. Float slot zero is the retained periodic factor.
inline PoisonState readPoisonState(const EffectInstance &record, uint32_t dayMilliseconds) {
    PoisonState state;
    state.startMilliseconds = uint64_t(uint32_t(record.integerParameter(1))) * dayMilliseconds + uint32_t(record.integerParameter(2));
    state.tickMilliseconds = uint64_t(uint32_t(record.integerParameter(3))) * dayMilliseconds + uint32_t(record.integerParameter(4));
    state.durationSeconds = record.integerParameter(5);
    state.periodSeconds = record.integerParameter(6);
    state.factor = record.floatParameters[0];
    return state;
}
inline void storePoisonState(EffectInstance &record, const PoisonState &state, uint32_t dayMilliseconds) {
    record.setIntegerParameter(1, static_cast<int32_t>(state.startMilliseconds / dayMilliseconds));
    record.setIntegerParameter(2, static_cast<int32_t>(state.startMilliseconds % dayMilliseconds));
    record.setIntegerParameter(3, static_cast<int32_t>(state.tickMilliseconds / dayMilliseconds));
    record.setIntegerParameter(4, static_cast<int32_t>(state.tickMilliseconds % dayMilliseconds));
    record.setIntegerParameter(5, state.durationSeconds);
    record.setIntegerParameter(6, state.periodSeconds);
    record.floatParameters[0] = state.factor;
}
} // namespace reone::game
