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

#include "reone/game/equipmentrules.h"

#include "reone/game/game.h"
#include "reone/game/object.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/types.h"
#include "reone/game/d20/class.h"
#include "reone/game/d20/spell.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/2das.h"

#include <algorithm>

#include <boost/algorithm/string.hpp>

namespace reone {

namespace game {

bool isMainHandWeaponSlot(int slot) {
    return slot == InventorySlots::rightWeapon || slot == InventorySlots::rightWeapon2;
}

bool isOffHandWeaponSlot(int slot) {
    return slot == InventorySlots::leftWeapon || slot == InventorySlots::leftWeapon2;
}

int getPairedMainHandSlot(int offHandSlot) {
    return offHandSlot == InventorySlots::leftWeapon2 ? InventorySlots::rightWeapon2 : InventorySlots::rightWeapon;
}

int getPairedOffHandSlot(int mainHandSlot) {
    return mainHandSlot == InventorySlots::rightWeapon2 ? InventorySlots::leftWeapon2 : InventorySlots::leftWeapon;
}

bool isOneHandedWeapon(const Item &item) {
    switch (item.weaponWield()) {
    case WeaponWield::StunBaton:
    case WeaponWield::SingleSword:
    case WeaponWield::BlasterPistol:
        return true;
    default:
        return false;
    }
}

bool isTwoHandedWeapon(const Item &item) {
    switch (item.weaponWield()) {
    case WeaponWield::DoubleBladedSword:
    case WeaponWield::BlasterRifle:
    case WeaponWield::HeavyWeapon:
        return true;
    default:
        return false;
    }
}

// Alternate hands share the equipable-slot entries of the primary hands.
static int getCandidateEquipSlot(int slot) {
    switch (slot) {
    case InventorySlots::rightWeapon2:
        return InventorySlots::rightWeapon;
    case InventorySlots::leftWeapon2:
        return InventorySlots::leftWeapon;
    default:
        return slot;
    }
}

// Item property types that limit who may equip an item.
// A cast-spell property of this kind lets anyone use a droid- or human-only item.
static constexpr uint16_t kRaceExemptCastSpell = 0x81;
static constexpr int kAttributeLimitCostTable = 26;
static const FeatType kProficiencyAll = static_cast<FeatType>(93);

std::optional<int> equipmentRefusalFeedback(EquipmentCandidateReason reason) {
    switch (reason) {
    case EquipmentCandidateReason::Alignment: return 1512;
    case EquipmentCandidateReason::Class: return 1513;
    case EquipmentCandidateReason::Race: return 1514;
    case EquipmentCandidateReason::Feat: return 0;
    case EquipmentCandidateReason::Gender: return 47993;
    case EquipmentCandidateReason::Attribute: return 117590;
    case EquipmentCandidateReason::ExcludedForCreature: return 130591;
    case EquipmentCandidateReason::MinimumLevel: return 1470;
    default: return std::nullopt;
    }
}

int minimumEquipLevel(const Creature &creature, const Item &item) {
    const uint32_t cost = item.cost(creature.game().baseItemCostMultiplier(item.baseItemType()));
    auto table = creature.services().resource.twoDas.get("itemvalue");
    const int rows = table ? table->getRowCount() : 0;
    int row = 0;
    while (row < rows && cost > static_cast<uint32_t>(table->getInt(row, "maxsingleitemvalue", 0))) ++row;
    return row + 1;
}

bool hasEquipmentProficiency(const Creature &creature, const Item &item) {
    if (!item.isEquippable()) return false;
    if (creature.hasEffectiveFeat(kProficiencyAll)) return true;
    return std::all_of(item.requiredFeats().begin(), item.requiredFeats().end(), [&creature](FeatType feat) {
        return creature.hasEffectiveFeat(feat);
    });
}

template <class Pred>
static bool anyActiveProperty(const Item &item, ItemProperty type, Pred pred) {
    return std::any_of(item.properties().begin(), item.properties().end(), [&](const Item::PropertyEntry &property) {
        return property.propertyName == static_cast<uint16_t>(type) && item.isPropertyActive(property) && pred(property);
    });
}

template <class Pred>
static bool allActiveProperties(const Item &item, ItemProperty type, Pred pred) {
    return std::all_of(item.properties().begin(), item.properties().end(), [&](const Item::PropertyEntry &property) {
        return property.propertyName != static_cast<uint16_t>(type) || !item.isPropertyActive(property) || pred(property);
    });
}

// Whether the item has a property of the type that its upgrades leave in force.
static bool hasProperty(const Item &item, ItemProperty type) {
    return item.hasActiveProperty(type);
}

// TSL passes when any limit matches the good-evil score; KotOR needs every
// limit to name the creature's simple alignment.
static bool meetsAlignmentLimits(const Creature &creature, const Item &item, bool tsl) {
    if (!hasProperty(item, ItemProperty::UseLimitationAlignmentGroup) || creature.hasEffectiveFeat(kProficiencyAll)) return true;
    const int goodEvil = creature.goodEvil();
    if (!tsl) {
        const int simple = goodEvil <= 40 ? 3 : (goodEvil >= 60 ? 2 : 1);
        return allActiveProperties(item, ItemProperty::UseLimitationAlignmentGroup, [simple](const Item::PropertyEntry &property) {
            return property.subtype == simple;
        });
    }
    return anyActiveProperty(item, ItemProperty::UseLimitationAlignmentGroup, [goodEvil](const Item::PropertyEntry &property) {
        switch (property.subtype) {
        case 0: return true;
        case 1: return goodEvil >= 41 && goodEvil <= 59;
        case 2: return goodEvil > 59;
        case 3: return goodEvil < 41;
        case 4: return goodEvil == 0;
        case 5: return goodEvil == 100;
        default: return false;
        }
    });
}

static bool meetsClassLimits(const Creature &creature, const Item &item) {
    if (!hasProperty(item, ItemProperty::UseLimitationClass) || creature.hasEffectiveFeat(kProficiencyAll)) return true;
    const auto &classes = creature.attributes().classLevels();
    return anyActiveProperty(item, ItemProperty::UseLimitationClass, [&classes](const Item::PropertyEntry &property) {
        return std::any_of(classes.begin(), classes.end(), [&property](const auto &entry) {
            return entry.first && static_cast<int>(entry.first->type()) == property.subtype;
        });
    });
}

// A droid- or human-only base item refuses anyone else outright, except that
// in TSL a lenient check lets the wrong race through every race limit.
static bool meetsRaceLimits(const Creature &creature, const Item &item, bool tsl, bool strict) {
    const int race = static_cast<int>(creature.racialType());
    const int subrace = static_cast<int>(creature.subrace());
    const bool exempt = anyActiveProperty(item, ItemProperty::ActivateItem, [](const Item::PropertyEntry &property) {
        return property.subtype == kRaceExemptCastSpell;
    });
    if (item.droidOrHuman() != 0 && !exempt) {
        bool matches = false;
        if (item.droidOrHuman() == 2) {
            matches = creature.racialType() == RacialType::Droid;
        } else if (item.droidOrHuman() == 1) {
            matches = creature.racialType() == RacialType::Human;
        } else {
            return false;
        }
        if (!matches) return tsl && !strict;
    }
    if (subrace >= 0 && subrace < 32 && (item.deniedSubraces() & (1u << subrace)) != 0) return false;
    const bool subraceLimits = tsl && hasProperty(item, ItemProperty::LimitUseBySubrace);
    if (!hasProperty(item, ItemProperty::UseLimitationRacialType) && !subraceLimits) return true;
    return std::any_of(item.properties().begin(), item.properties().end(), [&](const Item::PropertyEntry &property) {
        if (!item.isPropertyActive(property)) return false;
        if (property.propertyName == static_cast<uint16_t>(ItemProperty::UseLimitationRacialType)) return race == property.subtype;
        return tsl && property.propertyName == static_cast<uint16_t>(ItemProperty::LimitUseBySubrace) &&
               creature.racialType() != RacialType::Droid && subrace == property.subtype;
    });
}

static bool meetsFeatLimits(const Creature &creature, const Item &item) {
    if (!hasProperty(item, ItemProperty::UseLimitationFeat) || creature.hasEffectiveFeat(kProficiencyAll)) return true;
    return allActiveProperties(item, ItemProperty::UseLimitationFeat, [&creature](const Item::PropertyEntry &property) {
        return creature.featRemainingUses(static_cast<FeatType>(property.subtype)) != 0;
    });
}

static bool meetsGenderLimits(const Creature &creature, const Item &item) {
    const int gender = static_cast<int>(creature.gender());
    return allActiveProperties(item, ItemProperty::LimitUseByGender, [gender](const Item::PropertyEntry &property) {
        return property.subtype == gender || (property.subtype <= 1 && gender == static_cast<int>(Gender::Both));
    });
}

// Limited to named characters by tag, or to the player's own character.
static bool meetsPlayerCharacterLimits(const Creature &creature, const Item &item) {
    if (!hasProperty(item, ItemProperty::LimitUseByPc)) return true;
    auto table = creature.services().resource.twoDas.get("iprp_pc");
    return anyActiveProperty(item, ItemProperty::LimitUseByPc, [&](const Item::PropertyEntry &property) {
        if (property.subtype == 0) return creature.isPlayerCreated();
        return table && boost::iequals(table->getString(property.subtype, "expectedtag"), creature.tag());
    });
}

// Each limit names an ability (the intelligence limit reads constitution) and
// a minimum base score.
static bool meetsAttributeLimits(const Creature &creature, const Item &item) {
    if (!hasProperty(item, ItemProperty::LimitUseByAttribute)) return true;
    auto &twoDas = creature.services().resource.twoDas;
    auto costTables = twoDas.get("iprp_costtable");
    auto values = costTables ? twoDas.get(boost::to_lower_copy(costTables->getString(kAttributeLimitCostTable, "name"))) : nullptr;
    return allActiveProperties(item, ItemProperty::LimitUseByAttribute, [&](const Item::PropertyEntry &property) {
        Ability ability;
        switch (property.subtype) {
        case 0: ability = Ability::Strength; break;
        case 1: ability = Ability::Dexterity; break;
        case 2:
        case 3: ability = Ability::Constitution; break;
        case 4: ability = Ability::Wisdom; break;
        case 5: ability = Ability::Charisma; break;
        default: return true;
        }
        const int required = values ? values->getInt(property.costValue, "value", 0) : 0;
        return creature.attributes().getAbilityScore(ability) >= required;
    });
}

// Bao-Dur cannot equip the Jedi robes.
static bool isExcludedForCreature(const Creature &creature, const Item &item) {
    if (!boost::iequals(creature.tag(), "baodur")) return false;
    switch (item.baseItemType()) {
    case 35: case 36: case 37:
    case 97: case 98: case 99: case 100:
    case 102:
        return true;
    default:
        return false;
    }
}

// Base item types the item-use rules single out.
static constexpr int kDroidUtilityItemType = 12;
static constexpr int kDroidShieldItemType = 13;
static constexpr int kForearmBandsItemType = 20;
static constexpr int kStimItemType = 25;
static constexpr int kDroidRepairItemType = 26;
static constexpr int kTrapKitItemType = 28;
static constexpr int kMedicalItemType = 45;
static constexpr int kSquadRecoveryItemType = 47;

bool canUseItem(const Creature &creature, const Item &item, bool menuCheck) {
    const bool tsl = creature.game().isTSL();
    if (!meetsAlignmentLimits(creature, item, tsl) || !meetsClassLimits(creature, item) ||
        !meetsRaceLimits(creature, item, tsl, menuCheck)) return false;
    // TSL menus offer no healing kit to a creature at full vitality.
    if (tsl && menuCheck && (item.itemType() == kDroidRepairItemType || item.itemType() == kMedicalItemType) &&
        creature.currentHitPoints() == creature.maxHitPoints()) return false;
    if (!meetsFeatLimits(creature, item)) return false;
    if (tsl && (!meetsGenderLimits(creature, item) || !meetsPlayerCharacterLimits(creature, item) ||
                !meetsAttributeLimits(creature, item) || isExcludedForCreature(creature, item))) return false;
    if (creature.isPC() && creature.attributes().getAggregateLevel() < minimumEquipLevel(creature, item)) return false;
    // A weapon may be at most one size larger than its user.
    if (item.weaponType() != WeaponType::None &&
        static_cast<int>(item.weaponSize()) - static_cast<int>(creature.size()) >= 2) return false;
    return !item.isEquippable() || hasEquipmentProficiency(creature, item);
}

// The first property of a type that the item's upgrades leave in force.
static const Item::PropertyEntry *firstActiveProperty(const Item &item, ItemProperty type) {
    for (const auto &property : item.properties())
        if (property.propertyName == static_cast<uint16_t>(type) && item.isPropertyActive(property)) return &property;
    return nullptr;
}

static bool partyHasRace(Game &game, RacialType race) {
    return std::any_of(game.party().members().begin(), game.party().members().end(), [race](const auto &member) {
        return member.creature && member.creature->racialType() == race;
    });
}

static bool isEquippedBy(const Creature &creature, const Item &item) {
    return std::any_of(creature.equipment().begin(), creature.equipment().end(), [&item](const auto &entry) {
        return entry.second.get() == &item;
    });
}

bool isLeaderUsableItem(const Creature &leader, const Item &item, int flags) {
    Game &game = leader.game();
    const bool tsl = game.isTSL();
    const bool mineKit = (flags & LeaderUsableFlags::mines) && item.itemType() == kTrapKitItemType;
    // A mine kit asks for a usable trap and the Demolitions skill.
    auto trapUsable = [&]() {
        const auto *trap = firstActiveProperty(item, ItemProperty::Trap);
        if (!trap) return canUseItem(leader, item, true);
        return trap->usable && leader.attributes().hasSkill(SkillType::Demolitions) && canUseItem(leader, item, true);
    };
    const auto *castSpell = firstActiveProperty(item, ItemProperty::ActivateItem);
    if (item.plotFlag()) {
        if (mineKit) return trapUsable();
        if (!castSpell) return false;
    } else {
        auto module = game.module();
        auto area = module ? module->area() : nullptr;
        if (area && area->playerRestrictMode()) return false;
        const int type = item.itemType();
        const bool kit = type == kMedicalItemType || type == kSquadRecoveryItemType || type == kDroidRepairItemType;
        if (tsl) {
            if (mineKit) return trapUsable();
            // TSL offers stims and healing kits whenever someone in the party
            // can take them: droids for repair kits, humans for the rest.
            if ((flags & LeaderUsableFlags::healing) && (kit || type == kStimItemType)) {
                if (castSpell && !castSpell->usable) return false;
                return partyHasRace(game, type == kDroidRepairItemType ? RacialType::Droid : RacialType::Human);
            }
        } else {
            if ((flags & LeaderUsableFlags::healing) && kit) {
                return castSpell ? castSpell->usable && canUseItem(leader, item, true) : canUseItem(leader, item, true);
            }
            if ((flags & LeaderUsableFlags::self) && type == kStimItemType) {
                if (castSpell && !castSpell->usable) return false;
                return canUseItem(leader, item, true);
            }
            if (mineKit) return trapUsable();
        }
        if (!castSpell || !castSpell->usable) return false;
    }
    // Droid utilities and shields and forearm bands work only while worn.
    switch (item.itemType()) {
    case kDroidUtilityItemType:
    case kDroidShieldItemType:
    case kForearmBandsItemType:
        if (!isEquippedBy(leader, item)) return false;
        break;
    default:
        break;
    }
    // Only a spell aimed at its user can be used from the inventory.
    if (!(flags & LeaderUsableFlags::self)) return false;
    auto spell = leader.services().game.spells.get(static_cast<SpellType>(castSpell->subtype));
    return spell && spell->itemTargeting == 1 && canUseItem(leader, item, true);
}

EquipmentCandidateReason evaluateEquipmentAdmission(const Creature &creature, const Item &item) {
    const bool tsl = creature.game().isTSL();
    if (item.isNonEquippable()) return EquipmentCandidateReason::NonEquippable;
    if (!hasEquipmentProficiency(creature, item)) return EquipmentCandidateReason::Proficiency;
    if (!meetsAlignmentLimits(creature, item, tsl)) return EquipmentCandidateReason::Alignment;
    if (!meetsClassLimits(creature, item)) return EquipmentCandidateReason::Class;
    if (!meetsRaceLimits(creature, item, tsl, true)) return EquipmentCandidateReason::Race;
    if (!meetsFeatLimits(creature, item)) return EquipmentCandidateReason::Feat;
    if (tsl) {
        if (!meetsGenderLimits(creature, item)) return EquipmentCandidateReason::Gender;
        if (!meetsPlayerCharacterLimits(creature, item)) return EquipmentCandidateReason::PlayerCharacter;
        if (!meetsAttributeLimits(creature, item)) return EquipmentCandidateReason::Attribute;
        if (isExcludedForCreature(creature, item)) return EquipmentCandidateReason::ExcludedForCreature;
    }
    return EquipmentCandidateReason::None;
}

EquipmentSlotActivationDecision evaluateEquipmentSlotActivation(
    const Creature &creature,
    int requestedSlot) {

    EquipmentSlotActivationDecision result;
    result.requestedSlot = requestedSlot;

    if (requestedSlot != InventorySlots::leftWeapon)
        return result;

    result.pairedSlot = InventorySlots::rightWeapon;
    auto mainHand = creature.getEquippedItem(result.pairedSlot);
    if (mainHand && mainHand->weaponSize() == CreatureSize::Large) {
        result.available = false;
        result.reason = EquipmentSlotActivationReason::OffHandBlockedByLargeMainHandWeapon;
    }

    return result;
}

static void decideWeaponPair(
    const Creature &creature,
    const Item &item,
    EquipmentCandidateDecision &result) {

    const int requested = result.requestedSlot;
    const int mainSlot = isMainHandWeaponSlot(requested) ? requested : getPairedMainHandSlot(requested);
    const int offSlot = getPairedOffHandSlot(mainSlot);
    const auto mainHand = creature.getEquippedItem(mainSlot);
    const auto offHand = creature.getEquippedItem(offSlot);

    if (!mainHand && !offHand) {
        result.actualSlot = mainSlot;
        result.action = EquipmentCandidateAction::Equip;
        return;
    }
    if (!mainHand) {
        result.action = EquipmentCandidateAction::Reject;
        result.reason = EquipmentCandidateReason::OffHandWithoutMainHand;
        return;
    }
    const bool rangedChange = mainHand->isRanged() != item.isRanged();
    if (isTwoHandedWeapon(item) || (offHand && rangedChange)) {
        result.actualSlot = mainSlot;
        result.pairedSlot = offSlot;
        result.action = EquipmentCandidateAction::ClearPairAndEquip;
        return;
    }
    if (offHand) {
        result.action = EquipmentCandidateAction::Replace;
        return;
    }
    if (!rangedChange && isOneHandedWeapon(*mainHand) && requested == offSlot) {
        result.action = EquipmentCandidateAction::Equip;
        return;
    }
    result.actualSlot = mainSlot;
    result.action = EquipmentCandidateAction::Replace;
}

EquipmentCandidateDecision evaluateEquipmentCandidate(
    const Creature &creature,
    int requestedSlot,
    const Item *item) {

    EquipmentCandidateDecision result;
    result.requestedSlot = requestedSlot;
    result.actualSlot = requestedSlot;

    if (!item) {
        result.visible = true;
        result.valid = true;
        if (isMainHandWeaponSlot(requestedSlot)) {
            result.pairedSlot = getPairedOffHandSlot(requestedSlot);
            result.action = EquipmentCandidateAction::ClearMainHandAndOffHand;
        } else {
            result.action = EquipmentCandidateAction::ClearSlot;
        }
        return result;
    }

    // Screens list the items whose equipable slots include the requested slot.
    result.visible = item->isEquippable(getCandidateEquipSlot(requestedSlot));
    result.reason = evaluateEquipmentAdmission(creature, *item);
    if (result.reason != EquipmentCandidateReason::None) {
        result.action = EquipmentCandidateAction::Reject;
        return result;
    }
    if (isMainHandWeaponSlot(requestedSlot) || isOffHandWeaponSlot(requestedSlot)) {
        decideWeaponPair(creature, *item, result);
    } else {
        result.slotOccupied = static_cast<bool>(creature.getEquippedItem(requestedSlot));
        result.action = result.slotOccupied
            ? EquipmentCandidateAction::Replace
            : EquipmentCandidateAction::Equip;
    }
    if (result.action == EquipmentCandidateAction::Reject)
        return result;

    if (!item->isEquippable(getCandidateEquipSlot(result.actualSlot))) {
        result.action = EquipmentCandidateAction::Reject;
        result.reason = EquipmentCandidateReason::NotEquippableInActualSlot;
        return result;
    }
    result.valid = true;
    return result;
}

std::shared_ptr<Item> takeEquipmentCandidate(
    Game &game,
    Object &inventory,
    const std::shared_ptr<Item> &item) {

    bool last = false;
    if (!inventory.removeItem(item, last)) {
        return nullptr;
    }
    if (last) {
        return item;
    }

    auto split = game.newItemClone(*item);
    split->setStackSize(1);
    return split;
}

static std::shared_ptr<Area> activeAreaOwningItem(
    Game &game,
    const std::shared_ptr<Item> &item) {
    if (!item) return nullptr;
    auto module = game.module();
    auto area = module ? module->area() : nullptr;
    if (!area) return nullptr;
    auto owned = std::find_if(
        area->objects().begin(), area->objects().end(),
        [&item](const auto &candidate) {
            return candidate.get() == item.get();
        });
    return owned == area->objects().end() ? nullptr : area;
}

bool isActiveAreaOwnedItem(
    Game &game,
    const std::shared_ptr<Item> &item) {
    return static_cast<bool>(activeAreaOwningItem(game, item));
}

bool releaseAreaOwnedItem(
    Game &game,
    const std::shared_ptr<Item> &item) {
    if (!item || !item->isRuntimeLive() || item->isPresentationOnly() ||
        item->isEquipped() ||
        item->isHeld()) {
        return false;
    }
    auto area = activeAreaOwningItem(game, item);
    return area && area->releaseObject(item);
}

bool transferItemTo(
    Game &game,
    const std::shared_ptr<Item> &item,
    Object &receiver) {
    if (!item || (!item->isRuntimeLive() && !item->isPresentationOnly()) ||
        (!receiver.isRuntimeLive() && !receiver.isPresentationOnly()) ||
        (item->isPresentationOnly() != receiver.isPresentationOnly()) ||
        item.get() == &receiver) {
        return false;
    }

    uint32_t ownerId = item->owner();
    if (ownerId == receiver.id() && !item->isEquipped()) return true;

    if (!item->isHeld()) {
        // owner() describes nested inventory/equipment ownership; it does not
        // encode Area residency. Only an exact active-Area ownership edge makes
        // an unheld Item transferable. Everything else fails closed rather
        // than treating an unheld Item as freely claimable.
        if (item->isPresentationOnly() || item->isEquipped() ||
            std::find(receiver.items().begin(), receiver.items().end(), item) !=
                receiver.items().end()) {
            return false;
        }
        if (!releaseAreaOwnedItem(game, item)) {
            return false;
        }
        receiver.addItem(item);
        return true;
    }

    auto owner = game.getObjectById(ownerId);
    if (!owner) return false;

    if (auto creature = std::dynamic_pointer_cast<Creature>(owner);
        creature && item->isEquipped()) {
        if (!creature->takeEquippedItem(item)) return false;
    } else if (!owner->removeItemStack(item)) {
        return false;
    }

    receiver.addItem(item);
    return true;
}

} // namespace game

} // namespace reone
