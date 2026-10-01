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

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include <boost/algorithm/string/case_conv.hpp>

#include "reone/game/animationutil.h"
#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/provider/2das.h"
#include "reone/system/exception/validation.h"

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

static const std::string kBlank;
static const CreatureSoundSet kBlankSoundSet;

// Thresholds compare unsigned: the last row, 0xFFFFFFFF, is beyond a signed
// integer.
static std::optional<uint32_t> readExperienceThreshold(const TwoDA &table, int row) {
    const auto value = table.getStringOpt(row, "xp");
    if (!value || value->empty()) {
        return std::nullopt;
    }
    const bool hex = value->size() > 2 && (*value)[0] == '0' && ((*value)[1] == 'x' || (*value)[1] == 'X');
    std::size_t end = 0;
    const auto number = std::stoll(*value, &end, hex ? 16 : 10);
    if (end != value->size() || number < -2147483648LL || number > 4294967295LL) {
        throw ValidationException("Invalid XP threshold at row " + std::to_string(row));
    }
    return static_cast<uint32_t>(number);
}

// A regeneration value by label uses scanf conversion: a missing label or
// value is zero.
static float regenerationValue(const TwoDA &table, const char *label) {
    const int row = table.indexByLabel(label);
    if (row < 0) {
        return 0.0f;
    }
    float value = 0.0f;
    const auto cell = table.getString(row, "value");
    std::sscanf(cell.c_str(), "%f", &value);
    return value;
}

static const char *const kRacialAdjustmentColumns[] = {
    "stradjust", "dexadjust", "conadjust", "intadjust", "wisadjust", "chaadjust"};

// A tutorial column matches its name in any case; the first match is read. A
// message column is named Message or Message_PC and its page number.
static std::vector<TutorialRow> readTutorials(const TwoDA &table) {
    const auto &columns = table.columns();
    std::vector<TutorialRow> rows(table.getRowCount());
    std::vector<std::string> seen;
    for (size_t column = 0; column < columns.size(); ++column) {
        const auto name = boost::to_lower_copy(columns[column]);
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        auto cell = [&](int row) {
            const auto &value = table.rows()[row].values[column];
            return value == "****" ? std::string() : value;
        };
        if (name == "icon") {
            for (int row = 0; row < table.getRowCount(); ++row) rows[row].icon = cell(row);
            continue;
        }
        const bool pc = name.compare(0, 10, "message_pc") == 0;
        if (!pc && name.compare(0, 7, "message") != 0) continue;
        const auto digits = name.substr(pc ? 10 : 7);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const auto page = static_cast<size_t>(std::stoul(digits));
        if (std::to_string(page) != digits) continue;
        for (int row = 0; row < table.getRowCount(); ++row) {
            auto &messages = pc ? rows[row].pcMessages : rows[row].messages;
            if (messages.size() <= page) messages.resize(page + 1);
            const auto value = cell(row);
            if (!value.empty()) messages[page] = std::atoi(value.c_str());
        }
    }
    return rows;
}

static std::vector<bool> readDialogAnimationRows(const TwoDA &table) {
    std::vector<bool> rows;
    for (int row = 0; row < table.getRowCount(); ++row) {
        rows.push_back(isDialogAnimationRow(table, row));
    }
    return rows;
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

    if (auto kept = twoDas.get("removefxondeath")) {
        std::vector<int> types;
        for (int row = 0; row < kept->getRowCount(); ++row) {
            if (auto type = kept->getIntOpt(row, "effecttype")) types.push_back(*type);
        }
        _deathKeptEffectTypes = std::move(types);
    }
    if (auto thresholds = twoDas.get("exptable")) {
        std::vector<std::optional<uint32_t>> rows;
        for (int row = 0; row < thresholds->getRowCount(); ++row) {
            rows.push_back(readExperienceThreshold(*thresholds, row));
        }
        _experienceThresholds = std::move(rows);
    }
    if (auto experience = twoDas.get("xptable")) {
        std::vector<std::vector<float>> rows;
        const auto &columns = experience->columns();
        for (int row = 0; row < experience->getRowCount(); ++row) {
            std::vector<float> cells;
            for (const auto &column : columns) {
                cells.push_back(experience->getFloatOpt(row, column).value_or(0.0f));
            }
            rows.push_back(std::move(cells));
        }
        _killExperience = std::move(rows);
    }
    if (auto npcs = twoDas.get("npc")) {
        std::vector<std::optional<float>> rows;
        for (int row = 0; row < npcs->getRowCount(); ++row) {
            rows.push_back(npcs->getFloatOpt(row, "percentxp"));
        }
        _experiencePercents = std::move(rows);
    }
    if (auto soundSets = twoDas.get("appearancesndset")) {
        std::vector<CreatureSoundSet> rows;
        for (int row = 0; row < soundSets->getRowCount(); ++row) {
            auto sound = [&](const char *column) { return boost::to_lower_copy(soundSets->getString(row, column)); };
            CreatureSoundSet set;
            set.weapon = soundSets->getInt(row, "weapon", 0);
            set.armorType = sound("armortype");
            set.fallDirt = sound("falldirt");
            set.fallHard = sound("fallhard");
            set.fallMetal = sound("fallmetal");
            set.fallWater = sound("fallwater");
            rows.push_back(std::move(set));
        }
        _creatureSoundSets = std::move(rows);
    }
    if (auto weaponSounds = twoDas.get("weaponsounds")) {
        std::vector<std::unordered_map<std::string, std::string>> rows;
        for (int row = 0; row < weaponSounds->getRowCount(); ++row) {
            std::unordered_map<std::string, std::string> sounds;
            for (const auto &column : weaponSounds->columns()) {
                auto sound = boost::to_lower_copy(weaponSounds->getString(row, column));
                if (!sound.empty()) sounds.emplace(column, std::move(sound));
            }
            rows.push_back(std::move(sounds));
        }
        _weaponSounds = std::move(rows);
    }
    if (auto objectSounds = twoDas.get("placeableobjsnds")) {
        std::vector<ObjectSoundRow> rows;
        for (int row = 0; row < objectSounds->getRowCount(); ++row) {
            rows.push_back({boost::to_lower_copy(objectSounds->getString(row, "armortype")),
                            objectSounds->getString(row, "opened")});
        }
        _objectSounds = std::move(rows);
    }

    if (auto bodyBags = twoDas.get("bodybag")) {
        std::vector<BodyBagRow> rows;
        for (int row = 0; row < bodyBags->getRowCount(); ++row) {
            rows.push_back({bodyBags->getIntOpt(row, "appearance"),
                            bodyBags->getIntOpt(row, "name"),
                            bodyBags->getIntOpt(row, "corpse").value_or(0) != 0});
        }
        _bodyBags = std::move(rows);
    }
    if (auto traps = twoDas.get("traps")) {
        std::vector<TrapRow> rows;
        for (int row = 0; row < traps->getRowCount(); ++row) {
            rows.push_back({boost::to_lower_copy(traps->getString(row, "trapscript")),
                            {boost::to_lower_copy(traps->getString(row, "resref")),
                             traps->getInt(row, "setdc", 0),
                             {traps->getInt(row, "detectdcmod", 0), traps->getInt(row, "disarmdcmod", 0)}}});
        }
        _traps = std::move(rows);
    }
    if (auto fractions = twoDas.get("fractionalcr")) {
        std::vector<float> rows;
        for (int row = 0; row < fractions->getRowCount(); ++row) {
            rows.push_back(fractions->getFloat(row, "min"));
        }
        _fractionalChallengeMinimums = std::move(rows);
    }

    if (auto regeneration = twoDas.get("regeneration")) {
        RegenerationRates rates;
        for (int row = 0; row < 2; ++row) {
            rates.healthRegen[row] = regeneration->getFloat(row, "healthregen", 0.0f);
            rates.forceRegen[row] = regeneration->getFloat(row, "forceregen", 0.0f);
        }
        rates.timePerHitPoint = regenerationValue(*regeneration, "TimePerHP");
        rates.inCombatHitPointBase = regenerationValue(*regeneration, "InCombatHPBase");
        rates.outOfCombatHitPointBase = regenerationValue(*regeneration, "OutOfCombatHPBase");
        rates.constitutionBonus = regenerationValue(*regeneration, "ConModBonus");
        rates.skillRankBonus = regenerationValue(*regeneration, "SkillRankBons");
        rates.forcePointTime = regenerationValue(*regeneration, "FPRegenTime");
        rates.inCombatForcePointBase = regenerationValue(*regeneration, "InCombatFPBase");
        rates.outOfCombatForcePointBase = regenerationValue(*regeneration, "OutOfCombatFPBase");
        rates.wisdomBonus = regenerationValue(*regeneration, "WisModBonus");
        _regeneration = rates;
    }
    if (auto races = twoDas.get("racialtypes")) {
        const auto &columns = races->columns();
        std::vector<RacialType> rows;
        for (int row = 0; row < races->getRowCount(); ++row) {
            RacialType race;
            for (int ability = 0; ability < 6; ++ability) {
                const char *column = kRacialAdjustmentColumns[ability];
                if (std::find(columns.begin(), columns.end(), column) == columns.end()) continue;
                // A blank cell is zero.
                const auto byte = static_cast<uint8_t>(races->getInt(row, column, 0));
                race.abilityAdjustments[ability] = byte < 128u ? static_cast<int>(byte) : static_cast<int>(byte) - 256;
            }
            race.names = {races->getInt(row, "name", -1),
                          races->getInt(row, "convername", -1),
                          races->getInt(row, "convernamelower", -1)};
            rows.push_back(race);
        }
        _racialTypes = std::move(rows);
    }
    if (auto ranges = twoDas.get("ranges")) {
        std::vector<RangeRow> rows;
        for (int row = 0; row < ranges->getRowCount(); ++row) {
            rows.push_back({ranges->getFloatOpt(row, "primaryrange"), ranges->getFloatOpt(row, "secondaryrange")});
        }
        _ranges = std::move(rows);
    }
    if (auto icons = twoDas.get("effecticon")) {
        std::vector<EffectIconRow> rows;
        for (int row = 0; row < icons->getRowCount(); ++row) {
            rows.push_back({!icons->getString(row, "iconresref").empty(),
                            icons->getIntOpt(row, "priority"),
                            icons->getBoolOpt(row, "good"),
                            icons->getIntOpt(row, "namestrref")});
        }
        _effectIcons = std::move(rows);
    }

    if (auto values = twoDas.get("itemvalue")) {
        for (int row = 0; row < values->getRowCount(); ++row) {
            _itemValueLimits.push_back(values->getInt(row, "maxsingleitemvalue", 0));
        }
    }
    if (auto tags = twoDas.get("iprp_pc")) {
        std::vector<std::string> rows;
        for (int row = 0; row < tags->getRowCount(); ++row) {
            rows.push_back(tags->getString(row, "expectedtag"));
        }
        _playerCharacterLimitTags = std::move(rows);
    }
    if (auto effects = twoDas.get("videoeffects")) {
        std::vector<VideoEffectRow> rows;
        for (int row = 0; row < effects->getRowCount(); ++row) {
            VideoEffectRow effect;
            effect.scanNoise = effects->getInt(row, "enablescannoise");
            effect.saturation = effects->getInt(row, "enablesaturation");
            effect.clairvoyance = effects->getInt(row, "enableclairvoyance");
            effect.clairvoyanceFull = effects->getInt(row, "enableclairvoyancefull");
            effect.forceSight = effects->getInt(row, "enableforcesight");
            effect.fury = effects->getInt(row, "enablefury");
            effect.modulation = {effects->getFloat(row, "modulationred"),
                                 effects->getFloat(row, "modulationgreen"),
                                 effects->getFloat(row, "modulationblue")};
            effect.modulationPC = {effects->getFloat(row, "modulationred_pc"),
                                   effects->getFloat(row, "modulationgreen_pc"),
                                   effects->getFloat(row, "modulationblue_pc")};
            effect.saturationAmount = effects->getFloat(row, "saturation");
            effect.saturationAmountPC = effects->getFloat(row, "saturation_pc");
            rows.push_back(effect);
        }
        _videoEffects = std::move(rows);
    }
    if (auto tokens = twoDas.get("stringtokens")) {
        const auto &columns = tokens->columns();
        const auto tokenColumn = std::find(columns.begin(), columns.end(), "token");
        if (tokenColumn != columns.end()) {
            const auto index = static_cast<size_t>(tokenColumn - columns.begin());
            for (int row = 0; row < tokens->getRowCount(); ++row) {
                StringTokenRow token;
                for (int i = 0; i < static_cast<int>(token.strRefs.size()); ++i) {
                    token.strRefs[i] = tokens->getInt(row, "strref" + std::to_string(i + 1), -1);
                }
                token.defaultStrRef = tokens->getInt(row, "default", -1);
                token.action = tokens->getInt(row, "actioncode", -1);
                _stringTokens.emplace_back(tokens->rows()[row].values[index], token);
            }
        }
    }
    if (auto tutorials = twoDas.get("tutorial")) {
        _tutorials = readTutorials(*tutorials);
    }
    if (auto feedback = twoDas.get("feedbacktext")) {
        std::vector<std::optional<int>> rows;
        for (int row = 0; row < feedback->getRowCount(); ++row) {
            rows.push_back(feedback->getIntOpt(row, "strref"));
        }
        _feedbackTexts = std::move(rows);
    }
    if (auto scripts = twoDas.get("aiscripts")) {
        for (int row = 0; row < scripts->getRowCount(); ++row) {
            _aiScripts.push_back({scripts->getIntOpt(row, "NAME_STRREF"), scripts->getIntOpt(row, "AISTATE")});
        }
    }
    if (auto animations = twoDas.get("dialoganimations")) {
        _dialogAnimationRows = readDialogAnimationRows(*animations);
    }
    if (auto animations = twoDas.get("animations")) {
        _animationDialogRows = readDialogAnimationRows(*animations);
    }
    if (auto difficulties = twoDas.get("encdifficulty")) {
        std::vector<float> rows;
        for (int row = 0; row < difficulties->getRowCount(); ++row) {
            rows.push_back(difficulties->getFloat(row, "value"));
        }
        _encounterDifficulties = std::move(rows);
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

TrapDCModifiers CombatTables::trapDCModifiers(int trapType) const {
    const auto &rows = required(_traps, "traps");
    return trapType >= 0 && trapType < static_cast<int>(rows.size()) ? rows[trapType].mine.modifiers : TrapDCModifiers();
}

const std::vector<int> &CombatTables::deathKeptEffectTypes() const {
    return required(_deathKeptEffectTypes, "removefxondeath");
}

std::vector<uint32_t> CombatTables::experienceThresholds(int count) const {
    const auto &rows = required(_experienceThresholds, "exptable");
    if (static_cast<int>(rows.size()) < count) {
        throw ValidationException("Missing required exptable thresholds");
    }
    std::vector<uint32_t> result;
    result.reserve(count);
    for (int row = 0; row < count; ++row) {
        if (!rows[row]) {
            throw ValidationException("Missing required exptable XP at row " + std::to_string(row));
        }
        result.push_back(*rows[row]);
    }
    return result;
}

float CombatTables::killExperience(int row, int column) const {
    const auto &rows = required(_killExperience, "xptable");
    if (row < 0 || row >= static_cast<int>(rows.size())) {
        return 0.0f;
    }
    const auto &cells = rows[row];
    return column >= 0 && column < static_cast<int>(cells.size()) ? cells[column] : 0.0f;
}

std::optional<float> CombatTables::experiencePercent(int row) const {
    const auto &rows = required(_experiencePercents, "npc");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : std::nullopt;
}

const CreatureSoundSet &CombatTables::creatureSoundSet(int row) const {
    const auto &rows = required(_creatureSoundSets, "appearancesndset");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : kBlankSoundSet;
}

const std::string &CombatTables::weaponSound(int row, const std::string &column) const {
    const auto &rows = required(_weaponSounds, "weaponsounds");
    if (row < 0 || row >= static_cast<int>(rows.size())) {
        return kBlank;
    }
    auto sound = rows[row].find(column);
    return sound != rows[row].end() ? sound->second : kBlank;
}

const std::string &CombatTables::objectArmorType(int soundType) const {
    const auto &rows = required(_objectSounds, "placeableobjsnds");
    return soundType >= 0 && soundType < static_cast<int>(rows.size()) ? rows[soundType].armorType : kBlank;
}

BodyBagRow CombatTables::bodyBag(int row) const {
    const auto &rows = required(_bodyBags, "bodybag");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : BodyBagRow();
}

std::optional<std::string> CombatTables::trapScript(int row) const {
    if (!_traps) {
        return std::nullopt;
    }
    return row >= 0 && row < static_cast<int>(_traps->size()) ? (*_traps)[row].script : std::string();
}

float CombatTables::fractionalChallengeMinimum(int row) const {
    const auto &rows = required(_fractionalChallengeMinimums, "fractionalcr");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : 0.0f;
}

const RegenerationRates &CombatTables::regeneration() const {
    return required(_regeneration, "regeneration");
}

int CombatTables::racialAbilityAdjustment(int race, Ability ability) const {
    const auto &rows = required(_racialTypes, "racialtypes");
    const int index = static_cast<int>(ability);
    assert(index >= 0 && index < 6 && "Invalid racial ability selector");
    validateTwoDARowIndex("racialtypes", race, static_cast<int>(rows.size()));
    const auto &adjustment = rows[race].abilityAdjustments[index];
    if (!adjustment) {
        throw ValidationException(std::string("racialtypes.2da missing column: ") + kRacialAdjustmentColumns[index]);
    }
    return *adjustment;
}

RacialTypeNames CombatTables::racialTypeNames(int race) const {
    const auto &rows = required(_racialTypes, "racialtypes");
    return race >= 0 && race < static_cast<int>(rows.size()) ? rows[race].names : RacialTypeNames();
}

const RangeRow *CombatTables::findRange(int row) const {
    if (!_ranges || row < 0 || row >= static_cast<int>(_ranges->size())) {
        return nullptr;
    }
    return &(*_ranges)[row];
}

std::optional<RangeRow> CombatTables::range(int row) const {
    if (!_ranges) {
        return std::nullopt;
    }
    const auto *found = findRange(row);
    return found ? *found : RangeRow();
}

EffectIconRow CombatTables::effectIcon(int row) const {
    const auto &rows = required(_effectIcons, "effecticon");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : EffectIconRow();
}

MineRow CombatTables::mine(int trapType) const {
    if (!_traps || trapType < 0 || trapType >= static_cast<int>(_traps->size())) {
        return MineRow();
    }
    return (*_traps)[trapType].mine;
}

const std::vector<int> &CombatTables::itemValueLimits() const {
    return _itemValueLimits;
}

std::optional<std::string> CombatTables::playerCharacterLimitTag(int subtype) const {
    if (!_playerCharacterLimitTags) {
        return std::nullopt;
    }
    const auto &rows = *_playerCharacterLimitTags;
    return subtype >= 0 && subtype < static_cast<int>(rows.size()) ? rows[subtype] : std::string();
}

const CostTable *CombatTables::findCostTableAt(int index) const {
    if (!_costTableNames || index < 0 || index >= static_cast<int>(_costTableNames->size())) {
        return nullptr;
    }
    auto table = _costTables.find((*_costTableNames)[index]);
    return table != _costTables.end() ? &table->second : nullptr;
}

const std::string &CombatTables::objectOpenedSound(int soundType) const {
    const auto &rows = required(_objectSounds, "placeableobjsnds");
    return soundType >= 0 && soundType < static_cast<int>(rows.size()) ? rows[soundType].opened : kBlank;
}

std::optional<VideoEffectRow> CombatTables::videoEffect(int row) const {
    if (!_videoEffects) {
        return std::nullopt;
    }
    return row >= 0 && row < static_cast<int>(_videoEffects->size()) ? (*_videoEffects)[row] : VideoEffectRow();
}

const StringTokenRow *CombatTables::findStringToken(const std::string &name) const {
    for (const auto &[token, row] : _stringTokens) {
        if (token == name) return &row;
    }
    return nullptr;
}

const TutorialRow *CombatTables::findTutorial(int row) const {
    if (!_tutorials || row < 0 || row >= static_cast<int>(_tutorials->size())) {
        return nullptr;
    }
    return &(*_tutorials)[row];
}

std::optional<int> CombatTables::feedbackText(int row) const {
    const auto &rows = required(_feedbackTexts, "feedbacktext");
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row] : std::nullopt;
}

const std::vector<AIScriptRow> &CombatTables::aiScripts() const {
    return _aiScripts;
}

const std::vector<bool> *CombatTables::dialogAnimationRows() const {
    return _dialogAnimationRows ? &*_dialogAnimationRows : nullptr;
}

const std::vector<bool> *CombatTables::animationDialogRows() const {
    return _animationDialogRows ? &*_animationDialogRows : nullptr;
}

const std::vector<float> *CombatTables::encounterDifficulties() const {
    return _encounterDifficulties ? &*_encounterDifficulties : nullptr;
}

} // namespace game

} // namespace reone
