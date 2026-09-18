/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <string>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace reone::game {

/** The two fields transported by the attacked event, not the actor's action enum. */
struct AttackHistory {
    uint16_t type {0};
    uint8_t mode {0};

    int scriptType(bool inCombat) const {
        if (!inCombat) return 0;
        return type == 11 ? 9 : type == 30 ? 10 : 0;
    }
    int scriptMode(bool inCombat) const {
        if (!inCombat) return 0x7f000000;
        constexpr int values[] = {0, 1, 2, 3, 0, 4, 5};
        return mode < 7 ? values[mode] : 0;
    }
};

/**
 * Non-reference attack-save fields. Together with AttackHistory, ReactObject
 * and AmmoItem, these make up the 25 serialized attack fields. This record
 * transports history; it does not apply damage or generate visual reactions.
 */
struct AttackEventFields {
    uint8_t group {0xff};
    uint16_t animationLength {0};
    uint32_t missedBy {0};
    uint8_t result {0};
    uint16_t reactionDelay {0};
    uint16_t reactionAnimation {10001};
    uint16_t reactionAnimationLength {0};
    uint8_t concealment {0};
    int32_t ranged {0};
    int32_t sneakAttack {0};
    uint8_t weaponAttackType {0};
    std::array<float, 3> rangedTarget {};
    // Retain gameplay precision; the signed-short cast is at save only.
    std::array<int32_t, 15> damage;
    uint8_t killingBlow {0};
    uint8_t coupDeGrace {0};
    uint8_t criticalThreat {0};
    uint8_t deflected {0};
    std::string attackDebugText;
    std::string damageDebugText;

    AttackEventFields() { damage.fill(-1); }
};

/** Per-recipient snapshot. Next deliberately does not resolve saved IDs. */
class AttackerList {
public:
    template <class Objects, class Matches, class Id>
    uint32_t first(const Objects *objects, Matches matches, Id id) {
        if (!objects) return 0x7f000000;
        _ids.clear();
        for (const auto &object : *objects) {
            if (matches(object)) _ids.push_back(id(object));
        }
        if (_ids.empty()) return 0x7f000000;
        _next = 1;
        return _ids.front();
    }
    uint32_t next() {
        return _next < _ids.size() ? _ids[_next++] : 0x7f000000;
    }
    void clear() { _ids.clear(); _next = 0; }

private:
    std::vector<uint32_t> _ids;
    size_t _next {0};
};

} // namespace reone::game
