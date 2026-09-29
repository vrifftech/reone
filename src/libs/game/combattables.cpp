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

#include "reone/game/combattables.h"

#include <cstdio>

#include <boost/algorithm/string/case_conv.hpp>

#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/provider/2das.h"

using namespace reone::resource;

namespace reone {

namespace game {

// The cost tables the rules name directly, besides those iprp_costtable names.
static const char *const kNamedCostTables[] = {
    "iprp_bonuscost", "iprp_meleecost", "iprp_neg5cost", "iprp_resistcost",
    "iprp_soakcost", "iprp_damvulcost", "iprp_onhitdc"};

// The damage rules initialize the value before %i conversion, so a blank or
// missing dice cell is zero rather than a literal-damage encoding.
static int scanInteger(const TwoDA &table, int row, const char *column) {
    int value = 0;
    const auto cell = table.getString(row, column);
    std::sscanf(cell.c_str(), "%i", &value);
    return value;
}

template<class T>
static const T &required(const std::optional<T> &table, const char *resRef) {
    if (!table) {
        throw ResourceNotFoundException(std::string("2DA not found: ") + resRef);
    }
    return *table;
}

static void readCostTable(ITwoDAs &twoDas, const std::string &resRef,
                          std::unordered_map<std::string, CostTable> &tables) {
    if (resRef.empty() || tables.count(resRef) != 0) {
        return;
    }
    auto table = twoDas.get(resRef);
    if (!table) {
        return;
    }
    CostTable result;
    for (int row = 0; row < table->getRowCount(); ++row) {
        result.rows.push_back({table->getIntOpt(row, "value"), table->getIntOpt(row, "amount")});
    }
    tables.emplace(resRef, std::move(result));
}

void CombatTables::init(ITwoDAs &twoDas) {
    if (auto costTables = twoDas.get("iprp_costtable")) {
        std::vector<std::string> names;
        for (int row = 0; row < costTables->getRowCount(); ++row) {
            names.push_back(boost::to_lower_copy(costTables->getString(row, "name")));
            readCostTable(twoDas, names.back(), _costTables);
        }
        _costTableNames = std::move(names);
    }
    for (const char *resRef : kNamedCostTables) {
        readCostTable(twoDas, resRef, _costTables);
    }

    if (auto damageCosts = twoDas.get("iprp_damagecost")) {
        std::vector<DamageCost> rows;
        for (int row = 0; row < damageCosts->getRowCount(); ++row) {
            rows.push_back({scanInteger(*damageCosts, row, "rank"),
                            scanInteger(*damageCosts, row, "numdice"),
                            scanInteger(*damageCosts, row, "die")});
        }
        _damageCosts = std::move(rows);
    }
    if (auto monsterCosts = twoDas.get("iprp_monstcost")) {
        std::vector<MonsterDamageCost> rows;
        for (int row = 0; row < monsterCosts->getRowCount(); ++row) {
            rows.push_back({monsterCosts->getInt(row, "numdice", 0), monsterCosts->getInt(row, "die", 0)});
        }
        _monsterDamageCosts = std::move(rows);
    }
    if (auto damageTypes = twoDas.get("iprp_damagetype")) {
        _damageTypeCount = damageTypes->getRowCount();
    }
    if (auto protection = twoDas.get("iprp_protection")) {
        _protectionCount = protection->getRowCount();
    }

    if (auto onHit = twoDas.get("iprp_onhit")) {
        std::vector<int> rows;
        for (int row = 0; row < onHit->getRowCount(); ++row) {
            rows.push_back(onHit->getIntOpt(row, "param1resref").value_or(0));
        }
        _onHitParameterTables = std::move(rows);
    }
    if (auto durations = twoDas.get("iprp_onhitdur")) {
        std::vector<OnHitDuration> rows;
        for (int row = 0; row < durations->getRowCount(); ++row) {
            rows.push_back({durations->getIntOpt(row, "effectchance").value_or(0),
                            durations->getIntOpt(row, "durationrounds").value_or(0)});
        }
        _onHitDurations = std::move(rows);
    }

    if (auto gameEffects = twoDas.get("gameeffects")) {
        // The first column is the label; each further one is an immunity.
        std::vector<std::vector<int>> rows;
        const auto &columns = gameEffects->columns();
        for (int row = 0; row < gameEffects->getRowCount(); ++row) {
            std::vector<int> immunities;
            for (size_t column = 1; column < columns.size(); ++column) {
                auto value = gameEffects->getIntOpt(row, columns[column]);
                if (value && *value != 0) immunities.push_back(static_cast<int>(column) - 1);
            }
            rows.push_back(std::move(immunities));
        }
        _gameEffectImmunities = std::move(rows);
    }
    if (auto shields = twoDas.get("forceshields")) {
        std::unordered_map<std::string, ForceShieldRow> rows;
        for (int row = 0; row < shields->getRowCount(); ++row) {
            auto value = [&](const std::string &column) { return shields->getInt(row, column, 0); };
            ForceShieldRow shield;
            shield.visual = value("visualeffectdef");
            for (int index = 0; index < 4; ++index) {
                const std::string suffix = "_0" + std::to_string(index + 1);
                shield.appearances[index] = shields->getIntOpt(row, "appearance" + suffix);
                shield.appearanceVisuals[index] = value("visualeffect" + suffix);
            }
            for (int index = 0; index < static_cast<int>(shield.radii.size()); ++index) {
                shield.radii[index] = shields->getFloat(row, "radius_0" + std::to_string(index));
            }
            shield.damageFlags = value("damageflags");
            shield.vulnerabilities = value("vulnerflags");
            shield.resistance = value("resistance");
            shield.amount = value("amount");
            rows.emplace(boost::to_lower_copy(shields->rows()[row].label), shield);
        }
        _forceShields = std::move(rows);
    }
    if (auto adjustments = twoDas.get("forceadjust")) {
        std::vector<std::pair<float, float>> rows;
        for (int row = 0; row < adjustments->getRowCount(); ++row) {
            rows.emplace_back(adjustments->getFloat(row, "goodcost", 1.0f), adjustments->getFloat(row, "evilcost", 1.0f));
        }
        _forceCostMultipliers = std::move(rows);
    }
    if (auto excited = twoDas.get("excitedduration")) {
        std::vector<uint32_t> rows;
        for (int row = 0; row < excited->getRowCount(); ++row) {
            rows.push_back(static_cast<uint32_t>(excited->getInt(row, "duration", 0)));
        }
        _excitedDurations = std::move(rows);
    }
    if (auto poisons = twoDas.get("poison")) {
        std::vector<PoisonData> rows;
        for (int row = 0; row < poisons->getRowCount(); ++row) {
            auto get = [&](const char *column) { return poisons->getIntOpt(row, column).value_or(0); };
            PoisonData poison;
            poison.nameStrRef = get("name");
            poison.difficultyClass = get("dc_save");
            poison.duration = get("duration");
            poison.period = get("period");
            poison.hitPointDamage = get("dam_hp");
            poison.forcePointDamage = get("dam_fp");
            poison.abilityDamage = {get("dam_str"), get("dam_dex"), get("dam_con"),
                                    get("dam_int"), get("dam_wis"), get("dam_chr")};
            rows.push_back(poison);
        }
        _poisons = std::move(rows);
    }
    if (auto stateScripts = twoDas.get("statescripts")) {
        std::vector<std::string> rows;
        for (int row = 0; row < stateScripts->getRowCount(); ++row) {
            rows.push_back(stateScripts->getString(row, "scriptname"));
        }
        _stateScripts = std::move(rows);
    }
}

const CostTable &CombatTables::costTable(const std::string &resRef) const {
    auto table = _costTables.find(resRef);
    if (table == _costTables.end()) {
        throw ResourceNotFoundException("2DA not found: " + resRef);
    }
    return table->second;
}

const CostTable &CombatTables::costTableAt(int index) const {
    const auto &names = required(_costTableNames, "iprp_costtable");
    return costTable(index >= 0 && index < static_cast<int>(names.size()) ? names[index] : std::string());
}

DamageCost CombatTables::damageCost(int row) const {
    const auto &rows = required(_damageCosts, "iprp_damagecost");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : DamageCost();
}

MonsterDamageCost CombatTables::monsterDamageCost(int row) const {
    const auto &rows = required(_monsterDamageCosts, "iprp_monstcost");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : MonsterDamageCost();
}

int CombatTables::damageTypeCount() const {
    return required(_damageTypeCount, "iprp_damagetype");
}

int CombatTables::protectionCount() const {
    return required(_protectionCount, "iprp_protection");
}

int CombatTables::onHitParameterTable(int subtype) const {
    const auto &rows = required(_onHitParameterTables, "iprp_onhit");
    validateTwoDARowIndex("iprp_onhit", subtype, static_cast<int>(rows.size()));
    return rows[subtype];
}

OnHitDuration CombatTables::onHitDuration(int row) const {
    const auto &rows = required(_onHitDurations, "iprp_onhitdur");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : OnHitDuration();
}

const std::vector<int> &CombatTables::gameEffectImmunities(int row) const {
    const auto &rows = required(_gameEffectImmunities, "gameeffects");
    validateTwoDARowIndex("gameeffects", row, static_cast<int>(rows.size()));
    return rows[row];
}

const ForceShieldRow *CombatTables::findForceShield(int shield) const {
    if (!_forceShields) {
        return nullptr;
    }
    auto row = _forceShields->find(std::to_string(shield));
    return row != _forceShields->end() ? &row->second : nullptr;
}

ForceShieldRow CombatTables::forceShield(int shield) const {
    required(_forceShields, "forceshields");
    const auto *row = findForceShield(shield);
    return row ? *row : ForceShieldRow();
}

std::optional<float> CombatTables::forceCostMultiplier(int row, bool good) const {
    if (!_forceCostMultipliers) {
        return std::nullopt;
    }
    if (row < 0 || row >= static_cast<int>(_forceCostMultipliers->size())) {
        return 1.0f;
    }
    const auto &costs = (*_forceCostMultipliers)[row];
    return good ? costs.first : costs.second;
}

std::optional<uint32_t> CombatTables::excitedDuration(int row) const {
    if (!_excitedDurations) {
        return std::nullopt;
    }
    return row >= 0 && row < static_cast<int>(_excitedDurations->size()) ? (*_excitedDurations)[row] : 0u;
}

std::optional<PoisonData> CombatTables::poison(int row) const {
    if (!_poisons) {
        return std::nullopt;
    }
    return row >= 0 && row < static_cast<int>(_poisons->size()) ? (*_poisons)[row] : PoisonData();
}

const std::string &CombatTables::stateScript(int row) const {
    const auto &rows = required(_stateScripts, "statescripts");
    validateTwoDARowIndex("statescripts", row, static_cast<int>(rows.size()));
    return rows[row];
}

} // namespace game

} // namespace reone
