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

#include "reone/game/itemdescription.h"

#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/item.h"
#include "reone/game/party.h"
#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/strings.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/format.hpp>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

using namespace reone::resource;

namespace reone {

namespace game {

namespace {

// Interface strings of the property lines.
constexpr int kNoDescriptionStrRef = 32172;
constexpr int kAttributeRequirementsStrRef = 117597;
constexpr int kFirstAbilityRequirementStrRef = 117591;
constexpr int kFeatRequirementsStrRef = 38536;
constexpr int kGenderRequirementsStrRef = 47995;
constexpr int kSubraceRequirementsStrRef = 48013;
constexpr int kCharacterRequirementsStrRef = 48026;
constexpr int kPartyMemberRequirementsStrRef = 130590;
constexpr int kBaoDurNameStrRef = 102045;
constexpr int kDamageStrRef = 31385;
constexpr int kVersusStrRef = 1395;
constexpr int kPhysicalDamageStrRef = 38552;
constexpr int kRangeStrRef = 38565;
constexpr int kCriticalThreatStrRef = 38571;
constexpr int kOnHitStrRef = 38583;
constexpr int kChanceStrRef = 38574;
constexpr int kDurationStrRef = 47878;
constexpr int kTSLDifficultyClassStrRef = 48756;
constexpr int kK1DifficultyClassStrRef = 49135;
constexpr int kOffHandStrRef = 42145;
constexpr int kAttackModifierStrRef = 38566;
constexpr int kUnarmedAttackSuffixStrRef = 113136;
constexpr int kDefenseStrRef = 38593;
constexpr int kMaxDexterityBonusStrRef = 42144;
constexpr int kSingleUseStrRef = 783;
constexpr int kUsesStrRef = 41899;
constexpr int kUnlimitedUsesStrRef = 41900;
constexpr int kSavesStrRef = 41935;
constexpr int kSkillsStrRef = 41936;
constexpr int kVulnerabilityStrRef = 41898;
constexpr int kForceResistanceStrRef = 41930;
constexpr int kUseLimitationStrRef = 41934;

// The damage kinds of a weapon, named in this order.
constexpr std::pair<int, int> kDamageTypeNames[] {
    {0x0007, 38552},
    {0x0008, 38553},
    {0x0010, 38554},
    {0x0020, 38555},
    {0x0040, 38556},
    {0x0080, 38557},
    {0x0100, 38558},
    {0x0200, 38559},
    {0x0400, 38560},
    {0x0800, 38561},
    {0x1000, 38562},
    {0x2000, 41903}};

// Item types that list no properties: lightsaber crystals, grenades and
// (in TSL) rockets.
constexpr int kCrystalItemType = 46;
constexpr int kGrenadeItemType = 6;
constexpr int kRocketItemType = 49;
// Droid plating shows no Dexterity limit.
constexpr int kDroidPlatingItemType = 15;
// TSL gauntlets list damage like a weapon.
constexpr int kGauntletsBaseItem = 45;
// Items that only Bao-Dur may use.
constexpr int kBaoDurItems[] {35, 36, 37, 97, 98, 99, 102};
constexpr int kBaoDurNpc = 1;

// Of the armour proficiencies an item asks for, only the highest is listed.
constexpr int kHeavyArmourFeat = 4;
constexpr int kLightArmourFeat = 5;
constexpr int kMediumArmourFeat = 6;

// Cost tables of the attribute limits and of the on-hit difficulty class.
constexpr int kAttributeCostTable = 26;
constexpr int kOnHitDifficultyCostTable = 25;

// Property values that ask for no cost or parameter text.
constexpr int kNoCostValue = 0xffff;
constexpr int kNoParamValue = 0xff;
constexpr int kNoSubtype = 0xffff;
// A Dexterity limit of this value means none.
constexpr int kNoDexterityLimit = 0xff;

struct PropertyStrings {
    std::string name;
    std::string subtype;
    std::string cost;
    std::string param;
};

class ItemDescriptionBuilder {
public:
    ItemDescriptionBuilder(const Item &item, const Game &game, ServicesView &services) :
        _item(item),
        _game(game),
        _services(services),
        _tsl(game.isTSL()) {
        for (const auto &property : item.properties()) {
            if (item.isPropertyActive(property)) _active.push_back(&property);
        }
    }

    std::string build() {
        const int itemType = _item.itemType();
        if (itemType != kCrystalItemType && itemType != kGrenadeItemType &&
            !(_tsl && itemType == kRocketItemType)) {
            if (_tsl) addAttributeRequirements();
            addFeatRequirements();
            if (_tsl) {
                addNamedRequirements(ItemProperty::LimitUseByGender, kGenderRequirementsStrRef, "gender");
                addNamedRequirements(ItemProperty::LimitUseBySubrace, kSubraceRequirementsStrRef, "subrace");
                addCharacterRequirements();
                addPartyMemberRequirements();
            }
            const bool gauntlets = _tsl && _item.baseItemType() == kGauntletsBaseItem;
            if (_item.weaponType() != WeaponType::None || gauntlets) {
                addDamage();
                addRange();
                if (!gauntlets) addCriticalThreat();
                addOnHit();
                addWeaponSize();
            }
            addAttackModifier();
            addDefense();
            addUses();
            addImmunities();
            addSaves();
            addSkills();
            addDeflection();
            addOtherProperties();
        }
        std::string description(_item.descIdentified());
        if (description.empty()) description = gui(kNoDescriptionStrRef);
        return _text + description;
    }

private:
    const Item &_item;
    const Game &_game;
    ServicesView &_services;
    bool _tsl;
    // Active properties, in list order.
    std::vector<const Item::PropertyEntry *> _active;
    std::string _text;
    // The number read from a cost name; a name that holds none leaves the
    // previous one.
    int _scanned {0};

    std::string gui(int strRef) const {
        return _game.getInterfaceText(strRef);
    }

    std::shared_ptr<TwoDA> requiredTable(const std::string &resRef) const {
        return getRequiredTwoDA(_services.resource.twoDas, boost::to_lower_copy(resRef));
    }

    std::shared_ptr<TwoDA> costTable(int index) const {
        return requiredTable(requiredTable("iprp_costtable")->getString(index, "name"));
    }

    std::shared_ptr<TwoDA> paramTable(int index) const {
        return requiredTable(requiredTable("iprp_paramtable")->getString(index, "tableresref"));
    }

    // A blank cell leaves the value as it was.
    static bool readInt(const TwoDA &table, int row, const std::string &column, int &value) {
        auto cell = table.getIntOpt(row, column);
        if (!cell) return false;
        value = *cell;
        return true;
    }

    static int type(const Item::PropertyEntry &property) {
        return static_cast<int>(property.propertyName);
    }

    bool hasProperty(ItemProperty kind) const {
        return std::any_of(_active.begin(), _active.end(), [kind](const Item::PropertyEntry *property) {
            return type(*property) == static_cast<int>(kind);
        });
    }

    // Another active property of the same kind and subtype has a higher value.
    bool isOutvalued(const Item::PropertyEntry &property) const {
        return std::any_of(_active.begin(), _active.end(), [&property](const Item::PropertyEntry *other) {
            return other->propertyName == property.propertyName && other->subtype == property.subtype &&
                   other->costValue > property.costValue;
        });
    }

    // The name of a property and the names of its subtype, cost and
    // parameter; kNoCostValue and kNoParamValue ask for no cost or parameter.
    PropertyStrings propertyStrings(int kind, int subtype, int costValue, int paramValue) const {
        PropertyStrings strings;
        auto definitions = requiredTable("itempropdef");
        int strRef = -1;
        if (!readInt(*definitions, kind, "name", strRef)) return strings;
        if (strRef != -1) strings.name = gui(strRef);
        std::shared_ptr<TwoDA> subtypes;
        const std::string subtypeResRef(definitions->getString(kind, "subtyperesref"));
        if (!subtypeResRef.empty()) {
            subtypes = _services.resource.twoDas.get(boost::to_lower_copy(subtypeResRef));
            if (!subtypes) return strings;
            readInt(*subtypes, subtype, "name", strRef);
            strings.subtype = gui(strRef);
        }
        int index = 0;
        if (costValue != kNoCostValue && readInt(*definitions, kind, "costtableresref", index)) {
            readInt(*costTable(index), costValue, "name", strRef);
            if (strRef > 0) strings.cost = gui(strRef);
        }
        if (paramValue != kNoParamValue &&
            (readInt(*definitions, kind, "param1resref", index) ||
             (subtypes && readInt(*subtypes, subtype, "param1resref", index)))) {
            readInt(*paramTable(index), paramValue, "name", strRef);
            strings.param = gui(strRef);
        }
        return strings;
    }

    // The damage a damage cost value adds, as {minimum, maximum}.
    std::pair<int, int> damageRange(int costValue) const {
        auto costs = requiredTable("iprp_damagecost");
        int dice = 0;
        if (!readInt(*costs, costValue, "numdice", dice)) return {costValue, costValue};
        if (dice == 0) return {1, costValue};
        int die = 0;
        readInt(*costs, costValue, "die", die);
        return {dice, dice * die};
    }

    // The chance, duration in rounds and difficulty class of an on-hit
    // effect; a blank cell carries over the value read before it.
    void readChanceDurationDifficulty(int costValue, int paramValue, int &chance, int &duration, int &difficulty) const {
        auto durations = requiredTable("iprp_onhitdur");
        int value = difficulty;
        readInt(*durations, paramValue, "effectchance", value);
        chance = value;
        readInt(*durations, paramValue, "durationrounds", value);
        duration = value;
        readInt(*costTable(kOnHitDifficultyCostTable), costValue, "value", value);
        difficulty = value;
    }

    std::string damageTypes(int flags) const {
        std::string result;
        for (const auto &[mask, strRef] : kDamageTypeNames) {
            if ((flags & mask) == 0) continue;
            if (!result.empty()) result += ", ";
            result += gui(strRef);
        }
        return result;
    }

    static bool hasDamageType(int flags, int subtype) {
        return ((flags >> (subtype & 31)) & 1) != 0;
    }

    int difficultyClassStrRef() const {
        return _tsl ? kTSLDifficultyClassStrRef : kK1DifficultyClassStrRef;
    }

    // Requirements

    void addAttributeRequirements() {
        if (!hasProperty(ItemProperty::LimitUseByAttribute)) return;
        auto values = costTable(kAttributeCostTable);
        bool listed = false;
        int value = 0;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::LimitUseByAttribute)) continue;
            if (!listed) {
                _text += gui(kAttributeRequirementsStrRef) + ":\n";
                listed = true;
            }
            if (property->subtype <= 5) _text += gui(kFirstAbilityRequirementStrRef + property->subtype) + ": ";
            readInt(*values, property->costValue, "value", value);
            _text += std::to_string(value) + "\n";
        }
        if (listed) _text += "\n";
    }

    // A feat name's colon reads " - ".
    std::string featName(int feat) const {
        std::string name(gui(requiredTable("feat")->getInt(feat, "name", -1)));
        const size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(0, colon) + " - " + name.substr(colon + 1);
        return name;
    }

    void addFeatRequirements() {
        std::vector<int> feats;
        int armour = kLightArmourFeat;
        for (auto feat : _item.requiredFeats()) {
            feats.push_back(static_cast<int>(feat));
            if (feats.back() == kMediumArmourFeat && armour == kLightArmourFeat) armour = kMediumArmourFeat;
            if (feats.back() == kHeavyArmourFeat && (armour == kLightArmourFeat || armour == kMediumArmourFeat)) {
                armour = kHeavyArmourFeat;
            }
        }
        bool listed = false;
        const auto listFeat = [this, &listed](int feat) {
            if (!listed) {
                _text += gui(kFeatRequirementsStrRef) + ":\n";
                listed = true;
            }
            _text += featName(feat) + "\n";
        };
        for (int feat : feats) {
            if (feat == 0) continue;
            if (feat >= kHeavyArmourFeat && feat <= kMediumArmourFeat && feat != armour) continue;
            listFeat(feat);
        }
        for (const auto *property : _active) {
            if (type(*property) == static_cast<int>(ItemProperty::UseLimitationFeat) && property->subtype != 0) {
                listFeat(property->subtype);
            }
        }
        if (listed) _text += "\n";
    }

    void addNamedRequirements(ItemProperty kind, int headerStrRef, const std::string &tableName) {
        if (!hasProperty(kind)) return;
        auto names = requiredTable(tableName);
        bool listed = false;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(kind)) continue;
            if (!listed) {
                _text += gui(headerStrRef) + ":\n";
                listed = true;
            }
            _text += gui(names->getInt(property->subtype, "name", -1)) + "\n";
        }
        if (listed) _text += "\n";
    }

    // Subtype 0 names the player character.
    void addCharacterRequirements() {
        if (!hasProperty(ItemProperty::LimitUseByPc)) return;
        auto characters = requiredTable("iprp_pc");
        bool listed = false;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::LimitUseByPc)) continue;
            if (!listed) {
                _text += gui(kCharacterRequirementsStrRef) + ":\n";
                listed = true;
            }
            _text += (property->subtype != 0 ? gui(characters->getInt(property->subtype, "name", -1))
                                             : _game.party().playerCharacterName()) +
                     "\n";
        }
        if (listed) _text += "\n";
    }

    // Once Bao-Dur has joined, the items only he may use say so.
    void addPartyMemberRequirements() {
        if (_game.party().persistedState().influence[kBaoDurNpc] == -1) return;
        const int baseItem = _item.baseItemType();
        if (std::find(std::begin(kBaoDurItems), std::end(kBaoDurItems), baseItem) == std::end(kBaoDurItems)) return;
        _text += gui(kPartyMemberRequirementsStrRef) + ":\n" + gui(kBaoDurNameStrRef) + "\n\n";
    }

    // Weapon lines

    void addDamage() {
        if (hasProperty(ItemProperty::NoDamage)) {
            _text += gui(kDamageStrRef) + ": 1\n";
        } else {
            // Enhancement and damage of the weapon's own kinds raise its
            // range, damage penalties lower it; the minimum is at least 1.
            const int flags = _item.damageFlags();
            int minimum = _item.numDice();
            int maximum = _item.numDice() * _item.dieToRoll();
            const std::string kinds(damageTypes(flags));
            for (const auto *property : _active) {
                switch (static_cast<ItemProperty>(type(*property))) {
                case ItemProperty::DecreasedDamage:
                    minimum -= property->costValue;
                    maximum -= property->costValue;
                    break;
                case ItemProperty::DamageBonus:
                    if (hasDamageType(flags, property->subtype & 0xff)) {
                        const auto [low, high] = damageRange(property->costValue);
                        minimum += low;
                        maximum += high;
                    }
                    break;
                case ItemProperty::EnhancementBonus:
                    minimum += property->costValue;
                    maximum += property->costValue;
                    break;
                default:
                    break;
                }
            }
            if (minimum <= 0) minimum = 1;
            if (maximum != 0) {
                _text += gui(kDamageStrRef) + ": ";
                if (_tsl && _item.baseItemType() == kGauntletsBaseItem) {
                    if (!kinds.empty()) _text += kinds + " ";
                    _text += "+" + std::to_string(maximum) + "\n\n";
                } else {
                    if (!kinds.empty()) _text += kinds + ", ";
                    _text += str(boost::format("%d-%d\n\n") % minimum % maximum);
                }
            }
        }
        addGroupDamage(ItemProperty::DamageBonusVsAlignmentGroup, "\n");
        addGroupDamage(ItemProperty::DamageBonusVsRacialGroup, "\n\n");
        addDamageReduction();
        addExtraDamage();
    }

    static std::string bonusRange(int minimum, int maximum) {
        return minimum == maximum ? str(boost::format("+%d") % maximum) : str(boost::format("+%d-%d") % minimum % maximum);
    }

    // The bonuses against groups are summed into one line, named for the
    // last of them.
    void addGroupDamage(ItemProperty kind, const std::string &end) {
        const Item::PropertyEntry *last = nullptr;
        int minimum = 0;
        int maximum = 0;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(kind)) continue;
            const auto [low, high] = damageRange(property->costValue);
            minimum += low;
            maximum += high;
            last = property;
        }
        if (!last) return;
        const auto strings = propertyStrings(static_cast<int>(kind), last->subtype, kNoCostValue, last->paramValue);
        _text += strings.param + ": " + bonusRange(minimum, maximum) + " " + gui(kVersusStrRef) + " " + strings.subtype + end;
    }

    void addDamageReduction() {
        int amount = 0;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::DamageReduction) || isOutvalued(*property)) continue;
            const auto strings = propertyStrings(type(*property), property->subtype, property->costValue, kNoParamValue);
            std::sscanf(strings.subtype.c_str(), "%*s +%d", &amount);
            _text += strings.name + ": +" + std::to_string(amount) + " (" + strings.cost + ")\n\n";
        }
    }

    // Damage of kinds the weapon does not deal, one entry per kind; the
    // physical kinds count as one.
    void addExtraDamage() {
        const int flags = _item.damageFlags();
        const int damageBonus = static_cast<int>(ItemProperty::DamageBonus);
        bool listed = false;
        for (size_t i = 0; i < _active.size(); ++i) {
            const auto &property = *_active[i];
            if (type(property) != damageBonus || hasDamageType(flags, property.subtype)) continue;
            const auto strings = propertyStrings(damageBonus, property.subtype, property.costValue, kNoParamValue);
            if (!listed) _text += strings.name + ": ";
            bool listedBefore = false;
            int minimum = 0;
            int maximum = 0;
            for (size_t j = 0; j < _active.size(); ++j) {
                const auto &other = *_active[j];
                if (type(other) != damageBonus) continue;
                if (other.subtype != property.subtype && (property.subtype > 2 || other.subtype > 2)) continue;
                if (j < i) {
                    listedBefore = true;
                    break;
                }
                const auto [low, high] = damageRange(other.costValue);
                minimum += low;
                maximum += high;
            }
            if (listedBefore) continue;
            const std::string kind(property.subtype <= 2 ? gui(kPhysicalDamageStrRef) : strings.subtype);
            if (listed) _text += ", ";
            _text += bonusRange(minimum, maximum) + " " + kind;
            listed = true;
        }
        if (listed) _text += "\n\n";
    }

    void addRange() {
        auto baseItems = requiredTable("baseitems");
        if (baseItems->getInt(_item.baseItemType(), "rangedweapon", 0) == 0) return;
        _text += str(boost::format("%s: %dm\n") % gui(kRangeStrRef) % static_cast<int>(_item.attackRange())) + "\n";
    }

    // Keen doubles the threat range; massive criticals add their damage.
    void addCriticalThreat() {
        _text += gui(kCriticalThreatStrRef) + ":\n";
        int threat = _item.criticalThreat();
        if (hasProperty(ItemProperty::Keen)) threat <<= 1;
        int minimum = 0;
        int maximum = 0;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::MassiveCriticals)) continue;
            const auto [low, high] = damageRange(property->costValue);
            minimum += low;
            maximum += high;
        }
        if (minimum != 0 || maximum != 0) {
            _text += str(boost::format("%d-20,x%d +%d-%d\n") % (21 - threat) % _item.criticalHitMultiplier() % minimum % maximum);
        } else {
            _text += str(boost::format("%d-20,x%d\n") % (21 - threat) % _item.criticalHitMultiplier());
        }
        _text += "\n";
    }

    void addOnHit() {
        const int onHitKind = static_cast<int>(ItemProperty::OnHitProperties);
        // TSL adds knockdown to the listed effects.
        const int lastEffect = _tsl ? 11 : 10;
        int chance = 0;
        int duration = 0;
        int difficulty = 0;
        for (const auto *property : _active) {
            if (type(*property) != onHitKind || property->subtype > lastEffect) continue;
            const auto effect = propertyStrings(onHitKind, property->subtype, kNoCostValue, kNoParamValue);
            const std::string prefix(gui(kOnHitStrRef) + ": ");
            switch (property->subtype) {
            case 6:  // ability drain
            case 8:  // slay racial group
            case 9: {  // slay alignment group
                _text += prefix + effect.subtype + " ";
                const auto strings = propertyStrings(onHitKind, property->subtype, property->costValue, property->paramValue);
                _text += strings.param + " " + strings.cost + "\n\n";
                break;
            }
            case 7: {  // poison
                _text += prefix;
                const auto strings = propertyStrings(onHitKind, property->subtype, property->costValue, property->paramValue);
                readInt(*requiredTable("poison"), property->paramValue, "dc_save", difficulty);
                _text += strings.param + " " + gui(difficultyClassStrRef()) + " " + std::to_string(difficulty) + "\n\n";
                break;
            }
            case 10: {  // instant death
                _text += prefix + effect.subtype + " ";
                const auto strings = propertyStrings(onHitKind, property->subtype, property->costValue, property->paramValue);
                _text += ", " + strings.cost + "\n\n";
                break;
            }
            case 11:  // knockdown
                _text += prefix + effect.subtype + " ";
                readChanceDurationDifficulty(property->costValue, property->paramValue, chance, duration, difficulty);
                _text += str(boost::format("%s %d\n") % gui(difficultyClassStrRef()) % difficulty);
                break;
            default:
                _text += prefix + effect.subtype + " ";
                readChanceDurationDifficulty(property->costValue, property->paramValue, chance, duration, difficulty);
                duration *= 3;
                _text += str(boost::format("%d%% %s, %d %s, %s %d\n") % chance % gui(kChanceStrRef) % duration %
                             gui(kDurationStrRef) % gui(difficultyClassStrRef()) % difficulty);
                break;
            }
        }
    }

    void addWeaponSize() {
        if (_item.weaponSize() == CreatureSize::Small) _text += gui(kOffHandStrRef) + "\n\n";
    }

    // Attack and defence

    void addAttackModifier() {
        int total = 0;
        for (const auto *property : _active) {
            switch (static_cast<ItemProperty>(type(*property))) {
            case ItemProperty::EnhancementBonus:
            case ItemProperty::AttackBonus:
                total += property->costValue;
                break;
            case ItemProperty::AttackPenalty:
            case ItemProperty::DecreasedAttackModifier:
                total -= property->costValue;
                break;
            default:
                break;
            }
        }
        // TSL gauntlets say the modifier is for unarmed attacks.
        const std::string suffix(_tsl && _item.baseItemType() == kGauntletsBaseItem
                                     ? _services.resource.strings.getText(kUnarmedAttackSuffixStrRef)
                                     : std::string());
        if (total > 0) {
            _text += str(boost::format("%s: +%d%s\n") % gui(kAttackModifierStrRef) % total % suffix);
        } else if (total < 0) {
            _text += str(boost::format("%s: %d%s\n") % gui(kAttackModifierStrRef) % total % suffix);
        }
        static constexpr ItemProperty kVersusKinds[] {
            ItemProperty::EnhancementBonusVsAlignmentGroup,
            ItemProperty::EnhancementBonusVsRacialGroup,
            ItemProperty::AttackBonusVsAlignmentGroup,
            ItemProperty::AttackBonusVsRacialGroup};
        if (std::any_of(std::begin(kVersusKinds), std::end(kVersusKinds), [this](ItemProperty kind) { return hasProperty(kind); })) {
            if (total == 0) _text += gui(kAttackModifierStrRef) + ": ";
            for (auto kind : kVersusKinds) addAttackBonuses(static_cast<int>(kind));
        }
        _text += "\n";
    }

    // One line per subtype; the running sum carries over from one subtype
    // to the next.
    void addAttackBonuses(int kind) {
        int sum = 0;
        for (size_t i = 0; i < _active.size(); ++i) {
            const auto &property = *_active[i];
            if (type(property) != kind) continue;
            bool listedBefore = false;
            for (size_t j = 0; j < _active.size(); ++j) {
                const auto &other = *_active[j];
                if (type(other) != kind || other.subtype != property.subtype) continue;
                if (j < i) {
                    listedBefore = true;
                    break;
                }
                sum += other.costValue;
            }
            if (listedBefore) continue;
            const auto strings = propertyStrings(kind, property.subtype, property.costValue, kNoParamValue);
            _text += "+" + std::to_string(sum) + " " + gui(kVersusStrRef) + " " + strings.subtype + "\n";
        }
    }

    void addDefense() {
        // Armour bonuses stack unless the base item's defence is enchantment
        // of a kind, where only the highest of a subtype counts; the first
        // defence penalty counts when it is the highest of its subtype.
        int defense = _item.baseDefense();
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::AcBonus)) continue;
            if (_item.acBonusType() != ACBonus::Dodge && isOutvalued(*property)) continue;
            defense += property->costValue;
        }
        if (hasProperty(ItemProperty::DecreasedAc)) {
            const auto &properties = _item.properties();
            auto penalty = std::find_if(properties.begin(), properties.end(), [](const Item::PropertyEntry &property) {
                return type(property) == static_cast<int>(ItemProperty::DecreasedAc);
            });
            if (_item.isPropertyActive(*penalty) && !isOutvalued(*penalty)) defense -= penalty->costValue;
        }
        if (defense != 0) _text += gui(kDefenseStrRef) + ": " + str(boost::format("%d\n") % defense) + "\n";

        if (_item.itemType() != kDroidPlatingItemType) {
            const int limit = _item.maxDexterityBonus() & 0xff;
            int bonus = 0;
            if (_tsl) {
                for (const auto *property : _active) {
                    if (type(*property) == static_cast<int>(ItemProperty::MaxDexterityBonus)) bonus += property->costValue;
                }
            }
            const int value = ((limit == kNoDexterityLimit && bonus != 0) ? bonus : bonus + limit) & 0xff;
            if (value != kNoDexterityLimit) _text += gui(kMaxDexterityBonusStrRef) + ": +" + std::to_string(value) + "\n\n";
        }

        for (const auto *property : _active) {
            const auto kind = static_cast<ItemProperty>(type(*property));
            const bool versus = kind == ItemProperty::AcBonusVsAlignmentGroup ||
                                kind == ItemProperty::AcBonusVsDamageType ||
                                kind == ItemProperty::AcBonusVsRacialGroup;
            const bool protection = kind == ItemProperty::ImmunityDamageType || kind == ItemProperty::DamageResistance;
            if ((!versus && !protection) || isOutvalued(*property)) continue;
            const auto strings = propertyStrings(type(*property), property->subtype, property->costValue, kNoParamValue);
            if (versus) {
                _text += "+" + strings.cost + " " + gui(kVersusStrRef) + " " + strings.subtype + "\n\n";
            } else {
                _text += strings.name + ": " + strings.cost + " " + gui(kVersusStrRef) + " " + strings.subtype + "\n\n";
            }
        }
    }

    // Miscellaneous lines

    // The uses a charged or daily cast-spell property has left.
    int usesLeft(const Item::PropertyEntry &property) const {
        const int charges = _item.charges();
        const int mode = property.costValue;
        if (!property.usable) return 0;
        if (mode == 1) return _item.stackSize();
        if (mode >= 2 && mode <= 5) return charges > 0 ? charges / (7 - mode) : 0;
        if (mode == 6) return charges > 0 ? charges : 0;
        if (mode >= 8 && mode <= 12) return property.usesPerDay;
        if (mode >= 14 && mode <= 18) return 1;
        return 0;
    }

    void addUses() {
        // A mode outside the table keeps the previous property's number of uses.
        int uses = 0;
        const int maximum = _item.maxCharges();
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::ActivateItem)) continue;
            const int mode = property->costValue;
            if (mode == 1) {
                _text += gui(kSingleUseStrRef) + "\n\n";
                continue;
            }
            if (mode >= 2 && mode <= 4) {
                uses = maximum / (7 - mode);
            } else if (mode == 5 || mode == 6) {
                uses = maximum;
            } else if (mode == 7 || mode == 13) {
                uses = 0;
            } else if (mode >= 8 && mode <= 12) {
                uses = mode - 7;
            } else if (mode >= 14 && mode <= 18) {
                uses = 1;
            }
            _text += gui(kUsesStrRef) + ": ";
            if (uses != 0) {
                _text += str(boost::format("%d/%d\n") % usesLeft(*property) % uses) + "\n";
            } else {
                _text += gui(kUnlimitedUsesStrRef) + "\n\n";
            }
        }
    }

    void addImmunities() {
        if (!hasProperty(ItemProperty::Immunity)) return;
        int count = 0;
        for (const auto *property : _active) {
            if (type(*property) != static_cast<int>(ItemProperty::Immunity)) continue;
            const auto strings = propertyStrings(type(*property), property->subtype, kNoCostValue, kNoParamValue);
            if (count == 0) _text += strings.name + ": ";
            if (count > 0) _text += ", ";
            _text += strings.subtype;
            ++count;
        }
        _text += "\n\n";
    }

    void addSaves() {
        if (!hasProperty(ItemProperty::ImprovedSavingThrow) && !hasProperty(ItemProperty::DecreasedSavingThrows) &&
            !hasProperty(ItemProperty::ImprovedSavingThrowSpecific) && !hasProperty(ItemProperty::DecreasedSavingThrowsSpecific)) {
            return;
        }
        _text += gui(kSavesStrRef) + ": ";
        int count = 0;
        for (const auto *property : _active) {
            if (isOutvalued(*property)) continue;
            const auto kind = static_cast<ItemProperty>(type(*property));
            const bool bonus = kind == ItemProperty::ImprovedSavingThrow || kind == ItemProperty::ImprovedSavingThrowSpecific;
            const bool penalty = kind == ItemProperty::DecreasedSavingThrows || kind == ItemProperty::DecreasedSavingThrowsSpecific;
            if (!bonus && !penalty) continue;
            const auto strings = propertyStrings(type(*property), property->subtype, property->costValue, kNoParamValue);
            if (count > 0) _text += ", ";
            if (bonus) {
                _text += strings.subtype + " +" + strings.cost;
            } else {
                std::sscanf(strings.cost.c_str(), "%*s -%d", &_scanned);
                _text += strings.subtype + " -" + std::to_string(_scanned);
            }
            ++count;
        }
        _text += "\n\n";
    }

    void addSkills() {
        if (!hasProperty(ItemProperty::SkillBonus) && !hasProperty(ItemProperty::DecreasedSkillModifier)) return;
        _text += gui(kSkillsStrRef) + ": ";
        int count = 0;
        for (const auto *property : _active) {
            const auto kind = static_cast<ItemProperty>(type(*property));
            if ((kind != ItemProperty::SkillBonus && kind != ItemProperty::DecreasedSkillModifier) || isOutvalued(*property)) continue;
            const auto strings = propertyStrings(type(*property), property->subtype, property->costValue, kNoParamValue);
            if (count > 0) _text += ", ";
            _text += strings.subtype + " " + strings.cost;
            ++count;
        }
        _text += "\n\n";
    }

    // The deflection bonuses and penalties are summed; the line is titled
    // with the part of the first property's name before its colon.
    void addDeflection() {
        if (!hasProperty(ItemProperty::BlasterBoltDeflectIncrease) && !hasProperty(ItemProperty::BlasterBoltDeflectDecrease)) return;
        bool titled = false;
        int total = 0;
        for (const auto *property : _active) {
            const auto kind = static_cast<ItemProperty>(type(*property));
            if (kind != ItemProperty::BlasterBoltDeflectIncrease && kind != ItemProperty::BlasterBoltDeflectDecrease) continue;
            if (!titled) {
                const auto strings = propertyStrings(type(*property), property->subtype, property->costValue, kNoParamValue);
                const size_t colon = strings.name.find(':');
                if (colon != std::string::npos) _text += strings.name.substr(0, colon) + ": " + " ";
                titled = true;
            }
            total += kind == ItemProperty::BlasterBoltDeflectIncrease ? property->costValue : -property->costValue;
        }
        if (total > 0) _text += "+";
        _text += std::to_string(total) + "\n\n";
    }

    void addOtherProperties() {
        for (const auto *property : _active) {
            const int kind = type(*property);
            switch (static_cast<ItemProperty>(kind)) {
            case ItemProperty::AbilityBonus:
            case ItemProperty::DecreasedAbilityScore: {
                if (isOutvalued(*property)) break;
                const auto strings = propertyStrings(kind, property->subtype, property->costValue, kNoParamValue);
                _text += strings.subtype + ": " + strings.cost + "\n\n";
                break;
            }
            case ItemProperty::BonusFeat: {
                const auto strings = propertyStrings(kind, property->subtype, kNoCostValue, kNoParamValue);
                _text += strings.name + ": " + strings.subtype + "\n\n";
                break;
            }
            case ItemProperty::DamageVulnerability: {
                if (isOutvalued(*property)) break;
                const auto strings = propertyStrings(kind, property->subtype, kNoCostValue, kNoParamValue);
                int chance = 0;
                int duration = 0;
                int difficulty = 0;
                readChanceDurationDifficulty(property->costValue, property->paramValue, chance, duration, difficulty);
                duration *= 3;
                _text += gui(kVulnerabilityStrRef) + ": " +
                         str(boost::format("%d%% %s, %d%s ") % chance % gui(kChanceStrRef) % duration % gui(kDurationStrRef)) +
                         gui(kVersusStrRef) + " " + strings.subtype + "\n\n";
                break;
            }
            case ItemProperty::ImprovedForceResistance: {
                if (isOutvalued(*property)) break;
                const auto strings = propertyStrings(kind, property->subtype, property->costValue, kNoParamValue);
                std::sscanf(strings.cost.c_str(), "%*s %d", &_scanned);
                _text += gui(kForceResistanceStrRef) + ": +" + std::to_string(_scanned) + "\n\n";
                break;
            }
            case ItemProperty::Light:
            case ItemProperty::TrueSeeing:
            case ItemProperty::FreedomOfMovement:
                _text += propertyStrings(kind, kNoSubtype, kNoCostValue, kNoParamValue).name + "\n\n";
                break;
            case ItemProperty::UseLimitationAlignmentGroup:
            case ItemProperty::UseLimitationClass:
                _text += gui(kUseLimitationStrRef) + ": " +
                         propertyStrings(kind, property->subtype, kNoCostValue, kNoParamValue).subtype + "\n\n";
                break;
            case ItemProperty::Regeneration:
            case ItemProperty::RegenerationForcePoints: {
                if (isOutvalued(*property)) break;
                const auto strings = propertyStrings(kind, property->subtype, property->costValue, kNoParamValue);
                _text += strings.name + ": " + strings.cost + "\n\n";
                break;
            }
            default:
                break;
            }
        }
    }
};

} // namespace

std::string buildItemDescription(const Item &item, const Game &game, ServicesView &services) {
    return ItemDescriptionBuilder(item, game, services).build();
}

} // namespace game

} // namespace reone
