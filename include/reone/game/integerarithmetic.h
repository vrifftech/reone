/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace reone::game {
// Checked floating-point conversion shared by effect consumers.
inline std::int32_t truncateToInteger32(float value) {
    // The invalid-conversion sentinel, without undefined C++ conversion.
    if (!std::isfinite(value) || value >= 2147483648.0f || value < -2147483648.0f)
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
} // namespace reone::game
