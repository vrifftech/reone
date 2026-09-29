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
#include <utility>

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
    _table.reset();
    if (_gameId == resource::GameID::TSL)
        _table = getRequiredTwoDA(_twoDas, "autobalance");
}

const AutoBalanceRow &AutoBalance::get(int multiplierSet) const {
    const auto found = _rows.find(multiplierSet);
    if (found != _rows.end()) return found->second;
    AutoBalanceRow row;
    row.vitalityMultiplier = readMultiplier(*_table, multiplierSet, "vpmult");
    row.toHitMultiplier = readMultiplier(*_table, multiplierSet, "tohitmult");
    row.armorClassMultiplier = readMultiplier(*_table, multiplierSet, "armormult");
    row.damageMultiplier = readMultiplier(*_table, multiplierSet, "damagemult");
    row.savingThrowMultiplier = readMultiplier(*_table, multiplierSet, "savemult");
    row.challengeRatingModifier = readAdjustment(*_table, multiplierSet, "crmod");
    row.levelMultiplier = readMultiplier(*_table, multiplierSet, "levelmult");
    return _rows.emplace(multiplierSet, row).first->second;
}

} // namespace game

} // namespace reone
