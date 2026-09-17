/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <optional>

namespace reone::game {

// gameeffects.2da row selection for effect-immunity queries. This does not
// implement state admission, stacking, or lifecycle handling.
constexpr std::optional<int> getEffectImmunityRow(bool tsl, int type, int state = 0) {
    switch (type) {
    case 8:
        switch (state) {
        case 1: return 22;
        case 2: return 24;
        case 3: return 21;
        case 4: return 19;
        case 5: return 18;
        case 6: return 23;
        case 7: return 15;
        case 8: return 13;
        case 18: return tsl ? std::optional<int>(26) : std::nullopt;
        case 19: return tsl ? std::optional<int>(25) : std::nullopt;
        default: return std::nullopt;
        }
    case 11: return 4;
    case 14: return 5;
    case 17: return 6;
    case 18: return 1;
    case 19: return 20;
    case 27: return 9;
    case 29: return 8;
    case 34: return 10;
    case 35: return 2;
    case 37: return 3;
    case 49: return 7;
    case 56: return 11;
    case 93: return 12;
    case 94: return 13;
    case 95: return 14;
    case 97: return 15;
    case 99: return 16;
    case 100: return 17;
    // ForcePushed (60) has no root query row; query its child effects instead.
    default: return std::nullopt;
    }
}

} // namespace reone::game
