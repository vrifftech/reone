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

#include "reone/game/object/item.h"

#include "reone/audio/di/services.h"
#include "reone/audio/mixer.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/twodautil.h"
#include "reone/graphics/di/services.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/provider/audioclips.h"
#include "reone/resource/provider/gffs.h"
#include "reone/resource/provider/models.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/resources.h"
#include "reone/resource/strings.h"

#include <algorithm>
#include "reone/system/exception/validation.h"

using namespace reone::audio;
using namespace reone::graphics;
using namespace reone::resource;

namespace reone {

namespace game {

void Item::loadFromBlueprint(const std::string &resRef) {
    std::shared_ptr<Gff> uti(_services.resource.gffs.get(resRef, ResType::Uti));
    if (uti) {
        deserialize(*uti, SerializedIdentityContext::templateResource(resRef));
        return;
    }
}

void Item::deserialize(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    std::string ref;
    if (!identityContext.isSerializedState() &&
        gff.readResRef(ref, "EquippedRes")) {
        if (auto uti = _services.resource.gffs.get(ref, ResType::Uti)) {
            deserializeAll(*uti, SerializedIdentityContext::templateResource(ref));
        }
    }

    if (!identityContext.isSerializedState() &&
        gff.readResRef(ref, "InventoryRes")) {
        if (auto uti = _services.resource.gffs.get(ref, ResType::Uti)) {
            deserializeAll(*uti, SerializedIdentityContext::templateResource(ref));
        }
    }

    if (!identityContext.isSerializedState() &&
        gff.readResRef(ref, "TemplateResRef")) {
        if (auto uti = _services.resource.gffs.get(ref, ResType::Uti)) {
            deserializeAll(*uti, SerializedIdentityContext::templateResource(ref));
        }
    }

    deserializeAll(gff, identityContext);
}

void Item::deserializeAll(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    Object::deserialize(gff, identityContext);

    gff.readLocString(_localizedName, "LocalizedName", _services.resource.strings);
    gff.readLocString(_description, "Description", _services.resource.strings);
    gff.readLocString(_descIdentified, "DescIdentified", _services.resource.strings);

    gff.readByte(_charges, "Charges");
    gff.readDword(_cost, "Cost");
    gff.readDword(_addCost, "AddCost");
    gff.readBool(_stolen, "Stolen");
    gff.readWord(_stackSize, "StackSize");
    gff.readDword(_upgrades, "Upgrades");

    gff.readBool(_identified, "Identified");
    gff.readByte(_modelVariation, "ModelVariation");
    gff.readByte(_textureVariation, "TextureVar");
    gff.readByte(_bodyVariation, "BodyVariation");
    gff.readBool(_dropable, "Dropable");

    deserializeProperties(gff);
    deserializeBase(gff);

    loadAmmunitionType();
    updateTransform();
}

void Item::deserializeProperties(const resource::Gff &gff) {
    if (gff.has("PropertiesList")) {
        _properties.clear();
        _activateSpell.reset();
        _disguiseAppearance = -1;
    }
    for (const auto &prop : gff.getList("PropertiesList")) {
        PropertyEntry entry;
        prop->readByte(entry.chanceAppear, "ChanceAppear");
        prop->readByte(entry.costTable, "CostTable");
        prop->readWord(entry.costValue, "CostValue");
        prop->readByte(entry.paramTable, "Param1");
        prop->readByte(entry.paramValue, "Param1Value");
        prop->readWord(entry.subtype, "Subtype");
        prop->readByte(entry.upgradeType, "UpgradeType");
        prop->readByte(entry.usesPerDay, "UsesPerDay");
        prop->readBool(entry.usable, "Useable");
        prop->readDword64(entry.cooldownUntil, "ReoneUseUntil");

        if (prop->readWord(entry.propertyName, "PropertyName")) {
            switch (static_cast<ItemProperty>(entry.propertyName)) {
            case ItemProperty::ActivateItem: {
                _activateSpell = static_cast<SpellType>(entry.subtype);

                if (entry.costValue >= 8 && entry.costValue <= 12 && entry.usesPerDay == 0xff)
                    entry.usesPerDay = static_cast<uint8_t>(entry.costValue - 7);
                break;
            }
            case ItemProperty::Disguise:
                _disguiseAppearance = entry.subtype;
                break;
            default:
                break;
            }
        }

        _properties.push_back(entry);
    }
}

void Item::deserializeBase(const resource::Gff &gff) {
    if (!gff.readInt(_baseItem, "BaseItem")) {
        return;
    }

    auto baseItems = getRequiredTwoDA(_services.resource.twoDas, "baseitems");
    _itemType = baseItems->getInt(_baseItem, "itemtype", 0);
    _attackRange = baseItems->getFloat(_baseItem, "maxattackrange", 0.0f);
    _baseDefense = baseItems->getInt(_baseItem, "baseac", 0);
    _criticalHitMultiplier = baseItems->getInt(_baseItem, "crithitmult", 0);
    _criticalThreat = baseItems->getInt(_baseItem, "critthreat", 0);
    _damageFlags = baseItems->getInt(_baseItem, "damageflags", 0);
    _dieToRoll = baseItems->getInt(_baseItem, "dietoroll", 0);
    _maxDexterityBonus = baseItems->getInt(_baseItem, "dexbonus", -1);
    _equipableSlots = static_cast<uint32_t>(
        baseItems->getInt(_baseItem, "equipableslots", 0));
    _itemClass = boost::to_lower_copy(
        baseItems->getString(_baseItem, "itemclass"));
    _numDice = baseItems->getInt(_baseItem, "numdice", 0);
    _acBonusType = static_cast<ACBonus>(baseItems->getInt(
        _baseItem,
        "ac_enchant",
        static_cast<int>(ACBonus::Invalid)));
    _weaponType = static_cast<WeaponType>(
        baseItems->getInt(_baseItem, "weapontype", 0));
    _weaponWield = static_cast<WeaponWield>(
        baseItems->getInt(_baseItem, "weaponwield", 0));
    _weaponSize = static_cast<CreatureSize>(
        baseItems->getInt(_baseItem, "weaponsize", 0));
    _weaponFocusFeat = static_cast<FeatType>(
        baseItems->getInt(_baseItem, "focfeat", 0));
    _weaponSpecializationFeat = static_cast<FeatType>(
        baseItems->getInt(_baseItem, "specfeat", 0));
    _maxStackSize = static_cast<uint16_t>(std::clamp(
        baseItems->getInt(
            _baseItem,
            "maxstack",
            std::numeric_limits<uint16_t>::max()),
        1,
        static_cast<int>(std::numeric_limits<uint16_t>::max())));

    _poweredItem = baseItems->getInt(_baseItem, "powereditem") != 0;
    if (_poweredItem) {
        auto loadSound = [this, &baseItems](const char *column) {
            auto resRef = boost::to_lower_copy(baseItems->getString(_baseItem, column));
            return resRef.empty() ? nullptr : _services.resource.audioClips.get(resRef);
        };
        _powerUpSound = loadSound("powerupsnd");
        _powerDownSound = loadSound("powerdownsnd");
        _poweredSound = loadSound("poweredsnd");
    }

    std::string iconResRef;
    if (isEquippable(InventorySlots::body)) {
        _baseBodyVariation = boost::to_lower_copy(baseItems->getString(_baseItem, "bodyvar"));
        iconResRef = str(boost::format("i%s_%03d") % _itemClass % (int)_textureVariation);
    } else if (isEquippable(InventorySlots::rightWeapon)) {
        iconResRef = str(boost::format("i%s_%03d") % _itemClass % (int)_modelVariation);
    } else {
        iconResRef = str(boost::format("i%s_%03d") % _itemClass % (int)_modelVariation);
    }
    _icon = _services.resource.textures.get(iconResRef, TextureUsage::GUI);
    if (!_icon && isEquippable(InventorySlots::body)) {
        // Some body items (e.g. disguises) key the inventory icon on ModelVariation
        // rather than TextureVar; fall back to it when the primary icon is missing.
        iconResRef = str(boost::format("i%s_%03d") % _itemClass % (int)_modelVariation);
        _icon = _services.resource.textures.get(iconResRef, TextureUsage::GUI);
    }
}

void Item::loadAmmunitionType() {
    auto baseItems = getRequiredTwoDA(_services.resource.twoDas, "baseitems");

    int ammunitionIdx = baseItems->getInt(_baseItem, "ammunitiontype", 0);
    if (ammunitionIdx < 1) {
        return;
    }

    auto twoDa = getRequiredTwoDA(_services.resource.twoDas, "ammunitiontypes");
    _ammunitionType = std::make_shared<Item::AmmunitionType>();
    _ammunitionType->shieldHit = twoDa->getInt(ammunitionIdx, "shieldhit", 0) != 0;
    _ammunitionType->model = _services.resource.models.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "model")));
    _ammunitionType->muzzleFlash = _services.resource.models.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "muzzleflash")));
    _ammunitionType->shotSound1 = _services.resource.audioClips.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "shotsound0")));
    _ammunitionType->shotSound2 = _services.resource.audioClips.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "shotsound1")));
    _ammunitionType->impactSound1 = _services.resource.audioClips.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "impactsound0")));
    _ammunitionType->impactSound2 = _services.resource.audioClips.get(boost::to_lower_copy(twoDa->getString(ammunitionIdx, "impactsound1")));
}

int Item::maxDexterityBonusAdjustment() const {
    if (!_game.isTSL()) {
        return 0;
    }

    int result = 0;
    for (const auto &property : _properties) {
        if (property.propertyName !=
                static_cast<uint16_t>(ItemProperty::MaxDexterityBonus) ||
            !isPropertyActive(property)) {
            continue;
        }

        // TSL's ApplyMaxDexBonus adds the property's raw CostValue directly
        // to the item's retained maximum-Dexterity adjustment.
        result += property.costValue;
    }
    return result;
}

void Item::clone(const Item &from) {
    // Object
    _tag = from._tag;
    _name = from._name;
    // END Object

    // Serializable
    _baseItem = from._baseItem;
    _localizedName = from._localizedName;
    _description = from._description;
    _descIdentified = from._descIdentified;
    _charges = from._charges;
    _cost = from._cost;
    _addCost = from._addCost;
    _stolen = from._stolen;
    _stackSize = from._stackSize;
    _upgrades = from._upgrades;
    _identified = from._identified;
    _modelVariation = from._modelVariation;
    _bodyVariation = from._bodyVariation;
    _textureVariation = from._textureVariation;
    _dropable = from._dropable;
    // END Serializable

    _baseBodyVariation = from._baseBodyVariation;
    _itemClass = from._itemClass;
    _maxStackSize = from._maxStackSize;

    _icon = from._icon;
    _equipableSlots = from._equipableSlots;
    _attackRange = from._attackRange;
    _numDice = from._numDice;
    _dieToRoll = from._dieToRoll;
    _damageFlags = from._damageFlags;
    _weaponType = from._weaponType;
    _weaponWield = from._weaponWield;
    _weaponSize = from._weaponSize;

    _equipped = from._equipped;
    _ammunitionType = from._ammunitionType;
    _poweredItem = from._poweredItem;
    _isPowered = from._isPowered;
    _powerUpSound = from._powerUpSound;
    _powerDownSound = from._powerDownSound;
    _poweredSound = from._poweredSound;
    _poweredAudioSource = from._poweredAudioSource;

    _criticalThreat = from._criticalThreat;
    _criticalHitMultiplier = from._criticalHitMultiplier;
    _weaponFocusFeat = from._weaponFocusFeat;
    _weaponSpecializationFeat = from._weaponSpecializationFeat;
    _baseDefense = from._baseDefense;
    _maxDexterityBonus = from._maxDexterityBonus;
    _acBonusType = from._acBonusType;

    _activateSpell = from._activateSpell;
    _itemType = from._itemType;
    _disguiseAppearance = from._disguiseAppearance;
    _properties = from._properties;

    _audioSource = from._audioSource;
}

uint32_t Item::forceItemMask() const {
    if (_itemType >= 31 && _itemType <= 36) return uint32_t {1} << (_itemType - 31);
    return _itemType >= 39 && _itemType <= 41 ? 64 : 0;
}

std::optional<size_t> Item::spellProperty(SpellType spell) const {
    for (size_t i = 0; i < _properties.size(); ++i) {
        const auto &property = _properties[i];
        if (property.propertyName == static_cast<uint16_t>(ItemProperty::ActivateItem) &&
            property.subtype == static_cast<uint16_t>(spell) && canUseSpell(i)) return i;
    }
    return std::nullopt;
}

bool Item::canUseSpell(size_t index) const {
    if (index >= _properties.size() || _stackSize == 0) return false;
    const auto &property = _properties[index];
    if (property.propertyName != static_cast<uint16_t>(ItemProperty::ActivateItem) ||
        !isPropertyActive(property)) return false;
    const int mode = property.costValue;
    if (mode >= 14 && mode <= 18)
        return property.usable ||
            (property.cooldownUntil != 0 && _game.worldTimeMilliseconds() >= property.cooldownUntil);
    if (!property.usable) return false;
    if (mode == 1) return _stackSize > 0;
    if (mode >= 2 && mode <= 6) return static_cast<int>(_charges) >= 7 - mode;
    if (mode >= 8 && mode <= 12) return property.usesPerDay > 0;
    return mode == 7 || mode == 13;
}

bool Item::consumeSpellUse(size_t index) {
    if (!canUseSpell(index)) return false;
    auto &property = _properties[index];
    const int mode = property.costValue;
    if (mode == 1) {
        if (_stackSize > 0) --_stackSize;
        return _stackSize == 0;
    }
    if (mode >= 2 && mode <= 6) {
        _charges = static_cast<uint8_t>(std::max(0, static_cast<int>(_charges) - (7 - mode)));
        bool usableCharge = false;
        for (auto &other : _properties) {
            if (other.propertyName != static_cast<uint16_t>(ItemProperty::ActivateItem) ||
                other.costValue < 2 || other.costValue > 6) continue;
            other.usable = _charges >= 7 - other.costValue;
            usableCharge |= other.usable && isPropertyActive(other);
        }
        const bool exhausted = !usableCharge || (!property.usable &&
            (_itemType == 25 || _itemType == 45 || _itemType == 47));
        if (exhausted) _stackSize = 0;
        return exhausted;
    }
    if (mode >= 8 && mode <= 12) {
        if (property.usesPerDay > 0) --property.usesPerDay;
        property.usable = property.usesPerDay != 0;
    } else if (mode >= 14 && mode <= 18) {
        property.usable = false;
        property.cooldownUntil = _game.worldTimeMilliseconds() + uint64_t(mode - 13) * 60000;
    }
    return false;
}

void Item::update(float dt) {
}

void Item::playShotSound(int variant, glm::vec3 position) {
    if (!_ammunitionType) {
        return;
    }
    auto clip = variant == 1 ? _ammunitionType->shotSound2 : _ammunitionType->shotSound1;
    if (clip) {
        _audioSource = _services.audio.mixer.play(
            std::move(clip),
            AudioType::Sound,
            1.0f,
            false,
            std::move(position));
    }
}

void Item::playImpactSound(int variant, glm::vec3 position) {
    if (!_ammunitionType) {
        return;
    }
    auto clip = variant == 1 ? _ammunitionType->impactSound2 : _ammunitionType->impactSound1;
    if (clip) {
        _services.audio.mixer.play(
            std::move(clip),
            AudioType::Sound,
            1.0f,
            false,
            std::move(position));
    }
}

void Item::powerUp(glm::vec3 position) {
    if (!_poweredItem || _isPowered) {
        return;
    }
    _isPowered = true;
    if (_powerUpSound) {
        _services.audio.mixer.play(_powerUpSound, AudioType::Sound, 1.0f, false, position);
    }
    if (_poweredAudioSource) {
        _poweredAudioSource->stop();
    }
    if (_poweredSound) {
        _poweredAudioSource = _services.audio.mixer.play(_poweredSound, AudioType::Sound, 1.0f, true, position);
    }
}

void Item::powerDown(glm::vec3 position) {
    if (!_poweredItem || !_isPowered) {
        return;
    }
    _isPowered = false;
    if (_poweredAudioSource) {
        _poweredAudioSource->stop();
        _poweredAudioSource.reset();
    }
    if (_powerDownSound) {
        _services.audio.mixer.play(_powerDownSound, AudioType::Sound, 1.0f, false, position);
    }
}

void Item::updatePoweredSoundPosition(glm::vec3 position) {
    if (_poweredAudioSource) {
        _poweredAudioSource->setPosition(position);
    }
}

bool Item::isEquippable() const {
    return _equipableSlots != 0;
}

bool Item::isEquippable(int slot) const {
    return (_equipableSlots >> slot) & 1;
}

bool Item::isStackCompatibleWith(const Item &other) const {
    auto sameProperty = [](const PropertyEntry &lhs, const PropertyEntry &rhs) {
        return lhs.chanceAppear == rhs.chanceAppear &&
               lhs.costTable == rhs.costTable &&
               lhs.costValue == rhs.costValue &&
               lhs.paramTable == rhs.paramTable &&
               lhs.paramValue == rhs.paramValue &&
               lhs.propertyName == rhs.propertyName &&
               lhs.subtype == rhs.subtype &&
               lhs.upgradeType == rhs.upgradeType &&
               lhs.usesPerDay == rhs.usesPerDay && lhs.usable == rhs.usable &&
               lhs.cooldownUntil == rhs.cooldownUntil;
    };
    return _maxStackSize > 1 &&
           _baseItem == other._baseItem &&
           _plot == other._plot &&
           _charges == other._charges &&
           _upgrades == other._upgrades &&
           _stolen == other._stolen &&
           _modelVariation == other._modelVariation &&
           _bodyVariation == other._bodyVariation &&
           _textureVariation == other._textureVariation &&
           _tag == other._tag &&
           _localizedName.id() == other._localizedName.id() &&
           _localizedName.str() == other._localizedName.str() &&
           _properties.size() == other._properties.size() &&
           std::equal(
               _properties.begin(),
               _properties.end(),
               other._properties.begin(),
               sameProperty);
}

bool Item::mergeStackFrom(Item &other) {
    if (&other == this || !isStackCompatibleWith(other) ||
        _stackSize >= _maxStackSize || other._stackSize == 0) {
        return false;
    }
    const int moved = std::min<int>(
        _maxStackSize - _stackSize, other._stackSize);
    _stackSize += moved;
    other._stackSize -= moved;
    return other._stackSize == 0;
}

void Item::setDropable(bool dropable) {
    _dropable = dropable;
}

void Item::setStackSize(int stackSize) {
    _stackSize = stackSize;
}

void Item::setIdentified(bool value) {
    _identified = value;
}

void Item::setEquipped(bool equipped) {
    _equipped = equipped;
}

bool Item::isPropertyActive(const PropertyEntry &property) const {
    return isPropertyActive(_upgrades, property.upgradeType);
}

static SavingThrow getItemOnHitSavingThrow(ItemOnHitSubtype subtype) {
    switch (subtype) {
    case ItemOnHitSubtype::Sleep:
    case ItemOnHitSubtype::Stun:
    case ItemOnHitSubtype::Confusion:
    case ItemOnHitSubtype::Fear:
    case ItemOnHitSubtype::Slow:
        return SavingThrow::Will;
    case ItemOnHitSubtype::Paralyze:
    case ItemOnHitSubtype::SlayRG:
    case ItemOnHitSubtype::SlayAG:
    case ItemOnHitSubtype::Knockdown:
        return SavingThrow::Fortitude;
    case ItemOnHitSubtype::AbilityDrain:
        return SavingThrow::Reflex;
    case ItemOnHitSubtype::ItemPoison:
    case ItemOnHitSubtype::InstantDeath:
        return SavingThrow::None;
    }
    assert(false && "unsupported item on-hit subtype");
    return SavingThrow::None;
}

static SavingThrowType getItemOnHitSavingThrowType(ItemOnHitSubtype subtype) {
    switch (subtype) {
    case ItemOnHitSubtype::Sleep:
    case ItemOnHitSubtype::Stun:
    case ItemOnHitSubtype::Confusion:
        return SavingThrowType::MindAffecting;
    case ItemOnHitSubtype::Fear:
        return SavingThrowType::Fear;
    case ItemOnHitSubtype::Paralyze:
        return SavingThrowType::Paralysis;
    case ItemOnHitSubtype::SlayRG:
    case ItemOnHitSubtype::SlayAG:
        return SavingThrowType::Death;
    case ItemOnHitSubtype::Slow:
    case ItemOnHitSubtype::AbilityDrain:
    case ItemOnHitSubtype::ItemPoison:
    case ItemOnHitSubtype::InstantDeath:
    case ItemOnHitSubtype::Knockdown:
        return SavingThrowType::All;
    }
    assert(false && "unsupported item on-hit subtype");
    return SavingThrowType::All;
}

std::vector<ItemOnHitProperty> Item::itemOnHitProperties() const {
    static constexpr char kOnHitTable[] = "iprp_onhit";
    static constexpr char kOnHitDurationTable[] = "iprp_onhitdur";
    static constexpr char kOnHitDifficultyClassTable[] = "iprp_onhitdc";

    std::shared_ptr<resource::TwoDA> onHit;
    std::shared_ptr<resource::TwoDA> durations;
    std::shared_ptr<resource::TwoDA> difficultyClasses;
    std::vector<ItemOnHitProperty> result;
    ItemOnHitSelectionState selection;

    for (const PropertyEntry &property : _properties) {
        if (!isPropertyActive(property) ||
            property.propertyName !=
                static_cast<uint16_t>(ItemProperty::OnHitProperties) ||
            property.subtype >
                static_cast<uint16_t>(_game.isTSL() ? ItemOnHitSubtype::Knockdown :
                                      ItemOnHitSubtype::InstantDeath)) {
            continue;
        }

        if (!onHit) {
            onHit = getRequiredTwoDA(
                _services.resource.twoDas,
                kOnHitTable);
        }
        validateTwoDARow(*onHit, kOnHitTable, property.subtype);

        ItemOnHitProperty value;
        value.subtype = static_cast<ItemOnHitSubtype>(property.subtype);
        value.savingThrow = getItemOnHitSavingThrow(value.subtype);
        value.savingThrowType = getItemOnHitSavingThrowType(value.subtype);
        value.parameter = property.paramValue;

        selection.select(value.subtype);
        value.durationBranch = usesItemOnHitDurationHandler(value.subtype);
        const int parameterTable = onHit->getIntOpt(
            property.subtype, "param1resref").value_or(0);
        if (parameterTable == 1) {
            if (!durations) {
                durations = getRequiredTwoDA(
                    _services.resource.twoDas, kOnHitDurationTable);
            }
            selection.chance = 0;
            selection.rounds = 0;
            if (property.paramValue < durations->getRowCount()) {
                selection.chance = durations->getIntOpt(
                    property.paramValue, "effectchance").value_or(0);
                selection.rounds = durations->getIntOpt(
                    property.paramValue, "durationrounds").value_or(0);
            }
        }
        value.chance = selection.chance;
        value.duration = selection.rounds * 6.0f;

        if (!difficultyClasses) {
            difficultyClasses = getRequiredTwoDA(
                _services.resource.twoDas,
                kOnHitDifficultyClassTable);
        }
        value.difficultyClass = 20;
        if (property.costValue < difficultyClasses->getRowCount()) {
            value.difficultyClass = difficultyClasses->getIntOpt(
                property.costValue,
                "value").value_or(20);
        }

        // SavingThrowRoll receives an unsigned-short DC.
        value.difficultyClass = static_cast<uint16_t>(value.difficultyClass);
        result.push_back(value);
    }
    return result;
}

} // namespace game

} // namespace reone

namespace reone {
namespace game {
bool Item::isPropertyActive(uint32_t upgrades, uint8_t upgradeType) {
    if (upgradeType == 0xff) {
        return true;
    }
    // Select an upgrade bit using the low five bits of the selector.
    return (upgrades & (uint32_t {1} << (upgradeType & 31))) != 0;
}
} // namespace game
} // namespace reone
