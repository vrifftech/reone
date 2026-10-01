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

#include "types.h"

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

/** A trap type's detect and disarm DC modifiers (traps.2da). A blank cell, or a row outside it, is zero. */
struct TrapDCModifiers {
    int detect {0};
    int disarm {0};
};

/**
 * A creature sound set (appearancesndset.2da). Sound names are lower case; a
 * blank or missing cell is zero or empty.
 */
struct CreatureSoundSet {
    // The weaponsounds.2da row of the creature's own weapons.
    int weapon {0};
    std::string armorType;
    std::string fallDirt;
    std::string fallHard;
    std::string fallMetal;
    std::string fallWater;
};

/** A body bag (bodybag.2da). A blank cell, or a row outside it, has no value. */
struct BodyBagRow {
    std::optional<int> appearance;
    std::optional<int> nameStrRef;
    bool corpse {false};
};

/**
 * Vitality and Force regeneration (regeneration.2da). KotOR reads its in and
 * out of combat rows; TSL reads its values by label. A blank or missing cell,
 * row or label is zero.
 */
struct RegenerationRates {
    // Percent of the maximum per second: in combat, then out of combat.
    std::array<float, 2> healthRegen {};
    std::array<float, 2> forceRegen {};
    float timePerHitPoint {0.0f};
    float inCombatHitPointBase {0.0f};
    float outOfCombatHitPointBase {0.0f};
    float constitutionBonus {0.0f};
    float skillRankBonus {0.0f};
    float forcePointTime {0.0f};
    float inCombatForcePointBase {0.0f};
    float outOfCombatForcePointBase {0.0f};
    float wisdomBonus {0.0f};
};

/** A race's name text (racialtypes.2da). A blank cell, or a race outside the table, is -1. */
struct RacialTypeNames {
    int name {-1};
    int converName {-1};
    int converNameLower {-1};
};

/** A ranges.2da row. A blank cell has no value. */
struct RangeRow {
    std::optional<float> primary;
    std::optional<float> secondary;
};

/** An effect icon (effecticon.2da). A blank cell, or a row outside the table, has no value. */
struct EffectIconRow {
    bool hasIcon {false};
    std::optional<int> priority;
    std::optional<bool> good;
    std::optional<int> nameStrRef;
};

/** A mine type's item, setting DC and DC modifiers (traps.2da). A blank cell, or a missing table or row, is empty or zero. */
struct MineRow {
    std::string itemResRef;
    int setDC {0};
    TrapDCModifiers modifiers;
};

/**
 * A video effect (videoeffects.2da). TSL's colour columns end in _pc. A blank
 * cell, or a row outside the table, is zero.
 */
struct VideoEffectRow {
    int scanNoise {0};
    int saturation {0};
    int clairvoyance {0};
    int clairvoyanceFull {0};
    int forceSight {0};
    int fury {0};
    std::array<float, 3> modulation {};
    std::array<float, 3> modulationPC {};
    float saturationAmount {0.0f};
    float saturationAmountPC {0.0f};
};

/** A dialogue text token (stringtokens.2da). A blank cell is -1. */
struct StringTokenRow {
    std::array<int, 4> strRefs {-1, -1, -1, -1};
    int defaultStrRef {-1};
    int action {-1};
};

/**
 * A tutorial window (tutorial.2da): the message of each page, KotOR's and
 * TSL's, and the icon. A blank cell has no value.
 */
struct TutorialRow {
    std::vector<std::optional<int>> messages;
    std::vector<std::optional<int>> pcMessages;
    std::string icon;
};

/** A party AI behaviour (aiscripts.2da). A blank cell has no value. */
struct AIScriptRow {
    std::optional<int> nameStrRef;
    std::optional<int> state;
};

/**
 * The tables the rules and the presentation read while the game plays, parsed
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
    virtual TrapDCModifiers trapDCModifiers(int trapType) const = 0;

    /** The effect types that survive death (removefxondeath.2da). */
    virtual const std::vector<int> &deathKeptEffectTypes() const = 0;
    /** The experience each of the first count levels needs (exptable.2da). */
    virtual std::vector<uint32_t> experienceThresholds(int count) const = 0;
    /** A kill's experience by level row and table column (xptable.2da); zero outside it. */
    virtual float killExperience(int row, int column) const = 0;
    /** An npc.2da percentxp cell; nothing when it is blank or outside the table. */
    virtual std::optional<float> experiencePercent(int row) const = 0;
    /** A blank sound set outside the table. */
    virtual const CreatureSoundSet &creatureSoundSet(int row) const = 0;
    /** A weaponsounds.2da sound in lower case; empty when blank or outside the table. */
    virtual const std::string &weaponSound(int row, const std::string &column) const = 0;
    /** The lower-case armour type of an object sound type (placeableobjsnds.2da). */
    virtual const std::string &objectArmorType(int soundType) const = 0;

    virtual BodyBagRow bodyBag(int row) const = 0;
    /** The lower-case script of a mine type, or nothing when the table is missing. */
    virtual std::optional<std::string> trapScript(int row) const = 0;
    /** The least challenge rating of a fractionalcr row. */
    virtual float fractionalChallengeMinimum(int row) const = 0;

    virtual const RegenerationRates &regeneration() const = 0;
    /**
     * A race's adjustment to an ability; fails for a race outside
     * racialtypes.2da or a table without the ability's column.
     */
    virtual int racialAbilityAdjustment(int race, Ability ability) const = 0;
    virtual RacialTypeNames racialTypeNames(int race) const = 0;
    /** The row, or nothing when the table or the row is missing. */
    virtual const RangeRow *findRange(int row) const = 0;
    /** Nothing when the table is missing; a missing row reads as blank. */
    virtual std::optional<RangeRow> range(int row) const = 0;
    virtual EffectIconRow effectIcon(int row) const = 0;

    virtual MineRow mine(int trapType) const = 0;
    /** The most a single item may cost for each level (itemvalue.2da); none when the table is missing. */
    virtual const std::vector<int> &itemValueLimits() const = 0;
    /** The tag a player character limit expects (iprp_pc.2da); nothing when the table is missing. */
    virtual std::optional<std::string> playerCharacterLimitTag(int subtype) const = 0;
    /** The cost table iprp_costtable names at index, or nothing when either table is missing. */
    virtual const CostTable *findCostTableAt(int index) const = 0;
    /** The sound an object sound type plays when it opens (placeableobjsnds.2da). */
    virtual const std::string &objectOpenedSound(int soundType) const = 0;
    /** Nothing when the table is missing; a missing row reads as blank. */
    virtual std::optional<VideoEffectRow> videoEffect(int row) const = 0;
    /** The first row whose token cell is the name, or nothing. */
    virtual const StringTokenRow *findStringToken(const std::string &name) const = 0;
    /** The row, or nothing when the table or the row is missing. */
    virtual const TutorialRow *findTutorial(int row) const = 0;
    /** A feedbacktext.2da string; nothing when it is blank or outside the table. */
    virtual std::optional<int> feedbackText(int row) const = 0;
    /** None when the table is missing. */
    virtual const std::vector<AIScriptRow> &aiScripts() const = 0;
    /**
     * Whether each row of dialoganimations.2da, and of animations.2da, is a
     * dialogue animation; nothing when the table is missing.
     */
    virtual const std::vector<bool> *dialogAnimationRows() const = 0;
    virtual const std::vector<bool> *animationDialogRows() const = 0;
    /** The encdifficulty.2da values; nothing when the table is missing. */
    virtual const std::vector<float> *encounterDifficulties() const = 0;
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
    TrapDCModifiers trapDCModifiers(int trapType) const override;

    const std::vector<int> &deathKeptEffectTypes() const override;
    std::vector<uint32_t> experienceThresholds(int count) const override;
    float killExperience(int row, int column) const override;
    std::optional<float> experiencePercent(int row) const override;
    const CreatureSoundSet &creatureSoundSet(int row) const override;
    const std::string &weaponSound(int row, const std::string &column) const override;
    const std::string &objectArmorType(int soundType) const override;

    BodyBagRow bodyBag(int row) const override;
    std::optional<std::string> trapScript(int row) const override;
    float fractionalChallengeMinimum(int row) const override;

    const RegenerationRates &regeneration() const override;
    int racialAbilityAdjustment(int race, Ability ability) const override;
    RacialTypeNames racialTypeNames(int race) const override;
    const RangeRow *findRange(int row) const override;
    std::optional<RangeRow> range(int row) const override;
    EffectIconRow effectIcon(int row) const override;

    MineRow mine(int trapType) const override;
    const std::vector<int> &itemValueLimits() const override;
    std::optional<std::string> playerCharacterLimitTag(int subtype) const override;
    const CostTable *findCostTableAt(int index) const override;
    const std::string &objectOpenedSound(int soundType) const override;
    std::optional<VideoEffectRow> videoEffect(int row) const override;
    const StringTokenRow *findStringToken(const std::string &name) const override;
    const TutorialRow *findTutorial(int row) const override;
    std::optional<int> feedbackText(int row) const override;
    const std::vector<AIScriptRow> &aiScripts() const override;
    const std::vector<bool> *dialogAnimationRows() const override;
    const std::vector<bool> *animationDialogRows() const override;
    const std::vector<float> *encounterDifficulties() const override;

private:
    struct RacialType {
        // As signed bytes; nothing when the table has no such column.
        std::array<std::optional<int>, 6> abilityAdjustments;
        RacialTypeNames names;
    };

    struct TrapRow {
        std::string script;
        MineRow mine;
    };

    struct ObjectSoundRow {
        std::string armorType;
        std::string opened;
    };

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
    std::optional<std::vector<int>> _deathKeptEffectTypes;
    // Blank thresholds have no value.
    std::optional<std::vector<std::optional<uint32_t>>> _experienceThresholds;
    std::optional<std::vector<std::vector<float>>> _killExperience;
    std::optional<std::vector<std::optional<float>>> _experiencePercents;
    std::optional<std::vector<CreatureSoundSet>> _creatureSoundSets;
    // By column; blank cells are left out.
    std::optional<std::vector<std::unordered_map<std::string, std::string>>> _weaponSounds;
    std::optional<std::vector<ObjectSoundRow>> _objectSounds;
    std::optional<std::vector<BodyBagRow>> _bodyBags;
    std::optional<std::vector<TrapRow>> _traps;
    std::optional<std::vector<float>> _fractionalChallengeMinimums;
    std::optional<RegenerationRates> _regeneration;
    std::optional<std::vector<RacialType>> _racialTypes;
    std::optional<std::vector<RangeRow>> _ranges;
    std::optional<std::vector<EffectIconRow>> _effectIcons;
    std::vector<int> _itemValueLimits;
    std::optional<std::vector<std::string>> _playerCharacterLimitTags;
    std::optional<std::vector<VideoEffectRow>> _videoEffects;
    // With the raw token cell of each row.
    std::vector<std::pair<std::string, StringTokenRow>> _stringTokens;
    std::optional<std::vector<TutorialRow>> _tutorials;
    std::optional<std::vector<std::optional<int>>> _feedbackTexts;
    std::vector<AIScriptRow> _aiScripts;
    std::optional<std::vector<bool>> _dialogAnimationRows;
    std::optional<std::vector<bool>> _animationDialogRows;
    std::optional<std::vector<float>> _encounterDifficulties;
};

} // namespace game

} // namespace reone
