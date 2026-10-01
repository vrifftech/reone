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

#include "reone/game/creaturespeeds.h"

#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"

#include <boost/algorithm/string.hpp>

namespace reone {

namespace game {

void CreatureSpeeds::init() {
    auto table = getRequiredTwoDA(_twoDas, "creaturespeed");
    _rows.clear();
    _rows.reserve(table->getRowCount());
    for (int row = 0; row < table->getRowCount(); ++row) {
        _rows.push_back(Row {
            boost::to_lower_copy(table->getString(row, "2daname")),
            {table->getFloat(row, "walkrate", 0.0f), table->getFloat(row, "runrate", 0.0f)}});
    }
}

const CreatureSpeed &CreatureSpeeds::get(int row) const {
    // The stored row is not range-checked, so a hand-edited one can point past
    // the table.
    static const CreatureSpeed missing;
    if (row < 0 || row >= static_cast<int>(_rows.size())) return missing;
    return _rows[row].speed;
}

int CreatureSpeeds::find(const std::string &name) const {
    const auto lowerName = boost::to_lower_copy(name);
    for (size_t row = 0; row < _rows.size(); ++row) {
        if (_rows[row].name == lowerName) return static_cast<int>(row);
    }
    return 0;
}

} // namespace game

} // namespace reone
