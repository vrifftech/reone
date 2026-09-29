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

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace reone {

namespace resource {

class ITwoDAs;

} // namespace resource

namespace game {

/** A row of an item property cost table. A blank cell has no value. */
struct CostTableRow {
    std::optional<int> value;
    std::optional<int> amount;
};

/** An item property cost table. A row outside it reads as blank. */
struct CostTable {
    std::vector<CostTableRow> rows;

    CostTableRow row(int index) const {
        return index >= 0 && index < static_cast<int>(rows.size()) ? rows[index] : CostTableRow();
    }
};

/**
 * A damage bonus code (iprp_damagecost): its rank and dice. A blank or
 * missing cell is zero.
 */
struct DamageCost {
    int rank {0};
    int numDice {0};
    int die {0};
};

/** A monster damage code (iprp_monstcost): its dice. */
struct MonsterDamageCost {
    int numDice {0};
    int die {0};
};

/** An on-hit property's chance and duration (iprp_onhitdur). */
struct OnHitDuration {
    int chance {0};
    int rounds {0};
};

/** A Force shield (forceshields.2da). A blank or missing cell is zero. */
struct ForceShieldRow {
    int visual {0};
    // The appearances with their own visual; a blank one matches none.
    std::array<std::optional<int>, 4> appearances;
    std::array<int, 4> appearanceVisuals {};
    // radius_00 to radius_05.
    std::array<float, 6> radii {};
    int damageFlags {0};
    int vulnerabilities {0};
    int resistance {0};
    int amount {0};
};

/** A poison (poison.2da). A blank or missing cell, or a row outside it, is zero. */
struct PoisonData {
    int nameStrRef {0};
    int difficultyClass {0};
    int duration {0};
    int period {0};
    int hitPointDamage {0};
    int forcePointDamage {0};
    std::array<int, 6> abilityDamage {};
};

/**
 * The tables the combat rules read on every attack, hit and effect, parsed
 * once at start-up. A lookup in a table the game does not have fails as the
 * table's absence did before, when the table is required.
 */
class ICombatTables {
public:
    virtual ~ICombatTables() = default;

    /** An item property cost table by name. */
    virtual const CostTable &costTable(const std::string &resRef) const = 0;
    /** The item property cost table that iprp_costtable names at index. */
    virtual const CostTable &costTableAt(int index) const = 0;
    virtual DamageCost damageCost(int row) const = 0;
    virtual MonsterDamageCost monsterDamageCost(int row) const = 0;
    /** iprp_damagetype and iprp_protection row counts. */
    virtual int damageTypeCount() const = 0;
    virtual int protectionCount() const = 0;

    /** The parameter table (param1resref) of an on-hit subtype. */
    virtual int onHitParameterTable(int subtype) const = 0;
    /** Zero chance and rounds outside the table. */
    virtual OnHitDuration onHitDuration(int row) const = 0;

    /** The immunities whose gameeffects.2da column is set for the row. */
    virtual const std::vector<int> &gameEffectImmunities(int row) const = 0;
    /** The shield's row, or nothing when the table or the shield is missing. */
    virtual const ForceShieldRow *findForceShield(int shield) const = 0;
    /** The shield's row; a missing shield reads as blank. */
    virtual ForceShieldRow forceShield(int shield) const = 0;
    /** Nothing when the table is missing. */
    virtual std::optional<float> forceCostMultiplier(int row, bool good) const = 0;
    virtual std::optional<uint32_t> excitedDuration(int row) const = 0;
    virtual std::optional<PoisonData> poison(int row) const = 0;
    virtual const std::string &stateScript(int row) const = 0;
};

class CombatTables : public ICombatTables {
public:
    void init(resource::ITwoDAs &twoDas);

    const CostTable &costTable(const std::string &resRef) const override;
    const CostTable &costTableAt(int index) const override;
    DamageCost damageCost(int row) const override;
    MonsterDamageCost monsterDamageCost(int row) const override;
    int damageTypeCount() const override;
    int protectionCount() const override;

    int onHitParameterTable(int subtype) const override;
    OnHitDuration onHitDuration(int row) const override;

    const std::vector<int> &gameEffectImmunities(int row) const override;
    const ForceShieldRow *findForceShield(int shield) const override;
    ForceShieldRow forceShield(int shield) const override;
    std::optional<float> forceCostMultiplier(int row, bool good) const override;
    std::optional<uint32_t> excitedDuration(int row) const override;
    std::optional<PoisonData> poison(int row) const override;
    const std::string &stateScript(int row) const override;

private:
    std::unordered_map<std::string, CostTable> _costTables;
    std::optional<std::vector<std::string>> _costTableNames;
    std::optional<std::vector<DamageCost>> _damageCosts;
    std::optional<std::vector<MonsterDamageCost>> _monsterDamageCosts;
    std::optional<int> _damageTypeCount;
    std::optional<int> _protectionCount;
    std::optional<std::vector<int>> _onHitParameterTables;
    std::optional<std::vector<OnHitDuration>> _onHitDurations;
    std::optional<std::vector<std::vector<int>>> _gameEffectImmunities;
    // By lower-case label; the first row of a label is the one found.
    std::optional<std::unordered_map<std::string, ForceShieldRow>> _forceShields;
    // Good, then evil cost multipliers.
    std::optional<std::vector<std::pair<float, float>>> _forceCostMultipliers;
    std::optional<std::vector<uint32_t>> _excitedDurations;
    std::optional<std::vector<PoisonData>> _poisons;
    std::optional<std::vector<std::string>> _stateScripts;
};

} // namespace game

} // namespace reone
