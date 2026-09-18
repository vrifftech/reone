/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <algorithm>
#include <vector>
#include <cstdint>
namespace reone::game {
inline float effectStackTextScalar(bool tsl, int screenWidth) {
    return tsl && screenWidth > 1022 ? 1.0f + ((screenWidth - 1022) / 1024) * 0.5f : 1.0f;
}
inline int effectStackDrawCount(int count, bool leader) {
    if (leader) {
        return std::min(count, 9);
    }
    return count >= 6 ? 5 : count;
}
inline int effectStackSpacing(int count, bool leader) {
    if (!leader) {
        return 5;
    }
    if (count <= 4) {
        return 10;
    }
    switch (count) {
    case 5:
        return 10;
    case 6:
        return 8;
    case 7:
        return 7;
    case 8:
        return 6;
    default:
        return 5;
    }
}
inline std::vector<int> effectStackTopOffsets(
    int iconTop, int viewportTop, int viewportHeight,
    int count,
    bool leader,
    bool good, float textScalar = 1.0f) {

    count = effectStackDrawCount(count, leader);
    if (count <= 0) {
        return {};
    }

    int spacing = static_cast<uint8_t>(static_cast<int>(effectStackSpacing(count, leader) * textScalar));
    int top = iconTop;
    if (count <= 4) {
        int firstStep = spacing >> 1;
        int initialOffset = (count - 1) * firstStep;
        top += good ? -initialOffset : initialOffset;
    } else if (good) {
        top = viewportTop + 1;
    } else {
        int bottomMargin = leader ? 16 : 8;
        top = viewportTop + viewportHeight - bottomMargin - 1;
    }

    std::vector<int> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.push_back(top);
        top += good ? spacing : -spacing;
        if ((count == 7 || count == 8) &&
            (index == 0 || index == 2)) {
            top += good ? -1 : 1;
        }
    }
    return result;
}
} // namespace reone::game
