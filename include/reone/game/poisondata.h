/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <array>
#include "reone/resource/2da.h"

namespace reone::game {
struct PoisonData {
    int nameStrRef {0};
    int difficultyClass {0};
    int duration {0};
    int period {0};
    int hitPointDamage {0};
    int forcePointDamage {0};
    std::array<int, 6> abilityDamage {};
};

// poison row/cell contract: absent rows/columns and blank cells are
// zero, but an absent table is still an error in the owning resource loader.
// Malformed nonblank numbers keep TwoDA's existing conversion exception.
inline PoisonData readPoisonData(const resource::TwoDA &table, int row) {
    auto get = [&](const char *column) { return table.getIntOpt(row, column).value_or(0); };
    PoisonData result;
    result.nameStrRef = get("name");
    result.difficultyClass = get("dc_save");
    result.duration = get("duration");
    result.period = get("period");
    result.hitPointDamage = get("dam_hp");
    result.forcePointDamage = get("dam_fp");
    result.abilityDamage = {get("dam_str"), get("dam_dex"), get("dam_con"),
                            get("dam_int"), get("dam_wis"), get("dam_chr")};
    return result;
}

} // namespace reone::game
