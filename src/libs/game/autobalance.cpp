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
#include "reone/system/exception/validation.h"

#include <string>
#include <utility>

namespace reone {

namespace game {

static float getRequiredFloat(
    const resource::TwoDA &table,
    int row,
    const std::string &column) {

    auto value = table.getFloatOpt(row, column);
    if (!value) {
        throw ValidationException(
            "autobalance.2da " + column + " missing at row " +
            std::to_string(row));
    }
    return *value;
}

static int getRequiredInt(
    const resource::TwoDA &table,
    int row,
    const std::string &column) {

    auto value = table.getIntOpt(row, column);
    if (!value) {
        throw ValidationException(
            "autobalance.2da " + column + " missing at row " +
            std::to_string(row));
    }
    return *value;
}

void AutoBalance::init() {
    _rows.clear();
    if (_gameId != resource::GameID::TSL) {
        return;
    }

    auto table = getRequiredTwoDA(_twoDas, "autobalance");
    if (table->getRowCount() == 0) {
        throw ValidationException(
            "autobalance.2da requires at least one row");
    }

    std::vector<AutoBalanceRow> rows;
    rows.reserve(table->getRowCount());

    for (int row = 0; row < table->getRowCount(); ++row) {
        AutoBalanceRow balance;
        balance.vitalityMultiplier =
            getRequiredFloat(*table, row, "vpmult");
        balance.toHitMultiplier =
            getRequiredFloat(*table, row, "tohitmult");
        balance.armorClassMultiplier =
            getRequiredFloat(*table, row, "armormult");
        balance.damageMultiplier =
            getRequiredFloat(*table, row, "damagemult");
        balance.savingThrowMultiplier =
            getRequiredFloat(*table, row, "savemult");
        balance.challengeRatingModifier =
            getRequiredInt(*table, row, "crmod");
        balance.levelMultiplier =
            getRequiredFloat(*table, row, "levelmult");
        rows.push_back(balance);
    }

    _rows = std::move(rows);
}

const AutoBalanceRow &AutoBalance::get(int multiplierSet) const {
    if (multiplierSet < 0 || multiplierSet >= static_cast<int>(_rows.size())) {
        throw ValidationException(
            "autobalance.2da row out of range: " +
            std::to_string(multiplierSet) + "/" +
            std::to_string(_rows.size()));
    }
    return _rows[multiplierSet];
}

} // namespace game

} // namespace reone
