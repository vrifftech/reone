/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/script/enginetype.h"

#include "types.h"

#include <cstdint>

namespace reone {

namespace game {

class Talent : public script::EngineType {
public:
    // Normalize selector inputs to the byte domain at value creation.
    // Callers and runtime storage use ordinary integers.
    Talent(
        TalentType type,
        int value,
        int castingClass = 0,
        uint32_t item = 0x7f000000,
        int itemPropertyIndex = -1,
        int casterLevel = kUnspecifiedCasterLevel,
        int metaType = 255) :
        _type(type),
        _value(value),
        _castingClass(static_cast<uint8_t>(castingClass)),
        _item(item),
        _itemPropertyIndex(itemPropertyIndex),
        _casterLevel(static_cast<uint8_t>(casterLevel)),
        _metaType(static_cast<uint8_t>(metaType)) {
    }

    TalentType type() const { return _type; }
    int value() const { return _value; }
    int castingClass() const { return _castingClass; }
    uint32_t item() const { return _item; }
    int itemPropertyIndex() const { return _itemPropertyIndex; }
    int casterLevel() const { return _casterLevel; }
    int metaType() const { return _metaType; }

private:
    TalentType _type;
    int _value;
    int _castingClass;
    uint32_t _item;
    int _itemPropertyIndex;
    int _casterLevel;
    int _metaType;
};

} // namespace game

} // namespace reone
