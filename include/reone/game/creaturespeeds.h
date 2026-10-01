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

#include <string>
#include <vector>

namespace reone {

namespace resource {

class ITwoDAs;

} // namespace resource

namespace game {

/** How fast a creature on a creaturespeed row walks and runs, in metres a second. */
struct CreatureSpeed {
    float walkRate {0.0f};
    float runRate {0.0f};
};

class ICreatureSpeeds {
public:
    virtual ~ICreatureSpeeds() = default;

    /** The speeds of a row; a row past the table walks and runs at nothing. */
    virtual const CreatureSpeed &get(int row) const = 0;
    /** The row with this name, or the first row when none has it. */
    virtual int find(const std::string &name) const = 0;
};

class CreatureSpeeds : public ICreatureSpeeds, boost::noncopyable {
public:
    explicit CreatureSpeeds(resource::ITwoDAs &twoDas) :
        _twoDas(twoDas) {
    }

    void init();

    const CreatureSpeed &get(int row) const override;
    int find(const std::string &name) const override;

private:
    struct Row {
        std::string name;
        CreatureSpeed speed;
    };

    resource::ITwoDAs &_twoDas;
    std::vector<Row> _rows;
};

} // namespace game

} // namespace reone
