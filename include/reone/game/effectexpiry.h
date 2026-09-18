/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>

namespace reone::game {
// Repeated single-precision day subtraction preserves the duration remainder.
// The duration remains a floating-point value until its time component is stored.
inline uint64_t getEffectDurationMilliseconds(float seconds, uint32_t dayMilliseconds) {
    if (seconds <= 0.0f) return 0;
    const float daySeconds = static_cast<float>(dayMilliseconds / 1000u);
    uint64_t days = 0;
    while (seconds >= daySeconds) { seconds -= daySeconds; ++days; }
    return days * dayMilliseconds + static_cast<uint32_t>(seconds * 1000.0f);
}
inline uint64_t getEffectExpiryMilliseconds(uint64_t start, float seconds, uint32_t dayMilliseconds) {
    return start + getEffectDurationMilliseconds(seconds, dayMilliseconds);
}
} // namespace reone::game
