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

#pragma once

#include "reone/resource/types.h"

#include <vector>

namespace reone {

namespace resource {

class ITwoDAs;

} // namespace resource

namespace game {

struct AutoBalanceRow {
    float vitalityMultiplier {0.0f};
    float toHitMultiplier {0.0f};
    float armorClassMultiplier {0.0f};
    float damageMultiplier {0.0f};
    float savingThrowMultiplier {0.0f};
    int challengeRatingModifier {0};
    float levelMultiplier {0.0f};
};

class IAutoBalance {
public:
    virtual ~IAutoBalance() = default;

    virtual const AutoBalanceRow &get(int multiplierSet) const = 0;
};

class AutoBalance : public IAutoBalance, boost::noncopyable {
public:
    AutoBalance(
        resource::GameID gameId,
        resource::ITwoDAs &twoDas) :
        _gameId(gameId),
        _twoDas(twoDas) {
    }

    void init();

    const AutoBalanceRow &get(int multiplierSet) const override;

private:
    resource::GameID _gameId;
    resource::ITwoDAs &_twoDas;
    std::vector<AutoBalanceRow> _rows;
};

} // namespace game

} // namespace reone
