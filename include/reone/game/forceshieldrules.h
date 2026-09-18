/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "effect.h"
#include "reone/resource/2da.h"
namespace reone::game {
struct ForceShieldDefinition {
    int visual;
    int damageFlags;
    int vulnerabilities;
    int resistance;
    int amount;
};
inline ForceShieldDefinition readForceShieldDefinition(const resource::TwoDA &table,
                                                       int shield, int appearance) {
    const int row = table.indexByLabel(std::to_string(shield));
    auto value = [&](const std::string &column) { return table.getInt(row, column, 0); };
    int visual = value("visualeffectdef");
    for (int index = 1; index <= 4; ++index) {
        const std::string suffix = "_0" + std::to_string(index);
        auto requiredAppearance = table.getIntOpt(row, "appearance" + suffix);
        if (requiredAppearance && *requiredAppearance == appearance) {
            visual = value("visualeffect" + suffix);
            break;
        }
    }
    (void)value("permanent"); // Read by both handlers, not a lifetime override.
    return {visual, value("damageflags"), value("vulnerflags"), value("resistance"), value("amount")};
}
template<class Records>
inline std::optional<EffectId> replacedForceShield(const Records &records) {
    for (const auto &record : records) {
        if (record.serializedType > 107) break;
        if (record.serializedType != 107) continue;
        // GetForceShield returns the first root's argument, including zero.
        return record.integerParameter(0) != 0 ? std::optional<EffectId>(record.id) : std::nullopt;
    }
    return std::nullopt;
}
} // namespace reone::game
