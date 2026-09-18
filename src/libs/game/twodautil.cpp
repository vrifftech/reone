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

#include "reone/game/twodautil.h"
#include <cassert>

#include "reone/resource/2da.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/provider/2das.h"
#include "reone/system/exception/validation.h"

using namespace reone::resource;

namespace reone {

namespace game {

std::shared_ptr<TwoDA> getRequiredTwoDA(
    ITwoDAs &twoDas,
    const std::string &resRef) {

    auto table = twoDas.get(resRef);
    if (!table) {
        throw ResourceNotFoundException("2DA not found: " + resRef);
    }
    return table;
}

void validateTwoDARow(
    const TwoDA &table,
    const std::string &resRef,
    int row) {

    if (row < 0 || row >= table.getRowCount()) {
        throw ValidationException(str(boost::format(
            "%s.2da row out of range: %d/%d") %
            resRef % row % table.getRowCount()));
    }
}

int readRacialAbilityAdjustment(
    const TwoDA &table, int row, Ability ability) {
    static constexpr const char *columns[] = {
        "stradjust", "dexadjust", "conadjust", "intadjust", "wisadjust", "chaadjust"};
    const int index = static_cast<int>(ability);
    assert(index >= 0 && index < 6 && "Invalid racial ability selector");
    validateTwoDARow(table, "racialtypes", row);
    if (std::find(table.columns().begin(), table.columns().end(), columns[index]) ==
        table.columns().end()) {
        throw ValidationException(std::string("racialtypes.2da missing column: ") + columns[index]);
    }
    // Blank integer cells decode as zero.
    const auto byte = static_cast<uint8_t>(table.getInt(row, columns[index], 0));
    return byte < 128u ? static_cast<int>(byte) : static_cast<int>(byte) - 256;
}

} // namespace game

} // namespace reone
