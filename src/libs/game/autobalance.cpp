/*
 * Copyright (c) 2026 The reone project contributors
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

#include "reone/game/autobalance.h"

#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include <cstdio>

#include <string>

namespace reone {

namespace game {

// Selected numeric table cells initialize to zero before %f/%i conversion.
// Empty or deleted cells do not reject the whole row or select another multiplier set.
static float readMultiplier(const resource::TwoDA &table, int row, const char *column) {
    const auto cell = table.getString(row, column);
    float value = 0.0f;
    std::sscanf(cell.c_str(), "%f", &value);
    return value;
}

static int readAdjustment(const resource::TwoDA &table, int row, const char *column) {
    const auto cell = table.getString(row, column);
    int value = 0;
    std::sscanf(cell.c_str(), "%i", &value);
    return value;
}

void AutoBalance::init() {
    _rows.clear();
    if (_gameId != resource::GameID::TSL) {
        return;
    }
    const auto table = getRequiredTwoDA(_twoDas, "autobalance");
    _rows.reserve(table->getRowCount());
    for (int multiplierSet = 0; multiplierSet < table->getRowCount(); ++multiplierSet) {
        AutoBalanceRow row;
        row.vitalityMultiplier = readMultiplier(*table, multiplierSet, "vpmult");
        row.toHitMultiplier = readMultiplier(*table, multiplierSet, "tohitmult");
        row.armorClassMultiplier = readMultiplier(*table, multiplierSet, "armormult");
        row.damageMultiplier = readMultiplier(*table, multiplierSet, "damagemult");
        row.savingThrowMultiplier = readMultiplier(*table, multiplierSet, "savemult");
        row.challengeRatingModifier = readAdjustment(*table, multiplierSet, "crmod");
        row.levelMultiplier = readMultiplier(*table, multiplierSet, "levelmult");
        _rows.push_back(row);
    }
}

const AutoBalanceRow &AutoBalance::get(int multiplierSet) const {
    // A multiplier set with no row in the table has every multiplier at zero.
    static const AutoBalanceRow kMissingRow;
    if (multiplierSet < 0 || multiplierSet >= static_cast<int>(_rows.size())) {
        return kMissingRow;
    }
    return _rows[multiplierSet];
}

} // namespace game

} // namespace reone
