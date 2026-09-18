#include "reone/game/spellrules.h"
#include "reone/game/forcerules.h"
#include "reone/game/d20/spell.h"
#include "reone/game/savingthrowrules.h"
#include "../deathrules.h"
#include "reone/game/deathexperience.h"
#include "reone/game/reputes.h"
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

#include "reone/game/object/creature.h"
#include "reone/game/effect/regenerate.h"
#include "reone/game/projectiles.h"
#include "reone/game/staterules.h"
#include "../posthitdata.h"

#include "reone/game/d20/abilityrules.h"
#include "reone/game/effect/rules.h"
#include "reone/game/effect/damageshield.h"
#include "reone/game/effect/creaturestate.h"

#include <array>

#include "reone/audio/di/services.h"
#include "reone/audio/mixer.h"
#include "reone/game/action.h"
#include "reone/game/action/attackobject.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/animationutil.h"
#include "reone/game/attack.h"
#include "reone/game/autobalance.h"
#include "reone/game/effect/linkeffects.h"
#include "../physicalcombatrules.h"
#include "../effectimmunityrules.h"
#include "reone/game/d20/classes.h"
#include "reone/game/debug.h"
#include "reone/game/di/services.h"
#include "reone/game/equipmentrules.h"
#include "reone/game/effect/acdecrease.h"
#include "reone/game/effect/acincrease.h"
#include "reone/game/effect/abilitydecrease.h"
#include "reone/game/effect/abilityincrease.h"
#include "reone/game/effect/attackdecrease.h"
#include "reone/game/effect/attackincrease.h"
#include "reone/game/effect/bonusfeat.h"
#include "reone/game/effect/damage.h"
#include "reone/game/effect/disguise.h"
#include "reone/game/effectfeedback.h"
#include "reone/game/effect/visual.h"
#include "reone/game/effect/damagedecrease.h"
#include "reone/game/effect/damageimmunitydecrease.h"
#include "reone/game/effect/damageimmunityincrease.h"
#include "reone/game/effect/damageincrease.h"
#include "reone/game/effect/damagereduction.h"
#include "reone/game/effect/damageresistance.h"
#include "reone/game/effect/immunity.h"
#include "reone/game/effect/invisibility.h"
#include "reone/game/effect/savingthrowdecrease.h"
#include "reone/game/effect/savingthrowincrease.h"
#include "reone/game/effect/source.h"
#include "reone/game/effect/trueseeing.h"
#include "reone/game/footstepsounds.h"
#include "reone/game/game.h"
#include "reone/game/object/area.h"
#include "reone/game/object/module.h"
#include "reone/game/party.h"
#include "reone/game/portraits.h"
#include "reone/game/script/runner.h"
#include "reone/game/surfaces.h"
#include "reone/game/twodautil.h"
#include "reone/graphics/context.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/textureregistry.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/gff.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/provider/gffs.h"
#include "reone/resource/provider/models.h"
#include "reone/resource/provider/soundsets.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/resources.h"
#include "reone/resource/strings.h"
#include "reone/scene/di/services.h"
#include "reone/scene/drawdebug.h"
#include "reone/scene/graphs.h"
#include "reone/scene/types.h"
#include "reone/script/types.h"
#include "reone/system/clock.h"
#include "reone/system/di/services.h"
#include "reone/system/exception/validation.h"
#include "reone/system/logutil.h"
#include "reone/system/randomutil.h"
#include "reone/system/timer.h"

using namespace reone::audio;
using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::script;

namespace reone {

namespace game {

static constexpr int kStrRefRemains = 38151;
static constexpr int kMaximumDamageEffectModifier = 36;
static constexpr int kAllSavingThrows = 0;
static constexpr int kFortitudeSavingThrow = 1;
static constexpr int kReflexSavingThrow = 2;
static constexpr int kWillSavingThrow = 3;

static int rollDamageShieldContribution(ServicesView &services, int selector) {
    if (selector <= 5) return selector;
    const auto table = getRequiredTwoDA(services.resource.twoDas, "iprp_damagecost");
    return rollDamageShieldDice(*table, selector, [](int low, int high) { return randomInt(low, high); });
}

static int getHighestOwnedFeatRank(
    const Creature &creature,
    FeatType firstRank,
    int rankCount) {

    int first = static_cast<int>(firstRank);
    for (int rank = rankCount; rank >= 1; --rank) {
        if (creature.hasEffectiveFeat(static_cast<FeatType>(first + rank - 1))) {
            return rank;
        }
    }
    return 0;
}

static int getHighestTotalDefenseClassLevel(
    const CreatureAttributes &attributes) {

    return std::max({
        attributes.getClassLevel(ClassType::Scoundrel),
        attributes.getClassLevel(ClassType::JediSentinel),
        attributes.getClassLevel(ClassType::JediWatchman),
        attributes.getClassLevel(ClassType::SithAssassin),
    });
}

static constexpr float kCloseRangeAttackDistance2 = 25.0f;
static constexpr float kKeepPathDuration = 1000.0f;
static constexpr float kPathPointTolerance = 0.5f;

static constexpr char kItemPropertyCostTable[] = "iprp_costtable";
static constexpr char kBonusCostTable[] = "iprp_bonuscost";
static constexpr char kMeleeCostTable[] = "iprp_meleecost";
static constexpr char kDecreaseCostTable[] = "iprp_neg5cost";
static constexpr char kResistanceCostTable[] = "iprp_resistcost";
static constexpr char kReductionCostTable[] = "iprp_soakcost";
static constexpr char kVulnerabilityCostTable[] = "iprp_damvulcost";
static constexpr char kDamageTypeTable[] = "iprp_damagetype";
static constexpr char kProtectionTable[] = "iprp_protection";

static std::string g_talkDummyNode("talkdummy");

static const std::string g_headHookNode("headhook");
static const std::string g_maskHookNode("gogglehook");
static const std::string g_rightHandNode("rhand");
static const std::string g_leftHandNode("lhand");

static int getEquipabilitySlot(int slot) {
    switch (slot) {
    case InventorySlots::rightWeapon2:
        return InventorySlots::rightWeapon;
    case InventorySlots::leftWeapon2:
        return InventorySlots::leftWeapon;
    default:
        return slot;
    }
}

static bool attackModifierApplies(
    AttackBonus modifierType,
    const Item *weapon,
    bool offHand) {

    switch (modifierType) {
    case AttackBonus::Misc:
        return true;
    case AttackBonus::Onhand:
        return weapon && !offHand;
    case AttackBonus::Offhand:
        return weapon && offHand;
    default:
        return false;
    }
}

static bool racialTypeMatches(uint16_t racialType, const Creature &target) {
    auto type = static_cast<RacialType>(racialType);
    return type == RacialType::All || type == target.racialType();
}

static bool attackAlignmentGroupMatches(uint16_t alignment, const Creature &target) {
    auto group = static_cast<Alignment>(alignment);
    // The attack-property handler stores Neutral in the unused
    // law/chaos qualifier, so it applies to every target.
    return group == Alignment::All ||
           group == Alignment::Neutral ||
           group == target.alignment();
}

static bool defenseAlignmentGroupMatches(uint16_t alignment, const Creature &attacker) {
    auto group = static_cast<Alignment>(alignment);
    return group == Alignment::All || group == attacker.alignment();
}

static DamageType getItemPropertyDamageType(
    ServicesView &services,
    uint16_t subtype) {

    auto table = getRequiredTwoDA(
        services.resource.twoDas,
        kDamageTypeTable);
    validateTwoDARow(*table, kDamageTypeTable, subtype);
    return static_cast<DamageType>(1 << subtype);
}

static DamagePower getDamageReductionPower(
    ServicesView &services,
    uint16_t subtype) {

    auto table = getRequiredTwoDA(
        services.resource.twoDas,
        kProtectionTable);
    validateTwoDARow(*table, kProtectionTable, subtype);
    return static_cast<DamagePower>(subtype + 1);
}

static bool attackPropertyApplies(
    ItemProperty property,
    uint16_t subtype,
    const Creature *target) {

    switch (property) {
    case ItemProperty::EnhancementBonus:
    case ItemProperty::AttackBonus:
        return true;
    case ItemProperty::EnhancementBonusVsAlignmentGroup:
    case ItemProperty::AttackBonusVsAlignmentGroup:
        return target && attackAlignmentGroupMatches(subtype, *target);
    case ItemProperty::EnhancementBonusVsRacialGroup:
    case ItemProperty::AttackBonusVsRacialGroup:
        return target && racialTypeMatches(subtype, *target);
    default:
        return false;
    }
}

static void getSituationalAttackBonuses(
    const Creature &attacker,
    const Creature &target,
    const Item *weapon,
    int &closeProximityRangedBonus,
    int &meleeOnRangedBonus) {

    closeProximityRangedBonus = 0;
    meleeOnRangedBonus = 0;

    if (weapon && weapon->isRanged()) {
        closeProximityRangedBonus = getCloseProximityRangedAttackBonus(
            attacker.game().isTSL(),
            attacker.getSquareDistanceTo(target) <= kCloseRangeAttackDistance2,
            attacker.hasEffectiveFeat(FeatType::CloseCombat),
            attacker.hasEffectiveFeat(FeatType::ImprovedCloseCombat));
        return;
    }

    auto targetWeapon = target.getEquippedItem(InventorySlots::rightWeapon);
    meleeOnRangedBonus = getMeleeOnRangedAttackBonus(
        attacker.game().isTSL(),
        targetWeapon && targetWeapon->isRanged(),
        target.hasEffectiveFeat(FeatType::CloseCombat),
        target.hasEffectiveFeat(FeatType::ImprovedCloseCombat));
}

static bool equippedItemAppliesToAttack(
    int slot,
    const Item &item,
    const Item *weapon,
    bool offHand) {

    switch (slot) {
    case InventorySlots::rightWeapon:
        return weapon == &item &&
               (!offHand || item.weaponWield() == WeaponWield::DoubleBladedSword);
    case InventorySlots::leftWeapon:
        return weapon == &item && offHand;
    case InventorySlots::hands:
        return !weapon && !offHand;
    case InventorySlots::cWeaponL:
    case InventorySlots::cWeaponR:
    case InventorySlots::cWeaponB:
    case InventorySlots::rightWeapon2:
    case InventorySlots::leftWeapon2:
        return false;
    default:
        return true;
    }
}

static bool isHandSpecificAttackModifierSlot(int slot) {
    switch (slot) {
    case InventorySlots::rightWeapon:
    case InventorySlots::leftWeapon:
    case InventorySlots::hands:
        return true;
    default:
        return false;
    }
}

static void addAttackModifier(int modifier, int &bonus, int &penalty) {
    if (modifier > 0) {
        bonus += modifier;
    } else if (modifier < 0) {
        penalty -= modifier;
    }
}

static int getCostTableValue(
    ServicesView &services,
    const std::string &resRef,
    int row,
    const std::string &column,
    int blankValue) {

    auto table = getRequiredTwoDA(services.resource.twoDas, resRef);
    return table->getInt(row, column, blankValue);
}

struct NamedTwoDA {
    std::string resRef;
    std::shared_ptr<TwoDA> table;
};

static NamedTwoDA getItemPropertyCostTable(
    ServicesView &services,
    int index) {

    auto costTables = getRequiredTwoDA(
        services.resource.twoDas,
        kItemPropertyCostTable);
    std::string resRef = boost::to_lower_copy(
        costTables->getString(index, "name"));
    return {resRef, getRequiredTwoDA(services.resource.twoDas, resRef)};
}

static int getItemPropertyValue(
    ServicesView &services,
    const Item::PropertyEntry &property,
    const std::string &column,
    int blankValue) {

    auto costTable = getItemPropertyCostTable(
        services,
        property.costTable);
    return costTable.table->getInt(
        property.costValue,
        column,
        blankValue);
}

static bool savingThrowModifierApplies(
    int modifierSave,
    SavingThrowType modifierType,
    int requestedSave,
    SavingThrowType requestedType) {

    return (modifierSave == kAllSavingThrows ||
            modifierSave == requestedSave) &&
           (modifierType == SavingThrowType::All ||
            modifierType == requestedType);
}

static bool savingThrowPropertyApplies(
    ItemProperty propertyType,
    int subtype,
    int requestedSave,
    SavingThrowType requestedType) {

    switch (propertyType) {
    case ItemProperty::ImprovedSavingThrow:
        return requestedType != SavingThrowType::All &&
               subtype == static_cast<int>(requestedType);
    case ItemProperty::ImprovedSavingThrowSpecific:
    case ItemProperty::DecreasedSavingThrowsSpecific:
        return subtype == requestedSave;
    case ItemProperty::DecreasedSavingThrows:
        return true;
    default:
        return false;
    }
}

struct DamageModifier {
    int costValue;
    int numDice;
    int die;
    int flat;
    DamageType type;
};

static std::optional<DamageModifier> getDamageModifier(
    ServicesView &services,
    uint16_t costValue,
    DamageType type) {

    auto table = getRequiredTwoDA(services.resource.twoDas, "iprp_damagecost");

    DamageModifier result {costValue, 0, 0, 0, type};

    auto numDice = table->getIntOpt(costValue, "numdice");
    if (!numDice) {
        result.flat = costValue;
    } else {
        auto die = table->getIntOpt(costValue, "die");
        if (!die) {
            throw ValidationException(
                "Missing die in iprp_damagecost row " +
                std::to_string(costValue));
        }
        result.numDice = *numDice;
        result.die = *die;
    }

    if (result.numDice <= 0 && result.flat <= 0) {
        return std::nullopt;
    }
    return result;
}

static std::optional<DamageModifier> getFlatDamageModifier(
    ServicesView &services,
    const std::string &resRef,
    uint16_t costValue,
    DamageType type) {

    int value = getCostTableValue(
        services,
        resRef,
        costValue,
        "value",
        0);
    if (value == 0) {
        return std::nullopt;
    }

    return DamageModifier {costValue, 0, 0, value, type};
}

static int rollDamageModifier(
    const DamageModifier &modifier,
    int multiplier) {

    if (modifier.numDice <= 0 || modifier.die <= 0) {
        return multiplier * modifier.flat;
    }

    int result = 0;
    for (int multiple = 0; multiple < multiplier; ++multiple) {
        for (int die = 0; die < modifier.numDice; ++die) {
            result += randomInt(1, modifier.die);
        }
    }
    return result;
}

static void selectDamageModifier(
    std::map<int, DamageModifier> &modifiers,
    DamageModifier modifier) {

    int type = static_cast<int>(modifier.type);
    auto it = modifiers.find(type);
    if (it == modifiers.end() ||
        modifier.costValue > it->second.costValue) {
        modifiers.insert_or_assign(type, std::move(modifier));
    }
}

static bool damagePropertyApplies(
    ItemProperty property,
    uint16_t subtype,
    const Creature *target) {

    switch (property) {
    case ItemProperty::EnhancementBonus:
    case ItemProperty::DamageBonus:
    case ItemProperty::DecreasedDamage:
        return true;
    case ItemProperty::EnhancementBonusVsAlignmentGroup:
    case ItemProperty::DamageBonusVsAlignmentGroup:
        return target && attackAlignmentGroupMatches(subtype, *target);
    case ItemProperty::EnhancementBonusVsRacialGroup:
    case ItemProperty::DamageBonusVsRacialGroup:
        return target && racialTypeMatches(subtype, *target);
    default:
        return false;
    }
}

static bool equippedItemPropertiesAreActive(int slot) {
    return slot != InventorySlots::rightWeapon2 &&
           slot != InventorySlots::leftWeapon2;
}

static std::string formatFeedbackString(
    Game &game,
    ServicesView &services,
    int strRef,
    std::initializer_list<std::pair<int, std::string>> tokens) {

    std::string text = services.resource.strings.getText(strRef);
    for (const auto &[token, value] : tokens) {
        text = game.substituteCustomToken(
            std::move(text),
            token,
            value);
    }
    return text;
}

static void applyDeathExperience(
    Creature &victim, const std::shared_ptr<Object> &damager, const std::string &victimName,
    Game &game, ServicesView &services) {

    // The party-table count excludes the original PC. The faction
    // manager maps its (NPC faction, zero) query to repute's player row.
    if (victim.isDead() || game.party().isMember(victim) ||
        services.game.reputes.getReputation(Faction::Player, victim.faction()) > 10) return;
    const auto player = game.party().actualPlayer();
    if (!player) return; // CalculateDeathExperience's original-PC lookup failed.
    const auto thresholds = readExperienceThresholds(
        *getRequiredTwoDA(services.resource.twoDas, "exptable"), game.isTSL());
    const auto &context = victim.autoBalanceContext();
    const bool autoBalance = game.isTSL() && context.multiplierSet != 0 &&
                             !game.party().isMember(victim);
    const int challengeModifier = autoBalance
        ? services.game.autoBalance.get(context.multiplierSet).challengeRatingModifier : 0;
    const int row = getDeathExperienceRow(thresholds, std::uint32_t(player->xp()), game.isTSL());
    const int column = getDeathExperienceColumn(game.isTSL(), victim.challengeRating(),
        autoBalance, context.playerLevelAtSpawn, challengeModifier);
    const int companions = static_cast<int>(std::count_if(
        game.party().members().begin(), game.party().members().end(),
        [](const Party::Member &member) { return member.npc != kNpcPlayer; }));
    const auto xp = resolveDeathExperience(
        *getRequiredTwoDA(services.resource.twoDas, "xptable"),
        *getRequiredTwoDA(services.resource.twoDas, "npc"), row, column, game.isTSL(), companions);

    // DistributeExperience ignores nonpositive awards and always awards
    // the party. LastDamager is feedback attribution, not an XP recipient.
    if (xp.awarded > 0) game.party().awardXP(xp.awarded, XPSource::Combat);

    // Trap-owner attribution and the K2 death-message receiver remain separate
    // producer work; do not manufacture a trap owner or reuse K1 strings in K2.
    if (!game.isTSL()) {
        auto recipient = dyn_cast<Creature>(damager);
        if (!recipient) recipient = game.party().getLeader();
        const auto leader = game.party().getLeader();
        if (recipient && leader && leader->faction() == recipient->faction() &&
            leader->getSquareDistanceTo(victim) <= 900.0f) {
            game.messageLog().add(MessageLog::kFeedbackMessageType, MessageLog::Style::Normal,
                formatFeedbackString(game, services, 1407,
                    {{0, recipient->name()}, {1, victimName}, {2, std::to_string(xp.reported)}}));
        }
        game.floatingText().addExperience(victim, xp.awarded);
    }
}

Creature::Creature(
    uint32_t id,
    std::string sceneName,
    Game &game,
    ServicesView &services) :
    Object(id, ObjectType::Creature, std::move(sceneName), game, services) {

    // Workaround: the original engine does not retain perception range in
    // savegames. Set default ranges to match PercepRngDefault from ranges.2da.
    _perception.sightRange = 20.0f;
    _perception.hearingRange = 20.0f;
}

void Creature::retireAreaRuntime(
    Pathfinder &pathfinder,
    const std::set<const Object *> &retainedObjects) {
    retireAreaRuntimeState(retainedObjects);

    if (_path) {
        releasePath(pathfinder, *_path);
        _path.reset();
    }
    _pathVelocity = glm::vec3(0.0f);
    _previousPosition = _position;
    _stuckTimer.reset(0.0f);
    _stuckForce = glm::vec3(0.0f);
    setMovementType(MovementType::None);
    _blockingDoorId = script::kObjectInvalid;
    _blockedEventDoorId = script::kObjectInvalid;

    deactivateCombat(0.0f);
    _combatState.attackTarget.reset();
    _combatState.attemptedAttackTarget.reset();
    _combatState.attackAction = ActionType::QueueEmpty;
    _combatState.combatFeat = FeatType::Invalid;
    _combatState.deactivationTimer.reset(0.0f);
    _lastHostileTarget.reset();
    _lastAttackAction = ActionType::QueueEmpty;
    _lastCombatFeat = FeatType::Invalid;
    _lastAttackResult = AttackResultType::Invalid;
    _incomingAttacker = SavedObjectReference {};
    _attackerList.clear();
    _receivedAttack = {};
    _lastWeaponUsed = script::kObjectInvalid;
    _clientCombatMode = false;

    _perception.seen.clear();
    _perception.heard.clear();
    stopTalking();
    stopStuntMode();
    if (_audioSourceVoice) _audioSourceVoice->stop();
    if (_audioSourceFootstep) _audioSourceFootstep->stop();
    _audioSourceVoice.reset();
    _audioSourceFootstep.reset();
}

void Creature::loadFromBlueprint(const std::string &resRef) {
    auto utc = _services.resource.gffs.get(resRef, ResType::Utc);
    if (!utc) {
        return;
    }
    // A blueprint is a single source, so deserialize it once. Routing through
    // deserialize() would re-read the self-referential TemplateResRef and
    // deserialize the same data twice, doubling accumulated class levels.
    deserializeAll(*utc, SerializedIdentityContext::templateResource(resRef));
    restoreSerializedVitality();
    updateTransform();
    loadAppearance();
}

void Creature::loadAppearanceProperties() {
    std::shared_ptr<TwoDA> appearances(_services.resource.twoDas.get("appearance"));
    if (!appearances) {
        throw ResourceNotFoundException("appearance 2DA not found");
    }

    _modelType = parseModelType(appearances->getString(_appearance, "modeltype"));
    _walkSpeed = appearances->getFloat(_appearance, "walkdist", 1.0f);
    _runSpeed = appearances->getFloat(_appearance, "rundist", 1.0f);
    float personalSpace = appearances->getFloat(_appearance, "perspace", 0.6f);
    _creaturePersonalSpace = appearances->getFloat(_appearance, "creperspace", personalSpace);
    _size = static_cast<CreatureSize>(appearances->getInt(
        _appearance,
        "sizecategory",
        static_cast<int>(CreatureSize::Invalid)));
    _footstepType = appearances->getInt(_appearance, "footsteptype", -1);
    _envmap = boost::to_lower_copy(appearances->getString(_appearance, "envmap"));

    if (_portraitId > 0) {
        _portrait = _services.game.portraits.getTextureByIndex(_portraitId);
    } else {
        _portrait = _services.game.portraits.getTextureByAppearance(_appearance);
    }
}

void Creature::loadAppearance() {
    loadAppearanceProperties();

    auto modelSceneNode = buildModel();
    if (modelSceneNode) {
        finalizeModel(*modelSceneNode);
        _sceneNode = std::move(modelSceneNode);
        _sceneNode->setUser(*this);
        _sceneNode->setLocalTransform(_transform);
    }

    _animDirty = true;
}

Creature::ModelType Creature::parseModelType(const std::string &s) const {
    if (s == "S" || s == "L") {
        return ModelType::Creature;
    } else if (s == "F") {
        return ModelType::Droid;
    } else if (s == "B") {
        return ModelType::Character;
    }

    throw std::invalid_argument(str(boost::format("Model type '%s' is not supported") % s));
}

void Creature::updateModel() {
    if (!_sceneNode) {
        return;
    }
    auto bodyModelName = getBodyModelName();
    if (bodyModelName.empty()) {
        return;
    }
    auto replacement = _services.resource.models.get(bodyModelName);
    if (!replacement) {
        return;
    }
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    model->setModel(*replacement);
    finalizeModel(*model);
    if (!_stunt) {
        model->setLocalTransform(_transform);
    }
    _animDirty = true;
}

void Creature::loadTransformFromGIT(const resource::generated::GIT_Creature_List &git) {
    _position[0] = git.XPosition;
    _position[1] = git.YPosition;
    _position[2] = git.ZPosition;

    float cosine = git.XOrientation;
    float sine = git.YOrientation;
    _orientation = glm::quat(glm::vec3(0.0f, 0.0f, -glm::atan(cosine, sine)));

    updateTransform();
}

bool Creature::isDebilitated() const {
    return _combatState.debilitated || !canExecuteActions();
}

bool Creature::isTemporarilyDead() const {
    return _game.party().isMember(*this) && currentHitPoints() <= 0;
}

bool Creature::isInvisibleTo(const Creature &observer) const {
    for (const EffectInstance &applied : effects()) {
        if (!applied.hasLiveRuntimeSource() ||
            applied.type() != EffectType::Invisibility ||
            !applied.appliesVersus(&observer)) {
            continue;
        }

        auto type = static_cast<InvisibilityType>(
            applied.integerParameter(0));
        switch (type) {
        case InvisibilityType::Normal:
        case InvisibilityType::Improved:
            if (observer.hasVisibilityCounter(
                    kSeeInvisibleCounter | kTrueSeeingCounter)) {
                continue;
            }
            return true;
        case InvisibilityType::Darkness:
            if (observer.hasVisibilityCounter(
                    kUltravisionCounter | kTrueSeeingCounter)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

void Creature::setVisibilityCounter(uint8_t bit) {
    _visibilityCounterBits |= bit;
}

void Creature::restoreBlindnessCounter(int mask, uint64_t removedApplication) {
    int remaining = _visibilityCounterBits & ~mask;
    for (const auto &effect : effects()) {
        if (effect.serializedType > 73) break;
        if (effect.serializedType == 73 && effect.applicationOrder != removedApplication)
            remaining |= effect.integerParameter(0);
    }
    _visibilityCounterBits = static_cast<uint8_t>(remaining);
}

bool Creature::hasVisibilityCounter(uint8_t bits) const {
    uint8_t effective = _visibilityCounterBits;
    if (!hasEffect(EffectType::SeeInvisible)) {
        effective &= ~kSeeInvisibleCounter;
    }
    if (!hasEffect(EffectType::TrueSeeing)) {
        effective &= ~kTrueSeeingCounter;
    }
    if (!hasEffect(EffectType::Ultravision) &&
        !_trueSeeingUltravisionQuirk) {
        effective &= ~kUltravisionCounter;
    }
    return (effective & bits) != 0;
}

void Creature::restoreVisibilityCounter(
    EffectType type,
    uint8_t bit,
    uint64_t removedApplication,
    bool trueSeeingRemovalQuirk) {

    _visibilityCounterBits &= ~bit;
    if (bit == kUltravisionCounter) {
        _trueSeeingUltravisionQuirk = false;
    }
    bool another = std::any_of(
        effects().begin(),
        effects().end(),
        [type, removedApplication](const EffectInstance &applied) {
            return applied.applicationOrder != removedApplication &&
                   applied.hasLiveRuntimeSource() &&
                   applied.type() == type;
        });
    if (another) {
        if (trueSeeingRemovalQuirk) {
            _visibilityCounterBits |= kUltravisionCounter;
            _trueSeeingUltravisionQuirk = true;
        } else {
            _visibilityCounterBits |= bit;
        }
    }
}

void Creature::refreshVisibilityPerception() {
    auto module = _game.module();
    auto area = module ? module->area() : nullptr;
    if (area && area->isObjectResident(*this)) {
        area->refreshPerceptionFor(*this);
    }
}

void Creature::clearHostileActionsAgainst(const Object &object) {
    auto attackTarget = _combatState.attackTarget.resolve();
    if (attackTarget.get() == &object) {
        _combatState.attackTarget.reset();
        _combatState.shouldDeactivate = true;
    }

    const auto candidates = _actions.nodes;
    for (const auto &node : candidates) {
        if (std::find(_actions.nodes.begin(), _actions.nodes.end(), node) == _actions.nodes.end()) continue;
        const std::shared_ptr<Action> action = node->action;
        std::shared_ptr<Object> target;
        if (action && action->type() == ActionType::AttackObject) {
            target = static_cast<AttackObjectAction &>(*action).target();
        } else if (action && action->type() == ActionType::UseFeat) {
            auto &featAction = static_cast<UseFeatAction &>(*action);
            if (isPhysicalAttackFeat(featAction.feat())) {
                target = featAction.target();
            }
        }

        if (target.get() != &object) continue;
        action->cancel(action, *this);
        action->markCancelled();
        auto position = std::find(_actions.nodes.begin(), _actions.nodes.end(), node);
        if (position != _actions.nodes.end()) _actions.nodes.erase(position);
    }
}

bool Creature::canExecuteActions() const {
    if (isForcePushed()) return false;
    return !_dead && !isTemporarilyDead() && !hasEffect(EffectType::Stunned) &&
           (_effectState != 4 && _effectState != 5 && _effectState != 6);
}

bool Creature::permitsAction(const Action &action) const {
    switch (action.type()) {
    case ActionType::MoveToObject:
    case ActionType::MoveToPoint:
    case ActionType::MoveToLocation:
    case ActionType::MoveAwayFromObject:
    case ActionType::MoveAwayFromLocation:
    case ActionType::RandomWalk:
    case ActionType::Follow:
    case ActionType::FollowLeader:
    case ActionType::FollowOwner:
    case ActionType::ForceFollowObject: return canMove();
    case ActionType::AttackObject: return canAttack();
    case ActionType::UseFeat:
        return !isPhysicalAttackFeat(static_cast<const UseFeatAction &>(action).feat()) || canAttack();
    default: return canExecuteActions();
    }
}

bool Creature::isSelectable() const {
    bool hasDropableItems = false;
    for (auto &item : _items) {
        if (item->isDropable()) {
            hasDropableItems = true;
            break;
        }
    }
    return !isTemporarilyDead() && (!_dead || _selectableWhenDead || hasDropableItems);
}

bool Creature::isRunLimited() const {
    return _runLimited || (_stealthMode &&
        (!_game.isTSL() || !hasEffectiveFeat(FeatType::StealthRun)));
}

void Creature::beginSpellActivity(int spellId, bool itemCast) {
    if (itemCast) return;
    if (_game.isTSL()) switch (spellId) {
    case 181: case 182: case 184: case 200: case 201: case 269: return;
    default: break;
    }
    setStealthMode(false);
}

void Creature::setStealthMode(bool enabled) {
    if (_stealthMode == enabled) return;
    _stealthMode = enabled;
    _animDirty = true;
    if (isRunLimited() && _movementType == MovementType::Run)
        setMovementType(MovementType::Walk);
}

void Creature::updateMindTrickPerception(const Creature &target, bool heard, bool seen) {
    if (!_game.isTSL() || (!heard && !seen) || target.isStealthed()) return;
    if (_effectState != static_cast<int>(CreatureState::MindTrick) &&
        _effectState != static_cast<int>(CreatureState::DroidScramble)) return;
    if (getReputationToward(target) <= 10 && getSquareDistanceTo(target) <= 10.0f)
        removeMindTrickEffects();
}

void Creature::setExcitedState(uint8_t row) {
    const auto table = _services.resource.twoDas.get("excitedduration");
    if (!table) return;
    const float duration = table->getInt(row, "duration", 0) / 1000.0f;
    if (duration > _excitedTime) _excitedTime = duration;
}

void Creature::update(float dt) {
    _excitedTime = std::max(0.0f, _excitedTime - dt);
    Object::update(dt);
    updateForcePush(dt);
    updateStateHeartbeat(dt);
    updateModelAnimation();
    updateCombat(dt);
    updateLightsaberSoundPositions();
}

void Creature::updateModelAnimation() {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model)
        return;

    if (_animFireForget) {
        if (!model->isAnimationFinished())
            return;

        _animFireForget = false;
        _animDirty = true;
    }
    if (!_animDirty)
        return;

    std::shared_ptr<Animation> anim;
    std::shared_ptr<Animation> talkAnim;

    switch (_movementType) {
    case MovementType::Run:
        anim = model->model().getAnimation(getRunAnimation());
        break;
    case MovementType::Walk:
        anim = model->model().getAnimation(getWalkAnimation());
        break;
    default:
        if (_dead) {
            anim = model->model().getAnimation(getDeadAnimation());
        } else if (_effectAmbientState == 11) {
            anim = model->model().getAnimation(getAnimationName(AnimationType::LoopingSleep));
        } else if (_combatStance == CombatStance::Meditative) {
            anim = model->model().getAnimation(getAnimationName(AnimationType::LoopingMeditate));
        } else if (_talking) {
            anim = model->model().getAnimation(getTalkNormalAnimation());
            talkAnim = model->model().getAnimation(getHeadTalkAnimation());
        } else {
            anim = model->model().getAnimation(getPauseAnimation());
        }
        break;
    }

    if (talkAnim && anim) {
        model->playAnimation(*anim, nullptr, AnimationProperties::fromFlags(AnimationFlags::loopOverlay | AnimationFlags::propagate));
        model->playAnimation(*talkAnim, _lipAnimation, AnimationProperties::fromFlags(AnimationFlags::loopOverlay | AnimationFlags::propagate));
    } else {
        if (anim) {
            // The corpse pose is a short clip; looping/blending it makes the
            // model jerk and never settle, so play it once and hold the final
            // frame. Living poses keep looping and blending.
            int animFlags = _dead ? AnimationFlags::propagate
                                  : (AnimationFlags::loopBlend | AnimationFlags::propagate);
            model->playAnimation(*anim, nullptr, AnimationProperties::fromFlags(animFlags));
        }

        if (talkAnim) {
            model->playAnimation(*talkAnim, _lipAnimation, AnimationProperties::fromFlags(AnimationFlags::loopBlend | AnimationFlags::propagate));
        }
    }

    _animDirty = false;
}

int Creature::derivePermanentMaxHitPoints() const {
    int result = _attributes.getPermanentMaxHitPoints(_hitPoints);
    return std::clamp<int>(
        result,
        0,
        std::numeric_limits<int16_t>::max());
}

void Creature::updateDeathFromCurrentHitPoints() {
    if (_minOneHP && currentHitPoints() < 1) {
        _currentHitPoints = 1;
    }
    _dead = currentHitPoints() <= (_game.isTSL() && _isPC ? -10 : 0);
}

void Creature::restoreSerializedVitality() {
    // MaxHitPoints is a cache. HitPoints and CurrentHitPoints are both
    // serialized on the base-vitality axis, so reconstruct the runtime value
    // only after permanent attributes, levels and feats have been read.
    if (_hitPoints <= 0 && _maxHitPoints > 0) {
        // Preserve the long-standing tolerance for malformed/mod records that
        // omit HitPoints but provide the cached maximum.
        _hitPoints = _maxHitPoints;
    }

    const int serializedCurrent = _currentHitPoints;
    _maxHitPoints = derivePermanentMaxHitPoints();
    if (serializedCurrent > 0) {
        const int damage = static_cast<int>(_hitPoints) - serializedCurrent;
        _currentHitPoints = static_cast<int16_t>(std::clamp(
            static_cast<int>(_maxHitPoints) - damage,
            static_cast<int>(std::numeric_limits<int16_t>::min()),
            static_cast<int>(std::numeric_limits<int16_t>::max())));
    }
    updateDeathFromCurrentHitPoints();
}

int Creature::serializedCurrentHitPoints() const {
    if (_currentHitPoints <= 0) {
        return _currentHitPoints;
    }
    const int damage = static_cast<int>(_maxHitPoints) - _currentHitPoints;
    return std::clamp(
        static_cast<int>(_hitPoints) - damage,
        static_cast<int>(std::numeric_limits<int16_t>::min()),
        static_cast<int>(std::numeric_limits<int16_t>::max()));
}

void Creature::recalculatePermanentVitality() {
    const int oldMaximum = _maxHitPoints;
    const int oldCurrent = _currentHitPoints;
    _maxHitPoints = derivePermanentMaxHitPoints();
    if (oldCurrent > 0) {
        _currentHitPoints = static_cast<int16_t>(std::clamp(
            oldCurrent + static_cast<int>(_maxHitPoints) - oldMaximum,
            static_cast<int>(std::numeric_limits<int16_t>::min()),
            static_cast<int>(std::numeric_limits<int16_t>::max())));
    }
    updateDeathFromCurrentHitPoints();
}

void Creature::setMaxHitPoints(int baseHitPoints) {
    const int oldMaximum = _maxHitPoints;
    const int oldCurrent = _currentHitPoints;
    _hitPoints = static_cast<int16_t>(std::clamp(
        baseHitPoints,
        0,
        static_cast<int>(std::numeric_limits<int16_t>::max())));
    _maxHitPoints = derivePermanentMaxHitPoints();
    if (oldCurrent > 0) {
        _currentHitPoints = static_cast<int16_t>(std::clamp(
            oldCurrent + static_cast<int>(_maxHitPoints) - oldMaximum,
            static_cast<int>(std::numeric_limits<int16_t>::min()),
            static_cast<int>(std::numeric_limits<int16_t>::max())));
    }
    updateDeathFromCurrentHitPoints();
}

void Creature::setCurrentHitPoints(int hitPoints) {
    _currentHitPoints = static_cast<int16_t>(std::clamp(
        hitPoints,
        static_cast<int>(std::numeric_limits<int16_t>::min()),
        static_cast<int>(std::numeric_limits<int16_t>::max())));
    updateDeathFromCurrentHitPoints();
}

void Creature::initializeGeneratedVitality() {
    _hitPoints = static_cast<int16_t>(std::clamp(
        _attributes.getAggregateHitDie(),
        0,
        static_cast<int>(std::numeric_limits<int16_t>::max())));
    _maxHitPoints = derivePermanentMaxHitPoints();
    _currentHitPoints = _maxHitPoints;
    int64_t force = 0;
    _levelForcePoints.clear();
    for (const auto &[clazz, level] : _attributes.classLevels()) {
        const uint8_t gain = isForceUsingClass(clazz->type(), _game.isTSL())
            ? static_cast<uint8_t>(clazz->forcedie()) : 0;
        for (int i = 0; i < level; ++i) _levelForcePoints.push_back(gain);
        force += static_cast<int64_t>(level) * gain;
    }
    _forcePoints = narrowSignedResource(force);
    _currentForce = maxForcePoints();
    updateDeathFromCurrentHitPoints();
}

void Creature::damage(
    int amount,
    const std::shared_ptr<Object> &damager) {
    if (!_dead && amount >= 0) {
        // Direct damage has no typed packet. Publish a complete observation
        // here; effect damage has already published one before retaliation.
        std::array<int, 15> amounts;
        amounts.fill(-1);
        if (amount != std::numeric_limits<int>::max()) amounts.back() = amount;
        setLastDamager(damager);
        setLastDamageAmounts(amounts);
    }
    applyHitPointDamage(amount, damager);
}

void Creature::applyHitPointDamage(
    int amount,
    const std::shared_ptr<Object> &damager) {
    if (_dead) {
        return;
    }

    if (amount < 0) {
        // Heal instead of damage.
        int previousHitPoints = _currentHitPoints;
        _currentHitPoints = std::min(maxHitPoints(), _currentHitPoints - amount);
        _game.floatingText().addHeal(*this, _currentHitPoints - previousHitPoints);
        return;
    }

    bool deathEffect = amount == std::numeric_limits<int>::max();
    // Attribution belongs to this application, even when a nested hit has
    // replaced the public last-damage observation.
    const uint32_t damagerId = damager ? damager->id() : script::kObjectInvalid;
    if (deathEffect) {
        consumeTemporaryHitPoints(_temporaryHitPoints);
        if (_dead) return; // A removal callback may already have killed us.
        _currentHitPoints = 0; // special case for Death effect
    } else {
        const int totalBefore = currentHitPoints();
        const int allowedDamage = isMinOneHP()
            ? std::min(amount, std::max(0, totalBefore - 1)) : amount;
        const int remainingDamage = consumeTemporaryHitPoints(allowedDamage);
        if (_dead) return;
        _currentHitPoints = boundedResource(static_cast<int64_t>(_currentHitPoints) - remainingDamage);
        const int adjustedAmount = allowedDamage;
        if (amount > 0) {
            _game.floatingText().addDamage(
                *this, amount, adjustedAmount, damagerId);
        }
    }

    runDamagedScript();

    if (_immortal || currentHitPoints() > 0) {
        return;
    }

    (void)applyDeathEffect(damager, false);
}

int Creature::getReputationToward(const Creature &target) const {
    const bool sourceParty = _game.party().isMember(*this);
    const bool targetParty = _game.party().isMember(target);
    if (&target == this || (sourceParty && targetParty)) return 100;
    if (targetParty)
        return std::clamp(_services.game.reputes.getReputation(Faction::Player, faction()), 0, 100);
    return std::clamp(_services.game.reputes.getReputation(
        sourceParty ? Faction::Player : faction(), target.faction()), 0, 100);
}

void Creature::removeEffectsOnDeath() {
    const auto table = getRequiredTwoDA(_services.resource.twoDas, "removefxondeath");
    std::vector<uint64_t> refused;
    for (;;) {
        auto found = std::find_if(_effects.begin(), _effects.end(), [&](const EffectInstance &record) {
            return std::find(refused.begin(), refused.end(), record.applicationOrder) == refused.end() &&
                !isEffectPreservedOnDeath(record, *table, _game.isTSL());
        });
        if (found == _effects.end()) break;
        const auto id = found->id;
        const auto order = found->applicationOrder;
        removeEffectsById(id); // package removal can also remove exempt siblings
        if (findEffectApplication(order)) refused.push_back(order);
    }
}

void Creature::onDestroyabilityChanged() {
    if (_destroyable && _dead && !_game.party().isMember(*this) && !_noPermDeath)
        _game.queueObjectDestruction(*this, 3.0f);
}

bool Creature::applyDeathEffect(const std::shared_ptr<Object> &damager,
                               bool noFadeAway, const EffectInstance *operation) {
    if (_dead || _immortal || plotFlag()) return false;
    if (_minOneHP) { _currentHitPoints = 1; return false; }
    clearAllActions(true);
    _game.combat().cancelActions(*this);
    if (operation && shouldCheckDeathImmunity(operation->spellId, operation->semanticSubType())) {
        const auto creator = operation->boundCreator();
        if (hasEffectImmunity(ImmunityType::Death, dyn_cast<Creature>(creator).get())) return false;
    }
    const bool partyDeath = _game.party().isMember(*this);
    // A standalone Death effect is a new observation; HP depletion is not.
    // Keep a newer nested hit intact while attributing death to this source.
    if (operation) {
        std::array<int, 15> amounts;
        amounts.fill(-1);
        setLastDamager(damager);
        setLastDamageAmounts(amounts);
    }
    setLastHostileActor(damager ? damager->id() : script::kObjectInvalid);
    // K2's third Death parameter suppresses both XP and fade-away. K1 has
    // no third-parameter XP gate.
    if (!_game.isTSL() || !noFadeAway)
        applyDeathExperience(*this, damager, _name, _game, _services);
    finishCombatRound();
    _combatState.active = false;
    _combatState.activationType = 0;
    setClientCombatMode(false);
    _combatState.shouldDeactivate = false;
    _combatState.attackTarget.reset();
    _combatState.attackAction = ActionType::QueueEmpty;
    _combatState.combatFeat = FeatType::Invalid;
    runDeathScript();
    _dead = true;
    _currentHitPoints = std::min(-11, static_cast<int>(_currentHitPoints));
    if (!partyDeath) {
        if (!_livingName) _livingName = _name;
        _name = _services.resource.strings.getText(kStrRefRemains);
    }
    playSound(SoundSetEntry::Dead);
    playAnimation(getDieAnimation());
    removeEffectsOnDeath();
    _currentHitPoints = std::min(-11, static_cast<int>(_currentHitPoints));
    _dead = true;
    setMovementType(MovementType::None);
    const auto leader = _game.party().getLeader();
    if (leader.get() == this) {
        for (int index = 0; index < _game.party().getSize(); ++index) {
            const auto member = _game.party().getMember(index);
            if (member && member.get() != this && !member->isDead() && !member->isTemporarilyDead()) {
                _game.party().setPartyLeaderByIndex(index);
                break;
            }
        }
    }
    if (partyDeath || _noPermDeath || noFadeAway || !_destroyable) {
        _game.cancelObjectDestruction(*this);
    } else {
        const auto table = getRequiredTwoDA(_services.resource.twoDas, "appearance");
        _game.queueObjectDestruction(*this, readDestroyObjectDelay(*table, _appearance));
    }
    return true;
}

void Creature::applyHealingEffect(int amount, const std::shared_ptr<Object> &creator, bool quiet) {
    if (isDead() || isTemporarilyDead()) return;
    const int current = currentHitPointsWithoutTemporary();
    const int maximum = narrowSignedResource(maxHitPoints());
    const int sum = current + amount;
    if (sum > maximum) amount = maximum - current;
    _game.floatingText().addHeal(*this, amount);
    // The effect uses the direct HP setter; it is not a damage event.
    Object::setCurrentHitPoints(currentHitPointsWithoutTemporary() + amount);
    if (!_game.isTSL() || !quiet)
        addHitPointHealingFeedback(_game, _services, creator, *this, amount);
    // Heal restarts this live, sorted walk after each temporary Wounding package.
    for (size_t index = 0; index < _effects.size();) {
        const auto record = _effects[index];
        if (record.serializedType > 84) break;
        if (record.serializedType == 84 && record.durationType() == DurationType::Temporary) {
            if (!removeEffectsById(record.id)) break; // A reentrant removal may own it already.
            index = 0;
        } else {
            ++index;
        }
    }
}

bool Creature::applyResurrectionEffect(int hpPercent) {
    if (!_raiseable) return false;
    // Skip the write when the combined pool is already positive.
    // Writing that combined value to the ordinary pool would credit temp HP twice.
    if (currentHitPoints() <= 0) {
        Object::setCurrentHitPoints(getResurrectionHitPoints(
            _game.isTSL(), currentHitPoints(), maxHitPoints(), hpPercent));
    }
    _dead = currentHitPoints() <= 0;
    _game.cancelObjectDestruction(*this);
    clearAllActions(true);
    _game.combat().cancelActions(*this);
    finishCombatRound();
    _combatState.active = false;
    _combatState.activationType = 0;
    setClientCombatMode(false);
    _combatState.shouldDeactivate = false;
    _combatState.attackTarget.reset();
    _combatState.attackAction = ActionType::QueueEmpty;
    _combatState.combatFeat = FeatType::Invalid;
    _destroyable = true;
    _effectAIStateMask = 0xffff;
    _bodyFuel = false;
    resumeStateDrivenAnimation();
    if (!_dead && _livingName) { _name = *_livingName; _livingName.reset(); }
    _game.dismissDeathSequence(*this);
    removeResurrectionEffects(_effects, [this](EffectId id) { removeEffectsById(id); });
    _animDirty = true;
    return true;
}

void Creature::updateCombat(float dt) {
    _combatState.deactivationTimer.update(dt);
    _lightsaberIdlePowerDownTimer.update(dt);
    if (_lightsaberIdlePowerDownPending &&
        !_combatState.active &&
        _lightsaberIdlePowerDownTimer.elapsed()) {
        _lightsaberIdlePowerDownPending = false;
        setLightsabersPowered(false, true);
    }
    if (_combatState.shouldDeactivate && _combatState.deactivationTimer.elapsed()) {
        _combatState.active = false;
        _combatState.activationType = 0;
        setClientCombatMode(false);
        _combatState.shouldDeactivate = false;
        _combatState.debilitated = false;
        _combatState.attackTarget.reset();
        _combatState.attemptedAttackTarget.reset();
        _combatState.attackAction = ActionType::QueueEmpty;
        _combatState.combatFeat = FeatType::Invalid;
        _lastHostileTarget.reset();
        _lastAttackAction = ActionType::QueueEmpty;
        _lastCombatFeat = FeatType::Invalid;
        setLastHostileActor(script::kObjectInvalid);

        _animDirty = true;
        setLightsabersPowered(false, true);
    }
}

void Creature::clearAllActions(bool force) {
    Object::clearAllActions(force);
    setMovementType(MovementType::None);
}

void Creature::playAnimation(AnimationType type, AnimationProperties properties) {
    // If animation is looping by type and duration is -1.0, set flags accordingly
    bool looping = isAnimationLooping(type) && properties.duration == -1.0f;
    if (looping) {
        properties.flags |= AnimationFlags::loop;
    }

    std::string animName(getAnimationName(type));
    if (animName.empty())
        return;

    playAnimation(animName, std::move(properties));
}

void Creature::playAnimation(const std::string &name, AnimationProperties properties) {
    bool fireForget = !(properties.flags & AnimationFlags::loop);

    doPlayAnimation(fireForget, [&]() {
        auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
        if (model) {
            model->playAnimation(name, nullptr, properties);
        }
    });
}

bool Creature::doPlayAnimation(bool fireForget, const std::function<void()> &callback) {
    if (!_sceneNode || _movementType != MovementType::None) {
        return false;
    }

    callback();

    if (fireForget) {
        _animFireForget = true;
    }
    return true;
}

bool Creature::playAnimation(const std::shared_ptr<Animation> &anim, AnimationProperties properties) {
    bool fireForget = !(properties.flags & AnimationFlags::loop);

    return doPlayAnimation(fireForget, [&]() {
        auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
        if (model) {
            model->playAnimation(*anim, nullptr, properties);
        }
    });
}

bool Creature::playExternalAnimation(const std::shared_ptr<Animation> &anim, AnimationProperties properties) {
    properties.flags |= AnimationFlags::retargetRoot;
    bool started = doPlayAnimation(false, [&]() {
        auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
        if (model) {
            model->playAnimation(*anim, nullptr, properties);
        }
    });
    if (started) {
        // Dialogue owns the external clip until it explicitly releases the
        // participant. A pending state-driven refresh must not install an idle
        // or locomotion channel before the first stunt render update.
        _animDirty = false;
    }
    return started;
}

void Creature::playOverlayAnimation(AnimationType type) {
    std::string animName(getAnimationName(type));
    if (animName.empty()) {
        return;
    }
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model) {
        return;
    }
    // Deliberately not routed through doPlayAnimation: that refuses to play
    // anything while the creature is moving, which is the one case an overlay
    // exists to cover. _animFireForget is left alone too, so
    // updateModelAnimation goes on driving locomotion underneath the layer,
    // and the layer erases itself once it has finished.
    model->playAnimation(
        animName,
        nullptr,
        AnimationProperties::fromFlags(
            AnimationFlags::overlay |
            AnimationFlags::layer |
            AnimationFlags::fireForget |
            AnimationFlags::propagate));
}

void Creature::resumeStateDrivenAnimation() {
    _animFireForget = false;
    _animDirty = true;
    updateModelAnimation();
}

void Creature::playAnimation(CombatAnimation anim, CreatureWieldType wield, int variant) {
    std::string animName(getAnimationName(anim, wield, variant));
    if (!animName.empty()) {
        playAnimation(animName, AnimationProperties::fromFlags(AnimationFlags::blend));
    }
}

bool Creature::equip(const std::string &resRef) {
    std::shared_ptr<Item> item;
    if (isPresentationOnly()) {
        item = _game.newPresentationItem();
        item->loadFromBlueprint(resRef);
    } else {
        item = _game.newItemFromBlueprint(resRef);
    }

    bool equipped = false;

    if (item->isEquippable(InventorySlots::body)) {
        equipped = equip(InventorySlots::body, item);
    } else if (item->isEquippable(InventorySlots::rightWeapon)) {
        equipped = equip(InventorySlots::rightWeapon, item);
    }

    if (!equipped && item->isRuntimeLive()) {
        _game.destroyRuntimeObjectGraph(item);
    }

    return equipped;
}

void Creature::applyDisguiseAppearance(int appearance) {
    if (_disguised) return;
    _appearanceBeforeDisguise = static_cast<uint16_t>(_appearance);
    _appearance = static_cast<uint16_t>(appearance);
    _disguised = true;
    loadAppearanceProperties();
    if (_sceneNode) updateModel();
}

void Creature::removeDisguiseAppearance() {
    if (!_disguised) return;
    _appearance = _appearanceBeforeDisguise;
    loadAppearanceProperties();
    if (_sceneNode) updateModel();
    _disguised = false;
}

void Creature::updateDisguise() {
    // Presentation-only snapshots cannot admit effects. Live equipment instead
    // uses the same Disguise records as scripts, avoiding a second override.
    if (!isPresentationOnly()) return;
    int disguiseAppearance = -1;
    for (auto &[slot, item] : _equipment) {
        if (item->hasDisguise()) {
            disguiseAppearance = item->disguiseAppearance();
            break;
        }
    }
    if (disguiseAppearance >= 0) {
        if (!_disguised) {
            _appearanceBeforeDisguise = _appearance;
            _disguised = true;
        }
        _appearance = static_cast<uint32_t>(disguiseAppearance);
    } else if (_disguised) {
        _appearance = _appearanceBeforeDisguise;
        _disguised = false;
    }
}

bool Creature::canEquip(
    int slot,
    const std::shared_ptr<Item> &item) const {
    if (!item || (!item->isRuntimeLive() && !item->isPresentationOnly()) ||
        !item->isEquippable(getEquipabilitySlot(slot))) {
        return false;
    }
    if (std::find(_items.begin(), _items.end(), item) != _items.end()) {
        // Inventory -> equipment is a transfer. Callers must first remove or
        // split the candidate so one object never has two ownership edges.
        return false;
    }
    for (const auto &[equippedSlot, equipped] : _equipment) {
        if (equipped == item && equippedSlot != slot) {
            return false;
        }
    }
    auto previous = getEquippedItem(slot);
    if (previous != item && item->owner() != 0 &&
        item->owner() != script::kObjectInvalid) {
        return false;
    }
    if (previous && previous != item) return false;
    return true;
}

bool Creature::equip(int slot, const std::shared_ptr<Item> &item) {
    if (!canEquip(slot, item) || isActiveAreaOwnedItem(_game, item)) {
        return false;
    }
    if (getEquippedItem(slot) == item) {
        return true;
    }
    auto replacementEffects = effectsWithoutEquippedSource(nullptr);
    appendEquippedItemEffects(replacementEffects, slot, item);

    _equipment[slot] = item;
    item->setEquipped(true);
    item->setOwner(_id);
    replaceEffectState(std::move(replacementEffects));

    updateEquipmentPresentation();
    if (_sceneNode && _combatState.active &&
        (slot == InventorySlots::rightWeapon ||
         slot == InventorySlots::leftWeapon)) {
        setLightsabersPowered(true, true);
    }

    return true;
}

bool Creature::canReplaceEquipment(
    int slot,
    const std::shared_ptr<Item> &item,
    const Object &displacedReceiver) const {
    auto previous = getEquippedItem(slot);
    if (!previous || previous == item) return canEquip(slot, item);
    if (!item || (!item->isRuntimeLive() && !item->isPresentationOnly()) ||
        !item->isEquippable(getEquipabilitySlot(slot)) ||
        std::find(_items.begin(), _items.end(), item) != _items.end() ||
        (item->owner() != 0 && item->owner() != script::kObjectInvalid)) {
        return false;
    }
    for (const auto &[equippedSlot, equipped] : _equipment) {
        if (equipped == item && equippedSlot != slot) return false;
    }
    if ((!previous->isRuntimeLive() && !previous->isPresentationOnly()) ||
        (!displacedReceiver.isRuntimeLive() &&
         !displacedReceiver.isPresentationOnly()) ||
        (isPresentationOnly() != displacedReceiver.isPresentationOnly())) {
        return false;
    }
    return true;
}

bool Creature::replaceEquipment(
    int slot,
    const std::shared_ptr<Item> &item,
    Object &displacedReceiver) {
    auto previous = getEquippedItem(slot);
    if (!previous || previous == item) return equip(slot, item);
    if (!canReplaceEquipment(slot, item, displacedReceiver) ||
        isActiveAreaOwnedItem(_game, item)) {
        return false;
    }
    auto replacementEffects = effectsWithoutEquippedSource(previous.get());
    appendEquippedItemEffects(replacementEffects, slot, item);

    // All ordinary recoverable validation is complete. The remainder is one
    // synchronous ownership move; only exceptional allocation failure remains.
    previous->powerDown(_position);
    previous->setEquipped(false);
    previous->setOwner(0);
    _equipment[slot] = item;
    item->setEquipped(true);
    item->setOwner(_id);
    displacedReceiver.addItem(previous);
    replaceEffectState(std::move(replacementEffects));
    updateEquipmentPresentation();
    return true;
}

std::shared_ptr<Item> Creature::takeEquippedItem(
    const std::shared_ptr<Item> &item) {
    auto equipped = std::find_if(
        _equipment.begin(), _equipment.end(),
        [&item](const auto &entry) { return entry.second == item; });
    if (equipped == _equipment.end()) return nullptr;

    auto result = equipped->second;
    auto replacementEffects = effectsWithoutEquippedSource(result.get());
    result->powerDown(_position);
    result->setEquipped(false);
    result->setOwner(0);
    _equipment.erase(equipped);
    replaceEffectState(std::move(replacementEffects));
    updateEquipmentPresentation();
    return result;
}

bool Creature::moveEquippedItemTo(
    const std::shared_ptr<Item> &item,
    Object &receiver) {
    if (!item ||
        (!receiver.isRuntimeLive() && !receiver.isPresentationOnly()) ||
        (isPresentationOnly() != receiver.isPresentationOnly())) {
        return false;
    }
    auto taken = takeEquippedItem(item);
    if (!taken) return false;
    receiver.addItem(taken);
    return true;
}

void Creature::updateEquipmentPresentation() {
    uint32_t prevAppearance = _appearance;
    updateDisguise();
    if (_appearance != prevAppearance) {
        // Refresh appearance-derived state so the in-place model swap below picks
        // up the disguise (or restored) model; rebuilding the scene node here would
        // orphan it from the area scene graph.
        loadAppearanceProperties();
    }
    if (_sceneNode) updateModel();
}

std::shared_ptr<Item> Creature::getEquippedItem(int slot) const {
    auto equipped = _equipment.find(slot);
    return equipped != _equipment.end() ? equipped->second : nullptr;
}

bool Creature::isSlotEquipped(int slot) const {
    return _equipment.find(slot) != _equipment.end();
}

void Creature::setMovementType(MovementType type) {
    if (_movementType == type)
        return;

    _movementType = type;
    _animDirty = true;
    _animFireForget = false;
}

glm::vec3 Creature::getSelectablePosition() const {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model) {
        return _position;
    }
    if (_dead) {
        return model->getWorldCenterOfAABB();
    }
    auto headModel = static_cast<ModelSceneNode *>(model->getAttachment(g_headHookNode));
    if (headModel) {
        auto talkDummy = headModel->getNodeByName(g_talkDummyNode);
        return talkDummy ? talkDummy->origin() : headModel->getWorldCenterOfAABB();
    } else {
        auto talkDummy = model->getNodeByName(g_talkDummyNode);
        return talkDummy ? talkDummy->origin() : model->getWorldCenterOfAABB();
    }
}

void Creature::setAttemptedSpellTarget(uint32_t id) {
    _combatState.attemptedSpellTarget = _game.getObjectById(id);
}
float Creature::maxCleaveRange(const Creature *target) const {
    auto weapon = getEquippedItem(InventorySlots::rightWeapon);
    if (weapon && weapon->isRanged()) return 22.0f;
    const auto table = _services.resource.twoDas.get("appearance");
    const float sourceRadius = table ? table->getFloat(_appearance, "hitradius") : 0.0f;
    const float targetRadius = table && target ? table->getFloat(target->_appearance, "hitradius") : 0.0f;
    return target ? sourceRadius + targetRadius + 4.1f : 4.0f;
}

float Creature::getAttackRange() const {
    float result = kDefaultAttackRange;

    std::shared_ptr<Item> item(getEquippedItem(InventorySlots::rightWeapon));
    if (item && item->attackRange() > kDefaultAttackRange) {
        result = item->attackRange();
    }

    return result;
}

bool Creature::isLevelUpPending() const {
    return _xp >= getNeededXP();
}

int Creature::getNeededXP() const {
    int level = _attributes.getAggregateLevel();
    return level * (level + 1) * 500;
}

void Creature::runSpawnScript() {
    if (_game.isTSL() && !_autoBalancePlayerLevelAtSpawnSet) {
        _autoBalanceContext.playerLevelAtSpawn =
            static_cast<uint8_t>(_game.getGlobalNumber("G_PC_LEVEL"));
        _autoBalancePlayerLevelAtSpawnSet = true;
    }
    // The game gates the creation script on CreatnScrptFird, so it fires at most
    // once per creature rather than once per area attachment. A party member
    // carried through an ordinary module transition is the same creature
    // object: the destination area takes it in without recreating it, and its
    // OnSpawn must not run a second time. The game also latches the flag
    // regardless of whether a script was authored.
    if (_spawnScriptFired) {
        return;
    }
    _spawnScriptFired = true;
    if (!_onSpawn.empty()) {
        _game.scriptRunner().run(_onSpawn, _id);
    }
}

void Creature::runBlockedScript(uint32_t blockingDoorId) {
    if (_onBlocked.empty()) {
        return;
    }
    // The obstructing door travels with the run as an argument, so every
    // continuation and delayed action started from it goes on seeing the door
    // this event was raised for, whatever the creature has run into since.
    _game.scriptRunner().run(
        _onBlocked,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::BlockingDoor, Variable::ofObject(blockingDoorId)}});
}

void Creature::runEndRoundScript() {
    if (!_onEndRound.empty()) {
        _game.scriptRunner().run(_onEndRound, _id);
    }
}

void Creature::runDialogueScript(uint32_t speakerId, int32_t listenNumber) {
    _game.scriptRunner().run(
        _onDialogue,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastSpeaker, Variable::ofObject(speakerId)},
         {script::ArgKind::ListenPatternNumber, Variable::ofInt(listenNumber)}});
}

void Creature::giveXP(int amount) {
    setXP(_xp + amount);
}

void Creature::setXP(int xp) {
    bool wasLevelUpPending = isLevelUpPending();
    _xp = xp;

    if (!wasLevelUpPending && isLevelUpPending()) {
        _game.notifyLevelUpPending(*this);
    }
}

void Creature::playSound(SoundSetEntry entry, bool positional) {
    if (!_soundSet) {
        return;
    }
    auto maybeSound = _soundSet->find(entry);
    if (maybeSound == _soundSet->end()) {
        return;
    }
    std::optional<glm::vec3> position;
    if (positional) {
        position = _position + glm::vec3 {0.0f, 0.0f, 1.7f};
    }
    _audioSourceVoice = _services.audio.mixer.play(
        maybeSound->second,
        AudioType::Sound,
        1.0f,
        false,
        std::move(position));
}

void Creature::runAttackedScript(uint32_t attackerId) {
    if (_onAttacked.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onAttacked,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastAttacker, Variable::ofObject(attackerId)}});
}

void Creature::runDamagedScript() {
    if (_onDamaged.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onDamaged,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastAttacker, Variable::ofObject(getLastDamager())},
         {script::ArgKind::LastDamager, Variable::ofObject(getLastDamager())}});
}

void Creature::runDeathScript() {
    if (_onDeath.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onDeath,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastAttacker, Variable::ofObject(getLastDamager())},
         {script::ArgKind::LastDamager, Variable::ofObject(getLastDamager())}});
}

CreatureWieldType Creature::getWieldType() const {
    auto rightWeapon = getEquippedItem(InventorySlots::rightWeapon);
    auto leftWeapon = getEquippedItem(InventorySlots::leftWeapon);

    if (rightWeapon && leftWeapon) {
        return (rightWeapon->weaponWield() == WeaponWield::BlasterPistol) ? CreatureWieldType::DualPistols : CreatureWieldType::DualSwords;
    } else if (rightWeapon) {
        switch (rightWeapon->weaponWield()) {
        case WeaponWield::SingleSword:
            return CreatureWieldType::SingleSword;
        case WeaponWield::DoubleBladedSword:
            return CreatureWieldType::DoubleBladedSword;
        case WeaponWield::BlasterPistol:
            return CreatureWieldType::BlasterPistol;
        case WeaponWield::BlasterRifle:
            return CreatureWieldType::BlasterRifle;
        case WeaponWield::HeavyWeapon:
            return CreatureWieldType::HeavyWeapon;
        case WeaponWield::StunBaton:
        default:
            return CreatureWieldType::StunBaton;
        }
    }

    if (hasEffectiveFeat(FeatType::ComplexUnarmedAnims)) {
        return CreatureWieldType::HandToHandComplex;
    }

    return CreatureWieldType::HandToHand;
}

void Creature::startTalking(const std::shared_ptr<LipAnimation> &animation) {
    if (!_talking || _lipAnimation != animation) {
        _lipAnimation = animation;
        _talking = true;
        _animDirty = true;
    }
}

void Creature::stopTalking() {
    if (_talking || _lipAnimation) {
        _lipAnimation.reset();
        _talking = false;
        _animDirty = true;
    }
}

void Creature::setObjectSeen(const std::shared_ptr<Object> &object, bool seen) {
    if (seen) {
        _perception.seen[object->id()] = object;
    } else {
        _perception.seen.erase(object->id());
    }
}

void Creature::setObjectHeard(const std::shared_ptr<Object> &object, bool heard) {
    if (heard) {
        _perception.heard[object->id()] = object;
    } else {
        _perception.heard.erase(object->id());
    }
}

void Creature::runOnNotice(const Object &object, bool heard, bool seen) {
    // Execute onNotice once to handle both "heard" and "seen" perception
    // checks. k_ai_master script checks them in sequence, and performs
    // differently when an object is just "heard" assuming that it is not
    // "seen".

    if (_onNotice.empty()) {
        return;
    }

    _game.scriptRunner().run(
        _onNotice,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastPerceived, Variable::ofObject(object.id())},
         {script::ArgKind::LastPerceptionHeard, Variable::ofInt(heard)},
         {script::ArgKind::LastPerceptionInaudible, Variable::ofInt(!heard)},
         {script::ArgKind::LastPerceptionSeen, Variable::ofInt(seen)},
         {script::ArgKind::LastPerceptionVanished, Variable::ofInt(!seen)}});
}

void Creature::activateCombat(uint8_t activationType) {
    _lightsaberIdlePowerDownPending = false;
    if (_combatState.activationType != 1) _combatState.activationType = activationType;
    setClientCombatMode(true);
    if (_combatState.active) {
        _combatState.shouldDeactivate = false;
        return;
    }
    _combatState.active = true;
    _combatState.shouldDeactivate = false;
    _animDirty = true;
    setLightsabersPowered(true, true);
}

void Creature::applyFuryState(int spellId) {
    _furyDamageBonus = 0;
    switch (spellId) {
    case 164: case 271: _furySpellState = 7; break;
    case 165: case 272: _furySpellState = 8; break;
    case 166: case 273: _furySpellState = 9; break;
    default: break;
    }
}

void Creature::clearFuryState() {
    _furyDamageBonus = -1;
    _furySpellState = 0;
}

void Creature::incrementFuryDamageBonus() {
    if (!_game.isTSL()) return;
    if (_furyDamageBonus >= 0 && _furyDamageBonus <= 5) {
        ++_furyDamageBonus;
    }
}

void Creature::setLightsabersPowered(bool powered, bool animate) {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model) {
        return;
    }

    const std::array<std::pair<int, std::string>, 2> weaponSlots {{
        {InventorySlots::rightWeapon, g_rightHandNode},
        {InventorySlots::leftWeapon, g_leftHandNode},
    }};
    for (auto [slot, attachment] : weaponSlots) {
        auto weapon = static_cast<ModelSceneNode *>(model->getAttachment(attachment));
        if (!weapon || weapon->model().classification() != MdlClassification::lightsaber) {
            continue;
        }

        const auto activeAnimation = weapon->activeAnimationName();
        if ((powered && (activeAnimation == "powerup" || activeAnimation == "powered")) ||
            (!powered && (activeAnimation == "powerdown" || activeAnimation == "off"))) {
            continue;
        }
        weapon->playAnimation(animate ? (powered ? "powerup" : "powerdown") : (powered ? "powered" : "off"));
        if (!animate) {
            continue;
        }
        auto item = getEquippedItem(slot);
        if (item) {
            powered ? item->powerUp(_position) : item->powerDown(_position);
        }
    }
}

void Creature::updateLightsaberSoundPositions() {
    for (int slot : {InventorySlots::rightWeapon, InventorySlots::leftWeapon}) {
        auto item = getEquippedItem(slot);
        if (item) {
            item->updatePoweredSoundPosition(_position);
        }
    }
}

void Creature::deactivateCombat(float delay) {
    if (delay <= 0.0f) {
        _lightsaberIdlePowerDownPending = false;
        _combatState.active = false;
        _combatState.activationType = 0;
        setClientCombatMode(false);
        _combatState.attackTarget.reset();
        _combatState.shouldDeactivate = false;
        _combatState.debilitated = false;
        _animDirty = true;
        setLightsabersPowered(false, true);
        return;
    }
    if (!_combatState.active) {
        return;
    }
    _combatState.shouldDeactivate = true;
    _combatState.deactivationTimer.reset(delay);
}

bool Creature::isTwoWeaponFighting() const {
    return static_cast<bool>(getOffhandAttackWeapon());
}

std::shared_ptr<Item> Creature::getOffhandAttackWeapon() const {
    auto main = getEquippedItem(InventorySlots::rightWeapon);
    if (!main) {
        return nullptr;
    }

    int relativeSize = static_cast<int>(main->weaponSize()) -
                       static_cast<int>(_size);
    if (relativeSize > 0) {
        return main->weaponWield() == WeaponWield::DoubleBladedSword
                   ? main
                   : nullptr;
    }

    auto offhand = getEquippedItem(InventorySlots::leftWeapon);
    if (!offhand ||
        offhand->weaponType() == WeaponType::None ||
        offhand->weaponWield() == WeaponWield::None) {
        return nullptr;
    }
    return offhand;
}

void Creature::beginCombatAttack(Object &targetObject, FeatType feat) {
    setStealthMode(false);
    auto target = _game.getObjectById(targetObject.id());
    if (target) {
        removeCombatInvisibilityEffects();
    }
    _combatState.attackTarget = target;
    _combatState.attackAction = ActionType::AttackObject;
    _combatState.combatFeat = feat;
}

void Creature::setAttemptedAttackTarget(uint32_t target) {
    _combatState.attemptedAttackTarget = _game.getObjectById(target);
}

void Creature::recordQueuedAttack(Creature &target) {
    const auto leader = _game.party().getLeader();
    if (!target.isPC() && leader.get() != &target) return;
    target._incomingAttacker = SavedObjectReference::fromRuntimeId(id());
    _game.bindSavedObjectReference(target._incomingAttacker);
}

uint32_t Creature::getFirstAttacker() {
    auto module = _game.module();
    auto area = module ? module->area() : nullptr;
    const auto *objects = area && area->isObjectResident(*this) ? &area->objects() : nullptr;
    return _attackerList.first(objects,
        [this](const auto &object) {
            const auto creature = dyn_cast<Creature>(object);
            return creature && creature->getAttackTarget().get() == this;
        }, [](const auto &object) { return object->id(); });
}

void Creature::setClientCombatMode(bool active) {
    _clientCombatMode = active;
    _game.syncClientCombatMode();
}

void Creature::removeCombatInvisibilityEffects() {
    std::vector<EffectId> packages;
    for (const auto &effect : effects()) {
        if ((effect.type() == EffectType::Invisibility &&
             effect.integerParameter(0) == static_cast<int>(InvisibilityType::Normal)) ||
            effect.type() == EffectType::Sanctuary) {
            if (std::find(packages.begin(), packages.end(), effect.id) == packages.end())
                packages.push_back(effect.id);
        }
    }
    for (auto id : packages) removeEffectsById(id);
}

void Creature::removeMindTrickEffects() {
    if (!_game.isTSL()) return;
    std::vector<EffectId> packages;
    for (const auto &effect : effects()) {
        if (effect.serializedType > 8) break;
        if (effect.serializedType != 8) continue;
        const auto state = static_cast<CreatureState>(effect.integerParameter(0));
        if (state != CreatureState::MindTrick && state != CreatureState::DroidScramble) continue;
        if (std::find(packages.begin(), packages.end(), effect.id) == packages.end())
            packages.push_back(effect.id);
    }
    for (auto id : packages) removeEffectsById(id);
}

void Creature::broadcastCombatState(uint32_t opponentId) {
    auto opponent = _game.getObjectById<Creature>(opponentId);
    if (!opponent) return;
    const auto peerTarget = opponent->getAttackTarget();
    const auto peerAttempt = opponent->_combatState.attemptedAttackTarget.resolve();
    auto activationType = [&](const Creature &member) -> uint8_t {
        return peerTarget.get() == &member || peerAttempt.get() == &member ? 1 : 2;
    };
    if (opponent.get() == this || getReputationToward(*opponent) <= 10)
        activateCombat(activationType(*this));
    auto module = _game.module();
    auto area = module ? module->area() : nullptr;
    if (!area || !area->isObjectResident(*this)) return;
    // Snapshot membership before any presentation or script can change the area.
    const auto members = area->getObjectsByType(ObjectType::Creature);
    for (const auto &object : members) {
        auto member = std::static_pointer_cast<Creature>(object);
        if (!member->isRuntimeLive() || member.get() == this || member->faction() != faction()) continue;
        const float range = isPC() ? 30.0f : member->perception().sightRange;
        if (!forceAlwaysUpdate() && getSquareDistanceTo(*member) > range * range) continue;
        if (member->getReputationToward(*opponent) <= 10)
            member->activateCombat(activationType(*member));
    }
}

void Creature::receiveAttackEvent(const AttackHistory *history, uint32_t attackerId,
                                  const AttackEventFields *fields) {
    // Capture borrowed values before callbacks can retire their owning round.
    if (history) _receivedAttack = *history;
    const uint8_t weaponType = fields ? fields->weaponAttackType : 0;
    resolveInitiative();
    setExcitedState(1);
    auto attacker = _game.getObjectById<Creature>(attackerId);
    std::shared_ptr<Item> weapon;
    if (attacker) {
        attacker->resolveInitiative();
        attacker->setExcitedState(2);
        switch (weaponType) {
        case 1: case 6: weapon = attacker->getEquippedItem(InventorySlots::rightWeapon); break;
        case 2: weapon = attacker->getEquippedItem(InventorySlots::leftWeapon); break;
        case 3: weapon = attacker->getEquippedItem(InventorySlots::cWeaponL); break;
        case 4: weapon = attacker->getEquippedItem(InventorySlots::cWeaponR); break;
        case 5: weapon = attacker->getEquippedItem(InventorySlots::cWeaponB); break;
        case 7: case 8: weapon = attacker->getEquippedItem(InventorySlots::hands); break;
        default: break;
        }
    }
    _savedReferences["LastAttacker"] = _game.getObjectById(attackerId);
    setLastHostileActor(attackerId);
    if (attacker) {
        attacker->broadcastCombatState(id());
        attacker->removeCombatInvisibilityEffects();
        attacker->_lastWeaponUsed = weapon ? weapon->id() : script::kObjectInvalid;
    }
    if (!isRuntimeLive()) return;
    broadcastCombatState(attackerId);
    if (!isDead() && (!isPC() || currentHitPoints() > 0)) {
        runAttackedScript(attackerId);
        if (isRuntimeLive()) removeMindTrickEffects();
    }
}

void Creature::cancelCombat(int runEndRound) {
    clearCommandActions();
    deactivateCombat(0.0f);
    setLastHostileActor(script::kObjectInvalid);
    discardHostileActionGroups();
    _game.combat().endRound(*this, runEndRound);
    _combatState.attackTarget.reset();
    setClientCombatMode(false);
}

void Creature::refreshBodyFuel() {
    if (_bodyFuel) _currentForce = maxForcePoints();
}

void Creature::finishCombatRound() {
    const auto incoming = _incomingAttacker.boundObject();
    if (!incoming || incoming->isDead()) _incomingAttacker = SavedObjectReference {};
    _attackerList.clear();
    if (auto target = _combatState.attackTarget.resolve()) {
        _lastHostileTarget = target;
    } else {
        _lastHostileTarget.reset();
    }
    _lastAttackAction = _combatState.attackAction;
    if (_combatState.combatFeat != FeatType::Invalid) {
        _lastCombatFeat = _combatState.combatFeat;
    }
}

void Creature::adjustModifiedAttacks(int amount) {
    _modifiedAttacks += amount;
    // K1 really saturates on writes. K2 preserves raw state and caps its
    // contribution in CalculateOnHandAttacks instead.
    if (!_game.isTSL()) {
        _modifiedAttacks = std::clamp(_modifiedAttacks, 0, 2);
    }
}

void Creature::onEffectsCleared() {
    _effectState = _effectAmbientState = 0;
    _internalStateEffectId = _activeStateRootId = 0;
    _activePoisonEffectId = kUnassignedEffectId;
    _effectAIStateMask = 0xffff;
    _bodyFuel = false;
    _throwParryBlocked = false;
    _effectIconCounts.clear();
    _hasted = _slowed = false;
    _movementRate = 1.0f;
    restoreMovementAfterState();
    _modifiedAttacks = 0;
    _assuredHit = false;
    removeAssuredDeflection();
}

bool Creature::applyAssuredHit() {
    if (_assuredHit) {
        return false;
    }
    _assuredHit = true;
    return true;
}

bool Creature::applyAssuredDeflection(int returnDamage) {
    if (_assuredDeflection || _assuredReturn) return false;
    _assuredDeflection = true;
    _assuredReturn = returnDamage != 0;
    return true;
}

Alignment Creature::alignment() const {
    if (_goodEvil <= 40) {
        return Alignment::DarkSide;
    }
    if (_goodEvil >= 60) {
        return Alignment::LightSide;
    }
    return Alignment::Neutral;
}

bool Creature::isEffectLinkImmune(const Effect &effect) const {
    if (const auto *link = dynamic_cast<const LinkEffectsEffect *>(&effect)) {
        return isEffectLinkImmune(*link->childEffect()) ||
               isEffectLinkImmune(*link->parentEffect());
    }
    EffectInstance record = effect.saveFacingInstance();
    int state = record.serializedType == 8 ? record.integerParameter(0) : 0;
    auto row = getEffectImmunityRow(_game.isTSL(), record.serializedType, state);
    if (!row) return false;
    auto table = getRequiredTwoDA(_services.resource.twoDas, "gameeffects");
    validateTwoDARow(*table, "gameeffects", *row);
    const int count = _game.isTSL() ? 34 : 33;
    if (table->getColumnCount() < count + 1) {
        throw ValidationException("Incomplete gameeffects.2da immunity columns");
    }
    for (int immunity = 0; immunity < count; ++immunity) {
        auto value = table->getIntOpt(*row, table->columns()[immunity + 1]);
        if (!value) {
            throw ValidationException("Missing gameeffects.2da immunity value");
        }
        // GetEffectLinkImmunity deliberately queries with no
        // opposing creature; retained creator is for feedback/application.
        if (*value != 0 && hasEffectImmunity(static_cast<ImmunityType>(immunity))) {
            return true;
        }
    }
    return false;
}

bool Creature::hasEffectImmunity(
    ImmunityType immunityType, const Creature *creator) const {
    for (const EffectInstance &applied : effects()) {
        if (!applied.hasLiveRuntimeSource()) {
            continue;
        }
        if (applied.type() == EffectType::Immunity &&
            (applied.integerParameter(0) == static_cast<int>(immunityType) ||
             applied.integerParameter(0) == static_cast<int>(ImmunityType::All)) &&
            applied.appliesVersus(creator)) {
            return true;
        }
    }
    return false;
}

int Creature::getAbilityEffectModifier(Ability ability) const {
    AbilityEffectReducer reducer(_game.isTSL());
    for (const EffectInstance &effect : effects()) {
        if (!effect.hasLiveRuntimeSource() ||
            effect.integerParameter(0, -1) != static_cast<int>(ability)) continue;
        if (effect.type() == EffectType::AbilityIncrease)
            reducer.addIncrease(getEffectSourceKey(effect), effect.integerParameter(1));
        else if (effect.type() == EffectType::AbilityDecrease)
            reducer.addDecrease(getEffectSourceKey(effect), effect.integerParameter(1));
    }
    return reducer.total();
}

int Creature::getEffectiveAbilityScore(Ability ability) const {
    auto races = getRequiredTwoDA(_services.resource.twoDas, "racialtypes");
    const int racial = readRacialAbilityAdjustment(*races, static_cast<int>(_race), ability);
    return getAbilityScoreFromParts(_attributes.getAbilityScore(ability),
                                    getAbilityEffectModifier(ability), racial);
}

int Creature::getEffectiveAbilityModifier(Ability ability) const {
    int score = getEffectiveAbilityScore(ability);
    return score >= 10 ? (score - 10) / 2 : (score - 11) / 2;
}

bool Creature::hasEffectiveFeat(FeatType feat) const {
    feat = static_cast<FeatType>(static_cast<uint16_t>(feat));
    if (_attributes.hasFeat(feat)) {
        return true;
    }
    return std::any_of(
        effects().begin(), effects().end(),
        [feat](const EffectInstance &effect) {
            return effect.hasLiveRuntimeSource() &&
                   effect.type() == EffectType::BonusFeat &&
                   static_cast<uint16_t>(effect.integerParameter(0)) ==
                       static_cast<uint16_t>(feat);
        });
}

void Creature::appendEquippedItemEffects(
    std::deque<EffectInstance> &effects,
    int slot,
    const std::shared_ptr<Item> &item, bool onlyDeferredEffects) const {
    if (!item || !equippedItemPropertiesAreActive(slot)) {
        return;
    }

    auto append = [&](std::shared_ptr<Effect> effect) {
        if (!effect) {
            return;
        }
        effect->setSaveFacingCreator(item);
        EffectInstance instance = effect->saveFacingInstance();
        instance.effect = std::move(effect);
        instance.id = _game.allocateEffectId();
        instance.subType = static_cast<uint16_t>(
            (instance.subType & ~static_cast<uint16_t>(0x7)) |
            static_cast<uint16_t>(DurationType::Equipped));
        instance.creatorId = item->id();
        instance.exposed = 1;
        // Item-property application forwards load mode to ApplyEffect;
        // equipped duration alone does not bypass admission checks.
        instance.restoring = isRestoringSavedRuntime();
        effects.push_back(std::move(instance));
    };

    for (const Item::PropertyEntry &property : item->properties()) {
        if (!item->isPropertyActive(property)) {
            continue;
        }
        const auto propertyType = static_cast<ItemProperty>(property.propertyName);
        const bool armorClass = propertyType == ItemProperty::AcBonus ||
            propertyType == ItemProperty::AcBonusVsAlignmentGroup ||
            propertyType == ItemProperty::AcBonusVsDamageType ||
            propertyType == ItemProperty::AcBonusVsRacialGroup ||
            propertyType == ItemProperty::DecreasedAc;
        const bool deferred = armorClass || propertyType == ItemProperty::Disguise;
        if (onlyDeferredEffects && !deferred) continue;
        // Restore canonical AC/disguise records first, then reconcile missing
        // equipped records without replaying or duplicating the saved providers.
        if (!onlyDeferredEffects && deferred && isRestoringSavedRuntime()) continue;
        switch (propertyType) {
        case ItemProperty::AcBonus:
        case ItemProperty::AcBonusVsAlignmentGroup:
        case ItemProperty::AcBonusVsDamageType:
        case ItemProperty::AcBonusVsRacialGroup: {
            int amount = getCostTableValue(_services, kBonusCostTable, property.costValue, "value", 0);
            if (amount == 0) break;
            const int selector = propertyType == ItemProperty::AcBonusVsDamageType
                ? property.subtype : kPhysicalDamageTypeFlags;
            auto effect = _game.newEffect<ACIncreaseEffect>(amount, item->acBonusType(), selector);
            if (propertyType == ItemProperty::AcBonusVsRacialGroup)
                effect->setVersusRacialType(property.subtype);
            else if (propertyType == ItemProperty::AcBonusVsAlignmentGroup &&
                     property.subtype >= 1 && property.subtype <= 3)
                effect->setVersusAlignment(0, property.subtype);
            append(std::move(effect));
            break;
        }
        case ItemProperty::DecreasedAc: {
            int amount = getCostTableValue(_services, kDecreaseCostTable, property.costValue, "value", 0);
            if (amount != 0) append(_game.newEffect<ACDecreaseEffect>(
                -amount,
                static_cast<ACBonus>(property.subtype), kPhysicalDamageTypeFlags));
            break;
        }
        case ItemProperty::AbilityBonus: {
            int amount = getItemPropertyValue(
                _services, property, "value", 0);
            if (amount > 0) {
                append(_game.newEffect<AbilityIncreaseEffect>(
                    static_cast<Ability>(property.subtype), amount));
            }
            break;
        }
        case ItemProperty::DecreasedAbilityScore: {
            int amount = getItemPropertyValue(
                _services, property, "value", 0);
            if (amount > 0) {
                append(_game.newEffect<AbilityDecreaseEffect>(
                    static_cast<Ability>(property.subtype), amount));
            }
            break;
        }
        case ItemProperty::Disguise:
            append(_game.newEffect<DisguiseEffect>(property.subtype));
            break;
        case ItemProperty::BonusFeat:
            append(_game.newEffect<BonusFeatEffect>(
                static_cast<FeatType>(property.subtype)));
            break;
        case ItemProperty::Immunity: {
            static constexpr std::array<ImmunityType, 10> immunities {
                ImmunityType::SneakAttack, ImmunityType::AbilityDecrease,
                ImmunityType::MindSpells, ImmunityType::Poison, ImmunityType::Disease,
                ImmunityType::Fear, ImmunityType::Knockdown, ImmunityType::Paralysis,
                ImmunityType::CriticalHit, ImmunityType::Death};
            append(_game.newEffect<ImmunityEffect>(immunities.at(property.subtype)));
            break;
        }
        case ItemProperty::ImmunityDamageType: {
            int amount = getItemPropertyValue(
                _services, property, "value", 0);
            if (amount > 0) {
                append(_game.newEffect<DamageImmunityIncreaseEffect>(
                    getItemPropertyDamageType(_services, property.subtype),
                    amount));
            }
            break;
        }
        case ItemProperty::DamageVulnerability: {
            if (plotFlag()) {
                break;
            }
            int amount = getCostTableValue(
                _services,
                kVulnerabilityCostTable,
                property.costValue,
                "value",
                0);
            if (amount > 0) {
                append(_game.newEffect<DamageImmunityDecreaseEffect>(
                    getItemPropertyDamageType(_services, property.subtype),
                    amount));
            }
            break;
        }
        case ItemProperty::DamageResistance: {
            int amount = getCostTableValue(
                _services,
                kResistanceCostTable,
                property.costValue,
                "amount",
                0);
            if (amount > 0) {
                append(_game.newEffect<DamageResistanceEffect>(
                    getItemPropertyDamageType(_services, property.subtype),
                    amount,
                    0));
            }
            break;
        }
        case ItemProperty::DamageReduction: {
            int amount = getCostTableValue(
                _services,
                kReductionCostTable,
                property.costValue,
                "amount",
                0);
            if (amount > 0) {
                append(_game.newEffect<DamageReductionEffect>(
                    amount,
                    getDamageReductionPower(_services, property.subtype),
                    0));
            }
            break;
        }
        case ItemProperty::Regeneration:
        case ItemProperty::RegenerationForcePoints:
            if (property.costValue != 0) {
                auto effect = _game.newEffect<RegenerateEffect>(property.costValue, 6000, property.propertyName);
                effect->setSubType(0);
                append(std::move(effect));
            }
            break;
        case ItemProperty::TrueSeeing:
            append(_game.newEffect<TrueSeeingEffect>());
            break;
        default:
            break;
        }
    }
}

std::deque<EffectInstance> Creature::effectsWithoutEquippedSource(
    const Item *source) const {
    std::deque<EffectInstance> result;
    for (const EffectInstance &effect : effects()) {
        auto creator = effect.boundCreator();
        if (source && effect.durationType() == DurationType::Equipped &&
            creator.get() == source) {
            continue;
        }
        result.push_back(effect);
    }
    return result;
}

std::deque<EffectInstance> Creature::rebuildEquippedItemEffects(
    const std::map<int, std::shared_ptr<Item>> &equipment) const {
    std::deque<EffectInstance> result;
    for (const EffectInstance &effect : effects()) {
        if (effect.durationType() != DurationType::Equipped) {
            result.push_back(effect);
        }
    }
    std::set<const Item *> seen;
    for (const auto &[slot, item] : equipment) {
        if (item && seen.insert(item.get()).second) {
            appendEquippedItemEffects(result, slot, item);
        }
    }
    return result;
}

AttackBonusBreakdown Creature::getAttackBonusBreakdown(
    const Creature *target,
    const Item *weapon,
    bool offHand) const {

    AttackBonusBreakdown result;

    int strengthModifier = getEffectiveAbilityModifier(Ability::Strength);
    int dexterityModifier = getEffectiveAbilityModifier(Ability::Dexterity);

    if (weapon && weapon->isRanged()) {
        result.dexterityModifier = dexterityModifier;
    } else if (weapon && dexterityModifier > strengthModifier) {
        bool finesse = qualifiesForWeaponFinesse(
            _game.isTSL(),
            weapon->isLightsaber(),
            weapon->weaponWield(),
            hasEffectiveFeat(FeatType::FinesseLightsabers),
            hasEffectiveFeat(FeatType::FinesseMeleeWeapons));

        if (finesse) {
            result.dexterityModifier = dexterityModifier;
        } else {
            result.strengthModifier = strengthModifier;
        }
    } else {
        result.strengthModifier = strengthModifier;
    }

    int modifierBonus = 0;
    int modifierPenalty = 0;
    EffectModifierReducer miscModifierReducer;

    for (const auto &applied : effects()) {
        if (!applied.hasLiveRuntimeSource()) {
            continue;
        }
        if (!applied.appliesVersus(target)) {
            continue;
        }
        auto modifierType = static_cast<AttackBonus>(
            applied.integerParameter(1));
        switch (applied.type()) {
        case EffectType::AttackIncrease: {
            int bonus = applied.integerParameter(0);
            if (bonus > 0 &&
                attackModifierApplies(modifierType, weapon, offHand)) {
                if (modifierType == AttackBonus::Misc) {
                    miscModifierReducer.addIncrease(
                        getEffectSourceKey(applied),
                        static_cast<int>(AttackBonus::Misc),
                        bonus);
                } else {
                    modifierBonus += bonus;
                }
            }
            break;
        }
        case EffectType::AttackDecrease: {
            int penalty = applied.integerParameter(0);
            if (penalty > 0 &&
                attackModifierApplies(modifierType, weapon, offHand)) {
                if (modifierType == AttackBonus::Misc) {
                    miscModifierReducer.addDecrease(
                        getEffectSourceKey(applied),
                        static_cast<int>(AttackBonus::Misc),
                        penalty);
                } else {
                    modifierPenalty += penalty;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    for (const auto &[slot, item] : _equipment) {
        if (!item || !equippedItemAppliesToAttack(slot, *item, weapon, offHand)) {
            continue;
        }

        int itemBonus = 0;
        int itemPenalty = 0;
        for (const auto &property : item->properties()) {
            if (!item->isPropertyActive(property)) {
                continue;
            }

            int modifier = 0;
            auto propertyType = static_cast<ItemProperty>(property.propertyName);
            switch (propertyType) {
            case ItemProperty::EnhancementBonus:
            case ItemProperty::EnhancementBonusVsAlignmentGroup:
            case ItemProperty::EnhancementBonusVsRacialGroup:
            case ItemProperty::AttackBonus:
            case ItemProperty::AttackBonusVsAlignmentGroup:
            case ItemProperty::AttackBonusVsRacialGroup: {
                if (!attackPropertyApplies(
                        propertyType,
                        property.subtype,
                        target)) {
                    continue;
                }
                int value = getCostTableValue(
                    _services,
                    kMeleeCostTable,
                    property.costValue,
                    "value",
                    0);
                if (value <= 0) {
                    continue;
                }
                modifier = value;
                break;
            }
            case ItemProperty::AttackPenalty:
            case ItemProperty::DecreasedAttackModifier: {
                int value = getCostTableValue(
                    _services,
                    kDecreaseCostTable,
                    property.costValue,
                    "value",
                    0);
                if (value >= 0) {
                    continue;
                }
                modifier = value;
                break;
            }
            default:
                continue;
            }

            if (isHandSpecificAttackModifierSlot(slot)) {
                addAttackModifier(modifier, modifierBonus, modifierPenalty);
            } else if (modifier > 0) {
                itemBonus = std::max(itemBonus, modifier);
            } else {
                itemPenalty = std::max(itemPenalty, -modifier);
            }
        }
        EffectSourceKey source {EffectSourceKind::Item, item->runtimeIncarnation()};
        miscModifierReducer.addIncrease(source, static_cast<int>(AttackBonus::Misc), itemBonus);
        miscModifierReducer.addDecrease(source, static_cast<int>(AttackBonus::Misc), itemPenalty);
    }

    int attackEffectCap = getAttackEffectModifierCap(_game.isTSL());
    modifierBonus += miscModifierReducer.totalIncrease(attackEffectCap);
    modifierPenalty += miscModifierReducer.totalDecrease(attackEffectCap);
    result.effectBonus = std::min(modifierBonus, attackEffectCap) -
                         std::min(modifierPenalty, attackEffectCap);

    if (weapon &&
        weapon->weaponFocusFeat() != FeatType::Invalid &&
        hasEffectiveFeat(weapon->weaponFocusFeat())) {
        result.weaponFocusBonus = 1;
    }

    result.targetingBonus = getTargetingAttackBonus(
        _game.isTSL(),
        weapon && weapon->isRanged(),
        getHighestOwnedFeatRank(*this,
            FeatType::Targeting1,
            10));
    result.superiorWeaponFocusBonus =
        getSuperiorWeaponFocusLightsaberBonus(
            _game.isTSL(),
            weapon && weapon->isLightsaber(),
            getHighestOwnedFeatRank(*this,
                FeatType::SuperiorWeaponFocusLightsaber1,
                3));

    auto rightHandWeapon = getEquippedItem(InventorySlots::rightWeapon);
    auto targetRightHandWeapon = target
                                     ? target->getEquippedItem(
                                           InventorySlots::rightWeapon)
                                     : nullptr;
    result.formBonus = target
                           ? getLightsaberFormAttackBonus(
                                 _game.isTSL(),
                                 rightHandWeapon && rightHandWeapon->isLightsaber(),
                                 currentForm(),
                                 targetRightHandWeapon && targetRightHandWeapon->isLightsaber())
                           : 0;

    if (_game.isTSL() && target) {
        bool rank1 = hasEffectiveFeat(FeatType::DualStrike);
        bool rank2 = hasEffectiveFeat(FeatType::ImprovedDualStrike);
        bool rank3 = hasEffectiveFeat(FeatType::MasterDualStrike);
        bool qualifies = qualifiesForDualStrike(true, isPartyMember(), rank1, rank2, rank3);
        bool matchingAlly = false;
        if (qualifies) {
            for (const auto &member : _game.party().members()) {
                if (member.creature && member.creature.get() != this &&
                    member.creature->_combatState.attemptedAttackTarget.resolve().get() == target) {
                    matchingAlly = true;
                    break;
                }
            }
        }
        result.dualStrikeBonus = getDualStrikeAttackBonus(
            qualifies, matchingAlly, rank1, rank2, rank3);
    }

    if (target) {
        getSituationalAttackBonuses(
            *this,
            *target,
            weapon,
            result.closeProximityRangedBonus,
            result.meleeOnRangedBonus);
    }

    int twoWeaponPenalty = getTwoWeaponAttackPenalty(
        weapon,
        offHand,
        &result.smallOffhandBonus);
    result.dualWieldPenalty = -twoWeaponPenalty - result.smallOffhandBonus;

    if (!offHand) {
        result.duelingBonus = getDuelingBonus();
        switch (result.duelingBonus) {
        case 3:
            result.duelingFeat = FeatType::MasterDueling;
            break;
        case 2:
            result.duelingFeat = FeatType::ImprovedDueling;
            break;
        case 1:
            result.duelingFeat = FeatType::Dueling;
            break;
        default:
            break;
        }
    }

    result.baseAttackBonus = _attributes.getAggregateAttackBonus();
    if (isAutoBalanceEligible(
            _game.isTSL(),
            isPartyMember(),
            _autoBalanceContext.multiplierSet)) {
        const AutoBalanceRow &row = _services.game.autoBalance.get(
            _autoBalanceContext.multiplierSet);
        result.baseAttackBonus += getAutoBalanceLevelBonus(
            _autoBalanceContext.playerLevelAtSpawn,
            row.toHitMultiplier);
    }
    return result;
}

bool Creature::projectileDefenseEligible(const Creature &shooter, int damageFlags,
    const Item *weapon, bool allowShield, bool &shieldHit, bool &canReturn) const {
    canReturn = true;
    shieldHit = false;
    const bool canAct = !isDead() &&
        (_game.isTSL()
             ? (_effectState == 0 || _effectState == 1 || _effectState == 16) &&
                   (!isPC() || currentHitPoints() > 0) && combatStance() != CombatStance::Meditative
             : _effectState == 0 && !isTemporarilyDead());
    const bool facingShot = std::cos(getFacing() - shooter.getFacing()) <= 0.0f;
    const auto main = getEquippedItem(InventorySlots::rightWeapon);
    const auto off = getEquippedItem(InventorySlots::leftWeapon);
    const bool saber = (main && main->isLightsaber()) || (off && off->isLightsaber());
    const bool defense = hasEffectiveFeat(static_cast<FeatType>(55)) ||
        hasEffectiveFeat(static_cast<FeatType>(1)) || hasEffectiveFeat(static_cast<FeatType>(24));
    if (defense && canAct) {
        if (!_game.isTSL()) return saber && !_throwParryBlocked && facingShot;
        if (saber && !_throwParryBlocked && facingShot &&
            (damageFlags & static_cast<int>(DamageType::Blaster))) return true;
    }
    // A missed shot may strike a shield without passing an opposed defense roll.
    if (allowShield) {
        const auto shield = std::find_if(effects().begin(), effects().end(), [](const EffectInstance &effect) {
            return effect.type() == EffectType::ForceShield;
        });
        shieldHit = shield != effects().end() && shield->integerParameter(0) != 0;
        const auto ammunition = weapon ? weapon->ammunitionType() : nullptr;
        if (shieldHit && ammunition && ammunition->shieldHit) return true;
    }
    if (!_game.isTSL() || !canAct || _throwParryBlocked || !facingShot) return false;
    const bool redirect = _attributes.hasSpell(static_cast<SpellType>(163));
    canReturn = redirect;
    return redirect || _attributes.hasSpell(static_cast<SpellType>(162));
}

bool Creature::canParryRangedWeapon(const Creature &shooter, int damageFlags, bool &canReturn) const {
    bool shieldHit = false;
    return projectileDefenseEligible(shooter, damageFlags, nullptr, false, shieldHit, canReturn);
}

AttackResultType Creature::resolveRangedMiss(const Creature &shooter, const Item &weapon) const {
    bool shieldHit = false;
    bool canReturn = false;
    if (!projectileDefenseEligible(shooter, weapon.damageFlags(), &weapon, true, shieldHit, canReturn)) {
        return AttackResultType::Invalid;
    }
    return shieldHit ? AttackResultType::ShieldHit : AttackResultType::Parried;
}

AttackResultType Creature::resolveRangedDefense(const Creature &shooter, int damageFlags, int attackTotal) const {
    bool canReturn = false;
    if (!canParryRangedWeapon(shooter, damageFlags, canReturn)) return AttackResultType::Invalid;
    if (_assuredDeflection) {
        return _assuredReturn ? AttackResultType::Deflected : AttackResultType::Parried;
    }
    const auto weapon = getEquippedItem(InventorySlots::rightWeapon);
    int total = randomInt(1, 20) + getAttackBonusBreakdown(nullptr, weapon.get(), false).baseAttackBonus +
        getEffectiveAbilityModifier(Ability::Dexterity);
    if (hasEffectiveFeat(static_cast<FeatType>(24))) total += 6;
    else if (hasEffectiveFeat(static_cast<FeatType>(1))) total += 3;
    if (_game.isTSL()) {
        if (_attributes.hasSpell(static_cast<SpellType>(163))) total += 3;
        const auto off = getEquippedItem(InventorySlots::leftWeapon);
        const bool saber = (weapon && weapon->isLightsaber()) || (off && off->isLightsaber());
        if (saber && hasEffectiveFeat(static_cast<FeatType>(168)))
            total += (_attributes.getClassLevel(static_cast<ClassType>(11)) + 1) / 2;
        if (saber) switch (static_cast<int>(_currentForm)) {
        case 259: total -= 5; break;
        case 260: total += 4; break;
        case 261: total -= 4; break;
        case 262: total += 2; break;
        case 263: total += 1; break;
        default: break;
        }
        for (int feat = 244; feat >= 240; --feat) {
            if (shooter.hasEffectiveFeat(static_cast<FeatType>(feat))) {
                total -= 2 * (feat - 239); break;
            }
        }
    }
    for (const auto &effect : effects()) {
        // The defensive consumer reads integer 1, independently of the VM constructor.
        if (effect.serializedType == 92) total += effect.integerParameter(1);
        else if (effect.serializedType == 93) total -= effect.integerParameter(1);
    }
    if (total < attackTotal) return AttackResultType::Invalid;
    return canReturn && total >= attackTotal + 6 ? AttackResultType::Deflected : AttackResultType::Parried;
}

int Creature::getAttackBonus(bool offHand) const {
    auto weapon = offHand
                      ? getOffhandAttackWeapon()
                      : getEquippedItem(InventorySlots::rightWeapon);
    return getAttackBonusBreakdown(nullptr, weapon.get(), offHand).total();
}

static bool isArmorClassEffect(const EffectInstance &effect) {
    return effect.type() == EffectType::ACIncrease || effect.type() == EffectType::ACDecrease;
}

static ArmorClassEffectData armorClassEffectData(const EffectInstance &effect) {
    return {effect.integerParameter(0), effect.integerParameter(1),
            effect.integerParameter(2, static_cast<int>(RacialType::All)),
            effect.integerParameter(3), effect.integerParameter(4),
            effect.integerParameter(5, kPhysicalDamageTypeFlags),
            effect.type() == EffectType::ACDecrease};
}

void Creature::addArmorClassEffect(const EffectInstance &effect) {
    if (isArmorClassEffect(effect)) _armorClassCache.add(armorClassEffectData(effect), _game.isTSL());
}

void Creature::removeArmorClassEffect(const EffectInstance &effect) {
    if (!isArmorClassEffect(effect)) return;
    const auto removed = armorClassEffectData(effect);
    const auto maximum = remainingArmorClassMaximum(_armorClassCursor, effects().size(), removed,
        [&](size_t i) { return effects()[i].serializedType; },
        [&](size_t i) { return armorClassEffectData(effects()[i]); },
        [&](size_t i) { return effects()[i].applicationOrder == effect.applicationOrder; });
    _armorClassCache.remove(removed, _game.isTSL(), maximum);
}

void Creature::updateArmorClassEffectCursor() {
    _armorClassCursor.update(effects().size(), [&](size_t i) { return effects()[i].serializedType; });
}

DefenseBreakdown Creature::getDefenseBreakdown(const Creature *attacker, int damageFlags) const {
    int dexterityModifier = getEffectiveAbilityModifier(Ability::Dexterity);
    auto armor = getEquippedItem(InventorySlots::body);
    int armorDefense = armor ? armor->baseDefense() : 0;

    int armorMaxDexterityBonus = armor
        ? getEffectiveArmorMaxDexterityBonus(_game.isTSL(), armor->maxDexterityBonus(),
                                            armor->maxDexterityBonusAdjustment()) : -1;
    if (isDebilitated()) {
        dexterityModifier = std::min(dexterityModifier, 0);
    } else if (armorDefense > 0 && armorMaxDexterityBonus >= 0) {
        dexterityModifier = std::min(
            dexterityModifier,
            armorMaxDexterityBonus);
    }

    ConditionalArmorClass conditional(_armorClassCache);
    bool attackerSeen = true;
    if (attacker) {
        for (size_t i = _armorClassCursor.position; i < effects().size(); ++i) {
            const auto &effect = effects()[i];
            if (effect.serializedType != 48 && effect.serializedType != 49) break;
            if (!isArmorClassEffect(effect) || !effect.hasLiveRuntimeSource()) continue;
            conditional.add(armorClassEffectData(effect), static_cast<int>(attacker->racialType()),
                            static_cast<int>(attacker->alignment()), damageFlags);
        }
        const bool invisible = attacker->isInvisibleTo(*this);
        const bool seen = _perception.sees(attacker->id());
        attackerSeen = !invisible && seen;
        if (invisible) dexterityModifier = std::min(dexterityModifier, 0);
        else if (!seen) dexterityModifier = 0;
    }
    const auto ac = armorClassParts(_armorClassCache, conditional, _naturalAC, armorDefense,
                                 0, attacker != nullptr, attackerSeen);
    DefenseBreakdown breakdown;
    breakdown.armor = ac.armour;
    breakdown.dexterity = dexterityModifier;
    breakdown.classDefense = _attributes.getAggregateDefenseBonus();
    if (isAutoBalanceEligible(
            _game.isTSL(),
            isPartyMember(),
            _autoBalanceContext.multiplierSet)) {
        const AutoBalanceRow &row = _services.game.autoBalance.get(
            _autoBalanceContext.multiplierSet);
        breakdown.classDefense += getAutoBalanceLevelBonus(
            _autoBalanceContext.playerLevelAtSpawn,
            row.armorClassMultiplier);
    }
    breakdown.natural = ac.natural;
    breakdown.dodgeAndDeflection = armorClassShort(ac.shield + ac.dodgeAndDeflection);
    breakdown.feat = getDuelingBonus();
    breakdown.stance = getTotalDefenseBonus(
        _game.isTSL(),
        combatStance() == CombatStance::TotalDefense,
        getHighestTotalDefenseClassLevel(_attributes));

    bool attackerIsCombatTarget = false;
    if (attacker) {
        attackerIsCombatTarget =
            _combatState.attemptedAttackTarget.resolve().get() == attacker ||
            _combatState.attackTarget.resolve().get() == attacker;
    }
    auto rightHandWeapon = getEquippedItem(InventorySlots::rightWeapon);
    breakdown.form = getLightsaberFormDefenseBonus(
        _game.isTSL(),
        rightHandWeapon && rightHandWeapon->isLightsaber(),
        currentForm(),
        attackerIsCombatTarget);
    breakdown.debilitationPenalty = isDebilitated() ? -4 : 0;
    breakdown.total = armorClassShort(10 + breakdown.armor + breakdown.dexterity +
        breakdown.classDefense + breakdown.natural + breakdown.dodgeAndDeflection +
        breakdown.feat + breakdown.stance + breakdown.form + breakdown.debilitationPenalty);
    return breakdown;
}

int Creature::getDefense() const {
    return getDefense(nullptr, 0);
}

static int conditioningBonus(const Creature &creature) {
    if (creature.hasEffectiveFeat(FeatType::LightningReflexes)) {
        return 3;
    }
    if (creature.hasEffectiveFeat(FeatType::IronWill)) {
        return 2;
    }
    return creature.hasEffectiveFeat(FeatType::GreatFortitude) ? 1 : 0;
}

static int getClassSavingThrow(
    const CreatureAttributes &attributes,
    int requestedSave) {

    const SavingThrows result = attributes.getAggregateSavingThrows();

    switch (requestedSave) {
    case kFortitudeSavingThrow:
        return result.fortitude;
    case kReflexSavingThrow:
        return result.reflex;
    case kWillSavingThrow:
        return result.will;
    default:
        return 0;
    }
}

int Creature::getSavingThrowEffectBonus(
    SavingThrow savingThrow, SavingThrowType savingThrowType, const Object *versus) const {
    const int save = static_cast<int>(savingThrow);

    const auto *versusCreature = dyn_cast<Creature>(versus);
    EffectModifierReducer modifierReducer;
    for (const EffectInstance &applied : effects()) {
        if (!applied.hasLiveRuntimeSource() || !applied.appliesVersus(versusCreature)) continue;
        if (applied.type() != EffectType::SavingThrowIncrease &&
            applied.type() != EffectType::SavingThrowDecrease) continue;
        int effectSave = applied.integerParameter(1);
        auto effectType = static_cast<SavingThrowType>(applied.integerParameter(2));
        if (!savingThrowModifierApplies(effectSave, effectType, save, savingThrowType)) continue;
        auto source = getEffectSourceKey(applied);
        int value = applied.integerParameter(0);
        if (applied.type() == EffectType::SavingThrowIncrease) {
            modifierReducer.addIncrease(source, 0, value);
        } else {
            modifierReducer.addDecrease(source, 0, value);
        }
    }

    std::set<uint64_t> visitedItems;
    for (const auto &[slot, item] : _equipment) {
        if (!item ||
            slot == InventorySlots::rightWeapon2 ||
            slot == InventorySlots::leftWeapon2 ||
            !visitedItems.insert(item->runtimeIncarnation()).second) {
            continue;
        }

        EffectSourceKey source {EffectSourceKind::Item, item->runtimeIncarnation()};
        for (const Item::PropertyEntry &property : item->properties()) {
            if (!item->isPropertyActive(property)) {
                continue;
            }

            auto propertyType =
                static_cast<ItemProperty>(property.propertyName);
            if (!savingThrowPropertyApplies(
                    propertyType,
                    property.subtype,
                    save,
                    savingThrowType)) {
                continue;
            }

            int value = getItemPropertyValue(
                _services,
                property,
                "value",
                0);
            if (propertyType == ItemProperty::ImprovedSavingThrow ||
                propertyType == ItemProperty::ImprovedSavingThrowSpecific) {
                modifierReducer.addIncrease(source, 0, value);
            } else {
                modifierReducer.addDecrease(source, 0, value);
            }
        }
    }

    const int survival = getSurvivalSavingThrowBonus(
        _game.isTSL(), hasEffectiveFeat(FeatType::Survival), currentHitPoints(), maxHitPoints());
    const int cap = _game.isTSL() ? 60 : 20;
    return getSavingThrowEffectTotal(_game.isTSL(),
        modifierReducer.totalIncrease(cap), modifierReducer.totalDecrease(cap), survival);
}

int Creature::getSavingThrowBase(SavingThrow savingThrow) const {
    const int save = static_cast<int>(savingThrow);
    int abilityModifier = 0;
    int baseBonus = 0;
    switch (save) {
    case kFortitudeSavingThrow:
        abilityModifier =
            getEffectiveAbilityModifier(Ability::Constitution);
        baseBonus = _fortBonus;
        break;
    case kReflexSavingThrow:
        abilityModifier = getEffectiveAbilityModifier(Ability::Dexterity);
        baseBonus = _refBonus;
        break;
    case kWillSavingThrow:
        abilityModifier = getEffectiveAbilityModifier(Ability::Wisdom);
        baseBonus = _willBonus;
        break;
    default:
        break;
    }

    int autoBalanceBonus = 0;
    if (isAutoBalanceEligible(
            _game.isTSL(),
            isPartyMember(),
            _autoBalanceContext.multiplierSet)) {
        const AutoBalanceRow &row = _services.game.autoBalance.get(
            _autoBalanceContext.multiplierSet);
        autoBalanceBonus = getAutoBalanceLevelBonus(
            _autoBalanceContext.playerLevelAtSpawn,
            row.savingThrowMultiplier);
    }

    const int base = getBaseSavingThrowBonus(getClassSavingThrow(_attributes, save),
        conditioningBonus(*this), autoBalanceBonus);
    return getDerivedSavingThrowBonus(base, abilityModifier, baseBonus);
}

int Creature::getSavingThrow(SavingThrow save) const {
    return getSavingThrowStat(getSavingThrowBase(save),
        getSavingThrowEffectBonus(save, SavingThrowType::All, nullptr));
}

SavingThrowBreakdown Creature::getSavingThrowBreakdown(
    SavingThrow save, SavingThrowType type, const Object *versus) const {
    const int base = getSavingThrowBase(save);
    const int effects = getSavingThrowEffectBonus(save, type, versus);
    const int survival = getSurvivalSavingThrowBonus(
        _game.isTSL(), hasEffectiveFeat(FeatType::Survival), currentHitPoints(), maxHitPoints());
    int room = 0;
    auto module = _game.module();
    if (_game.isTSL() && module && module->area()) {
        room = getRoomSavingThrowModifier(true, _goodEvil,
            module->area()->getRoomForceRating(position()));
    }
    return {base, getSavingThrowModifier(_game.isTSL(), effects, room, survival)};
}

SavingThrowResult Creature::getSavingThrowResult(
    int total, int dc, SavingThrowType type, const Object *versus) const {
    return resolveSavingThrow(total, dc, [&] {
        int userType = -1;
        if (type == SavingThrowType::Death && spellCast() != SpellType::All) {
            auto spell = _services.game.spells.get(spellCast());
            if (spell) userType = spell->userType;
        }
        const auto immunity = savingThrowImmunity(type, userType);
        return immunity && hasEffectImmunity(*immunity, dyn_cast<Creature>(versus));
    });
}

SavingThrowResult Creature::rollSavingThrow(
    SavingThrow save, int dc, SavingThrowType type, const Object *versus) const {
    const auto breakdown = getSavingThrowBreakdown(save, type, versus);
    return getSavingThrowResult(randomInt(1, 20) + breakdown.total(), dc, type, versus);
}

PhysicalDamageBonus Creature::getPhysicalDamageBonus(
    const Item *weapon,
    bool offHand) const {

    int strengthModifier = getEffectiveAbilityModifier(Ability::Strength);
    int abilityModifier = strengthModifier;

    if (weapon && weapon->isRanged()) {
        if (strengthModifier > 0) {
            int mighty = 0;
            for (const auto &property : weapon->properties()) {
                if (!weapon->isPropertyActive(property) ||
                    property.propertyName != static_cast<uint16_t>(ItemProperty::Mighty)) {
                    continue;
                }
                mighty = property.costValue;
                break;
            }
            abilityModifier = std::min(strengthModifier, mighty);
        }
    } else if (strengthModifier > 0) {
        if (offHand) {
            abilityModifier = strengthModifier / 2;
        } else if (weapon &&
                   weapon->weaponWield() != WeaponWield::DoubleBladedSword &&
                   static_cast<int>(weapon->weaponSize()) ==
                       static_cast<int>(_size) + 1) {
            abilityModifier = 3 * strengthModifier / 2;
        }
    }

    int specialization = 0;
    if (weapon &&
        weapon->weaponSpecializationFeat() != FeatType::Invalid &&
        hasEffectiveFeat(weapon->weaponSpecializationFeat())) {
        specialization = 2;
    }

    PhysicalDamageBonus result {abilityModifier, strengthModifier, specialization};
    if (_game.isTSL() && _furyDamageBonus > 0) {
        result.furyDamage = _furyDamageBonus;
    }
    if (_game.isTSL()) {
        bool ranged = weapon && weapon->isRanged();
        result.combatFeatDamage = getCombatDamageFeatBonus(true, ranged,
            getHighestOwnedFeatRank(*this, FeatType::IncreaseCombatDamage1, 3),
            getHighestOwnedFeatRank(*this, FeatType::IncreaseMeleeDamage1, 3));
        result.preciseShotDamage = getPreciseShotDamageBonus(true, ranged,
            getHighestOwnedFeatRank(*this, FeatType::PreciseShot, 5));
        auto rightHand = getEquippedItem(InventorySlots::rightWeapon);
        result.formDamage = getLightsaberFormDamageBonus(true,
            rightHand && rightHand->isLightsaber(), currentForm());
        if (!weapon) {
            // The existing round builder's ordinary empty-hand attack is
            // type 7. Natural-weapon slot construction is a later owner.
            result.unarmedDice209 = getHighestOwnedFeatRank(*this, static_cast<FeatType>(209), 3);
            result.unarmedDice212 = getHighestOwnedFeatRank(*this, static_cast<FeatType>(212), 8);
        }
    }
    return result;
}

int Creature::getPhysicalDamageAutoBalanceFactor() const {
    if (!isAutoBalanceEligible(
            _game.isTSL(),
            isPartyMember(),
            _autoBalanceContext.multiplierSet)) {
        return 1;
    }

    const AutoBalanceRow &row = _services.game.autoBalance.get(
        _autoBalanceContext.multiplierSet);
    return getAutoBalanceDamageFactor(
        _autoBalanceContext.playerLevelAtSpawn,
        row.damageMultiplier);
}

int Creature::getMassiveCriticalDamage(
    const Item *weapon,
    bool criticalHit) const {

    if (!criticalHit) {
        return 0;
    }

    auto handItem = weapon
                        ? std::shared_ptr<Item>()
                        : getEquippedItem(InventorySlots::hands);
    const Item *sourceItem = weapon ? weapon : handItem.get();
    if (!sourceItem) {
        return 0;
    }

    for (const auto &property : sourceItem->properties()) {
        if (!sourceItem->isPropertyActive(property) ||
            property.propertyName != static_cast<uint16_t>(ItemProperty::MassiveCriticals)) {
            continue;
        }

        auto modifier = getDamageModifier(
            _services,
            property.costValue,
            weapon
                ? getPrimaryDamageType(sourceItem->damageFlags())
                : DamageType::Bludgeoning);
        return modifier
                   ? rollDamageModifier(*modifier, 1)
                   : 0;
    }

    return 0;
}

void Creature::addPhysicalDamageModifiers(
    DamagePacket &damage,
    DamageBreakdown &breakdown,
    const Creature *target,
    const Item *weapon,
    bool offHand,
    int criticalMultiplier) const {

    struct SourcedDamageModifier {
        EffectSourceKey source;
        DamageModifier modifier;
    };
    std::vector<SourcedDamageModifier> itemBonuses;
    std::vector<SourcedDamageModifier> itemPenalties;

    auto handItem = weapon
                        ? std::shared_ptr<Item>()
                        : getEquippedItem(InventorySlots::hands);
    const Item *sourceItem = weapon ? weapon : handItem.get();

    for (const auto &[slot, item] : _equipment) {
        if (!item || !equippedItemAppliesToAttack(slot, *item, weapon, offHand)) {
            continue;
        }

        std::map<int, DamageModifier> bonuses;
        std::map<int, DamageModifier> penalties;

        for (const auto &property : item->properties()) {
            if (!item->isPropertyActive(property)) {
                continue;
            }

            auto propertyType = static_cast<ItemProperty>(property.propertyName);
            if (!damagePropertyApplies(
                    propertyType,
                    property.subtype,
                    target)) {
                continue;
            }

            switch (propertyType) {
            case ItemProperty::EnhancementBonus:
            case ItemProperty::EnhancementBonusVsAlignmentGroup:
            case ItemProperty::EnhancementBonusVsRacialGroup: {
                auto modifier = getFlatDamageModifier(
                    _services,
                    kMeleeCostTable,
                    property.costValue,
                    !weapon && item.get() == sourceItem
                        ? DamageType::Bludgeoning
                        : getPrimaryDamageType(item->damageFlags()));
                if (!modifier || modifier->flat <= 0) {
                    break;
                }
                selectDamageModifier(bonuses, *modifier);
                break;
            }
            case ItemProperty::DamageBonus: {
                DamageType type = getItemPropertyDamageType(
                    _services,
                    property.subtype);
                auto modifier = getDamageModifier(
                    _services,
                    property.costValue,
                    type);
                if (modifier) {
                    selectDamageModifier(bonuses, *modifier);
                }
                break;
            }
            case ItemProperty::DamageBonusVsAlignmentGroup:
            case ItemProperty::DamageBonusVsRacialGroup: {
                DamageType type = getItemPropertyDamageType(
                    _services,
                    property.paramValue);
                auto modifier = getDamageModifier(
                    _services,
                    property.costValue,
                    type);
                if (modifier) {
                    selectDamageModifier(bonuses, *modifier);
                }
                break;
            }
            case ItemProperty::DecreasedDamage: {
                auto modifier = getFlatDamageModifier(
                    _services,
                    kDecreaseCostTable,
                    property.costValue,
                    !weapon && item.get() == sourceItem
                        ? DamageType::Bludgeoning
                        : getPrimaryDamageType(item->damageFlags()));
                if (!modifier) {
                    break;
                }
                selectDamageModifier(penalties, *modifier);
                break;
            }
            default:
                break;
            }
        }

        EffectSourceKey source {EffectSourceKind::Item, item->runtimeIncarnation()};
        for (const auto &entry : bonuses) {
            itemBonuses.push_back({source, entry.second});
        }
        for (const auto &entry : penalties) {
            itemPenalties.push_back({source, entry.second});
        }
    }

    EffectModifierReducer effectModifierReducer;
    for (const auto &applied : effects()) {
        if (!applied.hasLiveRuntimeSource()) {
            continue;
        }
        if (!applied.appliesVersus(target)) {
            continue;
        }
        switch (applied.type()) {
        case EffectType::DamageIncrease: {
            int bonus = applied.integerParameter(0);
            if (bonus > 0) {
                int type = static_cast<int>(getPrimaryDamageType(
                    applied.integerParameter(1)));
                effectModifierReducer.addIncrease(
                    getEffectSourceKey(applied),
                    type,
                    criticalMultiplier * bonus);
            }
            break;
        }
        case EffectType::DamageDecrease: {
            int penalty = applied.integerParameter(0);
            if (penalty > 0) {
                int type = static_cast<int>(getPrimaryDamageType(
                    applied.integerParameter(1)));
                effectModifierReducer.addDecrease(
                    getEffectSourceKey(applied),
                    type,
                    criticalMultiplier * penalty);
            }
            break;
        }
        default:
            break;
        }
    }

    for (const auto &entry : itemBonuses) {
        effectModifierReducer.addIncrease(
            entry.source, static_cast<int>(entry.modifier.type),
            rollDamageModifier(entry.modifier, criticalMultiplier));
    }
    for (const auto &entry : itemPenalties) {
        effectModifierReducer.addDecrease(
            entry.source, static_cast<int>(entry.modifier.type),
            std::abs(rollDamageModifier(entry.modifier, criticalMultiplier)));
    }
    auto addDamage = [&](int amount, DamageType type) {
        if (amount != 0) {
            damage.add(amount, type);
            breakdown.addRawDamage(amount, type);
        }
    };
    for (const auto &[type, amount] :
         effectModifierReducer.increasesBySubtype(kMaximumDamageEffectModifier)) {
        addDamage(amount, static_cast<DamageType>(type));
    }
    for (const auto &[type, amount] :
         effectModifierReducer.decreasesBySubtype(kMaximumDamageEffectModifier)) {
        addDamage(-amount, static_cast<DamageType>(type));
    }
}

void Creature::getMainHandDamage(int &min, int &max) const {
    auto weapon = getEquippedItem(InventorySlots::rightWeapon);
    getWeaponDamage(weapon.get(), min, max);
}

void Creature::getWeaponDamage(const Item *weapon, int &min, int &max) const {
    if (!weapon) {
        min = 1;
        max = 1;
    } else {
        min = weapon->numDice();
        max = weapon->numDice() * weapon->dieToRoll();
    }

    int modifier;
    if (weapon && weapon->isRanged()) {
        modifier = getEffectiveAbilityModifier(Ability::Dexterity);
    } else {
        modifier = getEffectiveAbilityModifier(Ability::Strength);
    }
    min += modifier;
    max += modifier;
}

void Creature::getOffhandDamage(int &min, int &max) const {
    auto weapon = getOffhandAttackWeapon();
    getWeaponDamage(weapon.get(), min, max);
}

void Creature::onEventSignalled(const std::string &name) {
    if (name == "draw_weapon") {
        setLightsabersPowered(true, true);
        if (!_combatState.active) {
            _lightsaberIdlePowerDownPending = true;
            _lightsaberIdlePowerDownTimer.reset(8.0f);
        }
        return;
    }
    if (_footstepType == -1 || _walkmeshMaterial == -1 || name != "snd_footstep") {
        return;
    }
    std::shared_ptr<FootstepTypeSounds> sounds(_services.game.footstepSounds.get(_footstepType));
    if (!sounds) {
        return;
    }
    const Surface &surface = _services.game.surfaces.getSurface(_walkmeshMaterial);
    std::vector<std::shared_ptr<AudioClip>> materialSounds;
    if (surface.sound == "DT") {
        materialSounds = sounds->dirt;
    } else if (surface.sound == "GR") {
        materialSounds = sounds->grass;
    } else if (surface.sound == "ST") {
        materialSounds = sounds->stone;
    } else if (surface.sound == "WD") {
        materialSounds = sounds->wood;
    } else if (surface.sound == "WT") {
        materialSounds = sounds->water;
    } else if (surface.sound == "CP") {
        materialSounds = sounds->carpet;
    } else if (surface.sound == "MT") {
        materialSounds = sounds->metal;
    } else if (surface.sound == "LV") {
        materialSounds = sounds->leaves;
    }
    int index = randomInt(0, 3);
    if (index >= static_cast<int>(materialSounds.size())) {
        return;
    }
    auto clip = materialSounds[index];
    if (clip) {
        _audioSourceFootstep = _services.audio.mixer.play(
            std::move(clip),
            AudioType::Sound,
            1.0f,
            false,
            _position);
    }
}

void Creature::giveGold(int amount) {
    _gold += amount;
}

void Creature::takeGold(int amount) {
    _gold -= amount;
}

glm::vec3 Creature::computeSteeringForce(const Uniwalk &uni, const glm::vec3 &next, float dt) {
    glm::vec3 desiredForce = glm::normalize(next - _position);
    glm::vec3 keepoutForce = computeKeepoutForce(uni, _position);

    // If we're not making progress - move in a random direction and
    // hope. If we wander off too far, the path will be recalculated.
    if (!_stuckTimer.elapsed() || glm::length2(_position - _previousPosition) < 0.0001) {
        // Try to unstuck for some time even if we're moving
        // again. Otherwise desiredForce kicks in again next frame.
        if (_stuckTimer.elapsed()) {
            _stuckTimer.reset(1.0f);
            _stuckForce =
                glm::normalize(glm::vec3 {
                    randomFloat(-1.0f, 1.0f),
                    randomFloat(-1.0f, 1.0f),
                    randomFloat(-1.0f, 1.0f),
                });
        } else {
            _stuckTimer.update(dt);
        }
        desiredForce = glm::vec3 {0.0f, 0.0f, 0.0f};
    } else {
        _stuckForce = glm::vec3 {0.0f, 0.0f, 0.0f};
    }
    _previousPosition = _position;

    glm::vec3 combinedForce = desiredForce + 0.1f * keepoutForce + _stuckForce;

    drawdebug::pushId("computeSteeringForce");
    drawdebug::pushId(_id);
    drawdebug::clear();

    if (isShowPathEnabled()) {
        drawdebug::line(_position, _position + desiredForce, 0x00BFFFFF, 0.02);
        drawdebug::line(_position, _position + keepoutForce, 0xDDA0DDFF, 0.02);
        drawdebug::line(_position, _position + _stuckForce, 0xF08080FF, 0.02);
        drawdebug::line(_position, _position + combinedForce, 0xADFF2FFF, 0.02);
    }

    drawdebug::popId();
    drawdebug::popId();

    return combinedForce;
}

bool Creature::navigateTo(const glm::vec3 &dest, bool run, float distance, float dt) {
    if (isMovementRestricted()) {
        setMovementType(MovementType::None);
        return false;
    }

    auto module = _game.module();
    if (!module || !module->area()) {
        // Navigation without a module does not make sense. This is only useful
        // for unit tests.
        return true;
    }

    Pathfinder &pf = module->area()->pathfinder();

    // Stop if we reached the destination.
    float distToDest2 = getSquareDistanceTo(glm::vec2(dest));
    if (distToDest2 <= distance * distance) {
        setMovementType(Creature::MovementType::None);
        clearPath();
        return true;
    }

    float eps2 = std::min(0.5f * 0.5f, distance * distance);
    if (_path && getSquareDistanceTo(getLastPathPoint(pf, *_path)) < eps2) {
        // Reached the last point, but not reached the destination. Find another
        // path.
        releasePath(pf, *_path);
        _path = std::nullopt;
    }

    if (_path && !updatePath(pf, *_path, position())) {
        // Lost the path and cannot recalculate.
        releasePath(pf, *_path);
        _path = std::nullopt;
        return false;
    }

    // Advance on path.
    if (_path) {
        glm::vec3 steeringForce = computeSteeringForce(pf.uni, getNextPathPoint(pf, *_path), dt);
        _pathVelocity += steeringForce * dt;

        float maxSpeed = 0.3f;
        float speed = glm::min(glm::length(_pathVelocity), maxSpeed);
        _pathVelocity = glm::normalize(_pathVelocity) * speed;

        glm::vec3 dir = glm::normalize(_pathVelocity);
        advanceOnPath(dest, dir, run, distance, dt);
        return false;
    }

    // Find a path and start following it.
    _path = createPath(pf, position(), dest);
    if (!_path) {
        return false;
    }
    _pathVelocity = {0.0f, 0.0f, 0.0f};

    return navigateTo(dest, run, distance, dt);
}

void Creature::advanceOnPath(const glm::vec3 &dest, const glm::vec3 &dir, bool run, float distance, float dt) {
    setMovementType(run ? Creature::MovementType::Run : Creature::MovementType::Walk);
    _game.module()->area()->moveCreature(
        _game.getObjectById<Creature>(_id), dir, run, dt, getDistanceTo(dest));

    // Report a door that obstructed this step. A door can stop the creature
    // from making progress while the slide in moveCreature still produces
    // some sideways motion, so this is keyed on the recorded obstruction
    // rather than on whether the step moved the creature at all.
    dispatchBlockedEvent();
}

void Creature::clearPath() {
    if (!_path) {
        return;
    }

    Pathfinder &pf = _game.module()->area()->pathfinder();
    releasePath(pf, *_path);
    _path = std::nullopt;
}

void Creature::dispatchBlockedEvent() {
    if (_blockingDoorId == script::kObjectInvalid) {
        // Nothing obstructed this step. Re-arm, so meeting the same door again
        // later reports again.
        _blockedEventDoorId = script::kObjectInvalid;
        return;
    }
    if (_blockedEventDoorId == _blockingDoorId) {
        // Still the same obstruction that was already reported.
        return;
    }
    _blockedEventDoorId = _blockingDoorId;
    runBlockedScript(_blockingDoorId);
}

std::string Creature::getAnimationName(AnimationType anim) const {
    std::string result;
    switch (anim) {
    case AnimationType::LoopingPause:
        return getPauseAnimation();
    case AnimationType::LoopingPause2:
        return getFirstIfCreatureModel("cpause2", "pause2");
    case AnimationType::LoopingListen:
        return "listen";
    case AnimationType::LoopingMeditate:
        return "meditate";
    case AnimationType::LoopingTalkNormal:
        return "tlknorm";
    case AnimationType::LoopingTalkPleading:
        return "tlkplead";
    case AnimationType::LoopingTalkForceful:
        return "tlkforce";
    case AnimationType::LoopingTalkLaughing:
        return getFirstIfCreatureModel("", "tlklaugh");
    case AnimationType::LoopingTalkSad:
        return "tlksad";
    case AnimationType::LoopingPauseTired:
        return "pausetrd";
    case AnimationType::LoopingFlirt:
        return "flirt";
    case AnimationType::LoopingUseComputer:
        return "usecomplp";
    case AnimationType::LoopingDance:
        return "dance";
    case AnimationType::LoopingDance1:
        return "dance1";
    case AnimationType::LoopingHorror:
        return "horror";
    case AnimationType::LoopingDeactivate:
        return getFirstIfCreatureModel("", "deactivate");
    case AnimationType::LoopingSpasm:
        return getFirstIfCreatureModel("cspasm", "spasm");
    case AnimationType::LoopingSleep:
        return "sleep";
    case AnimationType::LoopingProne:
        return "prone";
    case AnimationType::LoopingPause3:
        return getFirstIfCreatureModel("", "pause3");
    case AnimationType::LoopingWeld:
        return "weld";
    case AnimationType::LoopingDead:
        return getDeadAnimation();
    case AnimationType::LoopingTalkInjured:
        return "talkinj";
    case AnimationType::LoopingListenInjured:
        return "listeninj";
    case AnimationType::LoopingTreatInjured:
        return "treatinjlp";
    case AnimationType::LoopingUnlockDoor:
        return "unlockdr";
    case AnimationType::LoopingClosed:
        return "closed";
    case AnimationType::LoopingStealth:
        return "stealth";
    case AnimationType::FireForgetHeadTurnLeft:
        return getFirstIfCreatureModel("chturnl", "hturnl");
    case AnimationType::FireForgetHeadTurnRight:
        return getFirstIfCreatureModel("chturnr", "hturnr");
    case AnimationType::FireForgetSalute:
        return "salute";
    case AnimationType::FireForgetBow:
        return "bow";
    case AnimationType::FireForgetGreeting:
        return "greeting";
    case AnimationType::FireForgetTaunt:
        return getFirstIfCreatureModel("ctaunt", "taunt");
    case AnimationType::FireForgetDiveRoll:
        // Row 567 of K2 animations.2da, the one animation the shipped scripts
        // ever ask PlayOverlayAnimation for. K1 has neither the clip nor the
        // constant.
        return getFirstIfCreatureModel("cdiveroll", "diveroll");
    case AnimationType::FireForgetVictory1:
        return getFirstIfCreatureModel("cvictory", "victory");
    case AnimationType::FireForgetInject:
        return "inject";
    case AnimationType::FireForgetUseComputer:
        return "usecomp";
    case AnimationType::FireForgetPersuade:
        return "persuade";
    case AnimationType::FireForgetActivate:
        return "activate";
    case AnimationType::LoopingChoke:
        return "choke";
    case AnimationType::FireForgetTreatInjured:
        return "treatinj";
    case AnimationType::FireForgetOpen:
        return "open";
    case AnimationType::LoopingReady:
        return getAnimationName(CombatAnimation::Ready, getWieldType(), 0);

    case AnimationType::LoopingWorship:
    case AnimationType::LoopingGetLow:
    case AnimationType::LoopingGetMid:
    case AnimationType::LoopingPauseDrunk:
    case AnimationType::LoopingDeadProne:
    case AnimationType::LoopingKneelTalkAngry:
    case AnimationType::LoopingKneelTalkSad:
    case AnimationType::LoopingCheckBody:
    case AnimationType::LoopingSitAndMeditate:
    case AnimationType::LoopingSitChair:
    case AnimationType::LoopingSitChairDrink:
    case AnimationType::LoopingSitChairPazak:
    case AnimationType::LoopingSitChairComp1:
    case AnimationType::LoopingSitChairComp2:
    case AnimationType::LoopingRage:
    case AnimationType::LoopingChokeWorking:
    case AnimationType::LoopingMeditateStand:
    case AnimationType::FireForgetPauseScratchHead:
    case AnimationType::FireForgetPauseBored:
    case AnimationType::FireForgetVictory2:
    case AnimationType::FireForgetVictory3:
    case AnimationType::FireForgetThrowHigh:
    case AnimationType::FireForgetThrowLow:
    case AnimationType::FireForgetCustom01:
    case AnimationType::FireForgetForceCast:
    case AnimationType::FireForgetScream:
    default:
        debug("CreatureAnimationResolver: unsupported animation type: " + std::to_string(static_cast<int>(anim)));
        return "";
    }
}

std::string Creature::getDieAnimation() const {
    return getFirstIfCreatureModel("cdie", "die");
}

std::string Creature::getFirstIfCreatureModel(std::string creatureAnim, std::string elseAnim) const {
    return _modelType == Creature::ModelType::Creature ? std::move(creatureAnim) : std::move(elseAnim);
}

std::string Creature::getDeadAnimation() const {
    return getFirstIfCreatureModel("cdead", "dead");
}

std::string Creature::getPauseAnimation() const {
    if (_modelType == Creature::ModelType::Creature)
        return "cpause1";

    // TODO: if (_lowHP) return "pauseinj"

    if (_combatState.active) {
        WeaponType type = WeaponType::None;
        WeaponWield wield = WeaponWield::None;
        getWeaponInfo(type, wield);

        int wieldNumber = getWeaponWieldNumber(wield);
        return str(boost::format("g%dr1") % wieldNumber);
    }

    return "pause1";
}

bool Creature::getWeaponInfo(WeaponType &type, WeaponWield &wield) const {
    std::shared_ptr<Item> item(getEquippedItem(InventorySlots::rightWeapon));
    if (item) {
        type = item->weaponType();
        wield = item->weaponWield();
        return true;
    }

    return false;
}

int Creature::getWeaponWieldNumber(WeaponWield wield) const {
    switch (wield) {
    case WeaponWield::StunBaton:
        return 1;
    case WeaponWield::SingleSword:
        return getOffhandAttackWeapon() ? 4 : 2;
    case WeaponWield::DoubleBladedSword:
        return 3;
    case WeaponWield::BlasterPistol:
        return getOffhandAttackWeapon() ? 6 : 5;
    case WeaponWield::BlasterRifle:
        return 7;
    case WeaponWield::HeavyWeapon:
        return 9;
    default:
        return 8;
    }
}

int Creature::getRelativeWeaponSize(const Item &weapon) const {
    if (_size == CreatureSize::Invalid ||
        weapon.weaponSize() == CreatureSize::Invalid) {
        return -10;
    }

    int relativeSize = static_cast<int>(weapon.weaponSize()) -
                       static_cast<int>(_size);
    return relativeSize >= -2 && relativeSize <= 1 ? relativeSize : -10;
}

int Creature::getTwoWeaponAttackPenalty(
    const Item *weapon,
    bool offHand,
    int *smallOffhandBonus) const {

    if (smallOffhandBonus) {
        *smallOffhandBonus = 0;
    }

    auto mainHand = getEquippedItem(InventorySlots::rightWeapon);
    if (!mainHand || mainHand->weaponType() == WeaponType::None) {
        return 0;
    }

    auto offHandWeapon = getOffhandAttackWeapon();
    bool doubleBladed = offHandWeapon == mainHand &&
                        mainHand->weaponWield() == WeaponWield::DoubleBladedSword;
    if (!offHandWeapon || offHandWeapon->weaponType() == WeaponType::None) {
        return 0;
    }

    int superiorRank = getHighestOwnedFeatRank(*this,
        FeatType::SuperiorWeaponFocusTwoWeapon1,
        3);

    if (offHand) {
        if (weapon != offHandWeapon.get()) {
            return 0;
        }

        int penalty = 10;
        if (hasEffectiveFeat(FeatType::AdvancedDoubleWeaponFighting)) {
            penalty = 2;
        } else if (hasEffectiveFeat(FeatType::DoubleWeaponFighting)) {
            penalty = 4;
        } else if (hasEffectiveFeat(FeatType::Ambidexterity)) {
            penalty = 6;
        }
        return penalty - getSuperiorTwoWeaponPenaltyReduction(
                             _game.isTSL(),
                             true,
                             superiorRank);
    }

    if (weapon != mainHand.get()) {
        return 0;
    }

    bool balanced = doubleBladed;
    if (!balanced) {
        int relativeSize = getRelativeWeaponSize(
            mainHand->isRanged() ? *mainHand : *offHandWeapon);
        balanced = mainHand->isRanged() ? relativeSize <= -1 : relativeSize == -1;
    }

    if (balanced && smallOffhandBonus) {
        *smallOffhandBonus = 2;
    }

    int penalty = balanced ? 4 : 6;
    if (hasEffectiveFeat(FeatType::AdvancedDoubleWeaponFighting)) {
        penalty -= 4;
    } else if (hasEffectiveFeat(FeatType::DoubleWeaponFighting)) {
        penalty -= 2;
    }
    return penalty - getSuperiorTwoWeaponPenaltyReduction(
                         _game.isTSL(),
                         false,
                         superiorRank);
}

int Creature::getDuelingBonus() const {
    auto mainHand = getEquippedItem(InventorySlots::rightWeapon);
    bool leftHandEquipped = static_cast<bool>(
        getEquippedItem(InventorySlots::leftWeapon));
    if (!qualifiesForDueling(
            _game.isTSL(),
            static_cast<bool>(mainHand),
            mainHand ? mainHand->weaponWield() : WeaponWield::None,
            leftHandEquipped)) {
        return 0;
    }

    if (hasEffectiveFeat(FeatType::MasterDueling)) {
        return 3;
    }
    if (hasEffectiveFeat(FeatType::ImprovedDueling)) {
        return 2;
    }
    return hasEffectiveFeat(FeatType::Dueling) ? 1 : 0;
}

std::string Creature::getWalkAnimation() const {
    return getFirstIfCreatureModel("cwalk", "walk");
}

std::string Creature::getRunAnimation() const {
    if (_modelType == Creature::ModelType::Creature)
        return "crun";

    // TODO: if (_lowHP) return "runinj"

    if (_combatState.active) {
        WeaponType type = WeaponType::None;
        WeaponWield wield = WeaponWield::None;
        getWeaponInfo(type, wield);

        switch (wield) {
        case WeaponWield::SingleSword:
            return isSlotEquipped(InventorySlots::leftWeapon) ? "runds" : "runss";
        case WeaponWield::DoubleBladedSword:
            return "runst";
        case WeaponWield::BlasterRifle:
        case WeaponWield::HeavyWeapon:
            return "runrf";
        default:
            break;
        }
    }

    return "run";
}

std::string Creature::getTalkNormalAnimation() const {
    return "tlknorm";
}

std::string Creature::getHeadTalkAnimation() const {
    return "talk";
}

static std::string formatCombatAnimation(const std::string &format, CreatureWieldType wield, int variant) {
    return str(boost::format(format) % static_cast<int>(wield) % variant);
}

std::string Creature::getAnimationName(CombatAnimation anim, CreatureWieldType wield, int variant) const {
    switch (anim) {
    case CombatAnimation::Draw:
        return getFirstIfCreatureModel("", formatCombatAnimation("g%dw%d", wield, 1));
    case CombatAnimation::Ready:
        return getFirstIfCreatureModel("creadyr", formatCombatAnimation("g%dr%d", wield, 1));
    case CombatAnimation::Attack:
        return getFirstIfCreatureModel("g0a1", formatCombatAnimation("g%da%d", wield, variant));
    case CombatAnimation::Damage:
        return getFirstIfCreatureModel("cdamages", formatCombatAnimation("g%dd%d", wield, variant));
    case CombatAnimation::Dodge:
        return getFirstIfCreatureModel("cdodgeg", formatCombatAnimation("g%dg%d", wield, variant));
    case CombatAnimation::MeleeAttack:
        return getFirstIfCreatureModel("m0a1", formatCombatAnimation("m%da%d", wield, variant));
    case CombatAnimation::MeleeDamage:
        return getFirstIfCreatureModel("cdamages", formatCombatAnimation("m%dd%d", wield, variant));
    case CombatAnimation::MeleeDodge:
        return getFirstIfCreatureModel("cdodgeg", formatCombatAnimation("m%dg%d", wield, variant));
    case CombatAnimation::CinematicMeleeAttack:
        return formatCombatAnimation("c%da%d", wield, variant);
    case CombatAnimation::CinematicMeleeDamage:
        return formatCombatAnimation("c%dd%d", wield, variant);
    case CombatAnimation::CinematicMeleeParry:
        return formatCombatAnimation("c%dp%d", wield, variant);
    case CombatAnimation::BlasterAttack:
        return getFirstIfCreatureModel("b0a1", formatCombatAnimation("b%da%d", wield, variant));
    default:
        return "";
    }
}

std::string Creature::getActiveAnimationName() const {
    auto model = std::dynamic_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model)
        return "";

    return model->activeAnimationName();
}

std::shared_ptr<ModelSceneNode> Creature::buildModel() {
    std::string modelName(getBodyModelName());
    if (modelName.empty()) {
        return nullptr;
    }
    std::shared_ptr<Model> model(_services.resource.models.get(modelName));
    if (!model) {
        return nullptr;
    }
    auto &sceneGraph = _services.scene.graphs.get(_sceneName);
    auto sceneNode = sceneGraph.newModel(*model, ModelUsage::Creature);
    sceneNode->setDrawDistance(_game.options().graphics.drawDistance);
    sceneNode->setAnimationEventListener(*this);

    return sceneNode;
}

void Creature::finalizeModel(ModelSceneNode &body) {
    auto &sceneGraph = _services.scene.graphs.get(_sceneName);

    // Body texture

    if (!_envmap.empty()) {
        if (_envmap == "default") {
            body.setEnvironmentMap(&_services.graphics.textureRegistry.get(TextureName::defaultCubemapRgb));
        } else {
            body.setEnvironmentMap(_services.resource.textures.get(_envmap, TextureUsage::EnvironmentMap).get());
        }
    }
    std::string bodyTextureName(getBodyTextureName());
    if (!bodyTextureName.empty()) {
        std::shared_ptr<Texture> texture(_services.resource.textures.get(bodyTextureName, TextureUsage::MainTex));
        if (texture) {
            body.setMainTexture(texture.get());
        }
    }

    // Mask

    std::shared_ptr<Model> maskModel;
    std::string maskModelName(getMaskModelName());
    if (!maskModelName.empty()) {
        maskModel = _services.resource.models.get(maskModelName);
    }

    // Head

    std::string headModelName(getHeadModelName());
    if (!headModelName.empty()) {
        std::shared_ptr<Model> headModel(_services.resource.models.get(headModelName));
        if (headModel) {
            std::shared_ptr<ModelSceneNode> headSceneNode(sceneGraph.newModel(*headModel, ModelUsage::Creature));
            body.attach(g_headHookNode, *headSceneNode);
            if (maskModel) {
                auto maskSceneNode = sceneGraph.newModel(*maskModel, ModelUsage::Equipment);
                headSceneNode->attach(g_maskHookNode, *maskSceneNode);
            }
        }
    }

    // Right weapon

    std::string rightWeaponModelName(getWeaponModelName(InventorySlots::rightWeapon));
    if (!rightWeaponModelName.empty()) {
        std::shared_ptr<Model> weaponModel(_services.resource.models.get(rightWeaponModelName));
        if (weaponModel) {
            std::shared_ptr<ModelSceneNode> weaponSceneNode(sceneGraph.newModel(*weaponModel, ModelUsage::Equipment));
            body.attach(g_rightHandNode, *weaponSceneNode);
            if (weaponModel->classification() == MdlClassification::lightsaber) {
                weaponSceneNode->playAnimation(_combatState.active ? "powered" : "off");
            }
        }
    }

    // Left weapon

    std::string leftWeaponModelName(getWeaponModelName(InventorySlots::leftWeapon));
    if (!leftWeaponModelName.empty()) {
        std::shared_ptr<Model> weaponModel(_services.resource.models.get(leftWeaponModelName));
        if (weaponModel) {
            std::shared_ptr<ModelSceneNode> weaponSceneNode(sceneGraph.newModel(*weaponModel, ModelUsage::Equipment));
            body.attach(g_leftHandNode, *weaponSceneNode);
            if (weaponModel->classification() == MdlClassification::lightsaber) {
                weaponSceneNode->playAnimation(_combatState.active ? "powered" : "off");
            }
        }
    }
}

CreaturePresentation Creature::presentation() const {
    if (_presentation) return *_presentation;
    CreaturePresentation result;
    result.gender = static_cast<int>(_gender);
    result.appearance = _appearance;
    auto body = getEquippedItem(InventorySlots::body);
    if (body && !body->baseBodyVariation().empty()) {
        auto variation = boost::to_lower_copy(body->baseBodyVariation());
        result.bodyVariation = variation.front() - 'a';
        result.textureVariation = body->textureVariation();
    }
    return result;
}

void Creature::setPresentation(const CreaturePresentation &presentation) {
    if (!isPresentationOnly()) {
        throw std::logic_error("Visual tuples require a presentation-only creature");
    }
    if (presentation.appearance < 0 || presentation.appearance > 65535 ||
        presentation.gender < 0 || presentation.gender > static_cast<int>(Gender::None) ||
        presentation.bodyVariation < 0 || presentation.bodyVariation >= 26 ||
        presentation.textureVariation < 0 || presentation.textureVariation > 255) {
        throw std::invalid_argument("Invalid creature presentation");
    }
    _presentation = presentation;
    _appearance = presentation.appearance;
    _gender = static_cast<Gender>(presentation.gender);
}

std::string Creature::getBodyModelName() const {
    std::string column;

    if (_modelType == Creature::ModelType::Character) {
        column = "model";

        std::shared_ptr<Item> bodyItem(getEquippedItem(InventorySlots::body));
        if (_presentation) {
            column += static_cast<char>('a' + _presentation->bodyVariation);
        } else if (bodyItem) {
            std::string baseBodyVar(bodyItem->baseBodyVariation());
            column += baseBodyVar;
        } else {
            column += "a";
        }

    } else {
        column = "race";
    }

    std::shared_ptr<TwoDA> appearance(_services.resource.twoDas.get("appearance"));
    if (!appearance) {
        throw ResourceNotFoundException("appearance 2DA not found");
    }

    std::string modelName(appearance->getString(_appearance, column));
    boost::to_lower(modelName);

    return modelName;
}

std::string Creature::getBodyTextureName() const {
    std::string column;
    std::shared_ptr<Item> bodyItem(getEquippedItem(InventorySlots::body));

    if (_modelType == Creature::ModelType::Character) {
        column = "tex";

        if (_presentation) {
            column += static_cast<char>('a' + _presentation->bodyVariation);
        } else if (bodyItem) {
            std::string baseBodyVar(bodyItem->baseBodyVariation());
            column += baseBodyVar;
        } else {
            column += "a";
        }
    } else {
        column = "racetex";
    }

    std::shared_ptr<TwoDA> appearance(_services.resource.twoDas.get("appearance"));
    if (!appearance) {
        throw ResourceNotFoundException("appearance 2DA not found");
    }

    std::string texName(boost::to_lower_copy(appearance->getString(_appearance, column)));
    if (texName.empty())
        return "";

    if (_modelType == Creature::ModelType::Character) {
        bool texFound = false;
        if (_presentation || bodyItem) {
            auto variation = _presentation ? _presentation->textureVariation : bodyItem->textureVariation();
            std::string tmp(str(boost::format("%s%02d") % texName % variation));
            std::shared_ptr<Texture> texture(_services.resource.textures.get(tmp, TextureUsage::MainTex));
            if (texture) {
                texName = std::move(tmp);
                texFound = true;
            }
        }
        if (!texFound) {
            texName += "01";
        }
    }

    return texName;
}

std::string Creature::getHeadModelName() const {
    if (_modelType != Creature::ModelType::Character) {
        return "";
    }
    std::shared_ptr<TwoDA> appearance(_services.resource.twoDas.get("appearance"));
    if (!appearance) {
        throw ResourceNotFoundException("appearance 2DA not found");
    }
    int headIdx = appearance->getInt(_appearance, "normalhead", -1);
    if (headIdx == -1) {
        return "";
    }
    std::shared_ptr<TwoDA> heads(_services.resource.twoDas.get("heads"));
    if (!heads) {
        throw ResourceNotFoundException("heads 2DA not found");
    }

    std::string modelName(heads->getString(headIdx, "head"));
    boost::to_lower(modelName);

    return modelName;
}

std::string Creature::getMaskModelName() const {
    std::shared_ptr<Item> headItem(getEquippedItem(InventorySlots::head));
    if (!headItem)
        return "";

    std::string modelName(boost::to_lower_copy(headItem->itemClass()));
    modelName += str(boost::format("_%03d") % headItem->modelVariation());

    return modelName;
}

std::string Creature::getWeaponModelName(int slot) const {
    std::shared_ptr<Item> bodyItem(getEquippedItem(slot));
    if (!bodyItem)
        return "";

    std::string modelName(bodyItem->itemClass());
    boost::to_lower(modelName);

    modelName += str(boost::format("_%03d") % bodyItem->modelVariation());

    return modelName;
}

void Creature::deserialize(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    std::string templateRes;
    if (!identityContext.isSerializedState() &&
        gff.readResRef(templateRes, "TemplateResRef")) {
        if (auto utc = _services.resource.gffs.get(templateRes, ResType::Utc)) {
            deserializeAll(
                *utc,
                SerializedIdentityContext::templateResource(templateRes));
        }
    }
    deserializeAll(gff, identityContext);

    restoreSerializedVitality();

    updateTransform();
    loadAppearance();
}

void Creature::deserializeAll(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    if (_game.isTSL()) {
        if (gff.has("CurrentForm")) _currentForm = static_cast<CombatForm>(gff.getUint("CurrentForm"));
        if (gff.has("MultiplierSet")) _autoBalanceContext.multiplierSet = static_cast<uint8_t>(gff.getUint("MultiplierSet"));
        if (gff.has("PCLevelAtSpawn")) {
            _autoBalanceContext.playerLevelAtSpawn = static_cast<uint8_t>(gff.getUint("PCLevelAtSpawn"));
            _autoBalancePlayerLevelAtSpawnSet = true;
        }
    }

    Object::deserialize(gff, identityContext);

    // The game reads IsPC before post-processing hit points. A player character
    // remains an incapacitated, resumable runtime object through 0..-9 HP and
    // becomes truly dead only at -10 HP.
    gff.readBool(_isPC, "IsPC");
    _initiative = !isPC();
    setStealthMode(gff.getBool("StealthMode", _stealthMode));
    gff.readInt(_assignedPuppet, "AssignedPup");

    // index into racialtypes.2da
    gff.readEnum(_race, "Race");

    // index into subrace.2da
    gff.readEnum(_subrace, "SubraceIndex");

    // index into appearance.2da
    gff.readEnum(_appearance, "Appearance_Type");

    // Savegames keep the visible disguise appearance in Appearance_Type and
    // the normal appearance separately. Restore this before equipped items
    // are loaded, so equipping a saved disguise does not overwrite it.
    _disguised = false;
    _appearanceBeforeDisguise = 0;
    bool disguised;
    uint16_t appearanceBeforeDisguise;
    if (gff.readBool(disguised, "PM_IsDisguised") &&
        disguised &&
        gff.readWord(appearanceBeforeDisguise, "PM_Appearance")) {
        _disguised = true;
        _appearanceBeforeDisguise = appearanceBeforeDisguise;
    }

    // in dex into gender.2da
    gff.readEnum(_gender, "Gender");

    // index into portrait.2da
    gff.readWord(_portraitId, "PortraitId");

    // index into repute.2da
    gff.readEnum(_faction, "FactionID");

    gff.readBool(_disarmable, "Disarmable");
    gff.readBool(_noPermDeath, "NoPermDeath");
    if (identityContext.isSerializedState()) {
        // LoadCreature uses one for all three absent BYTE fields in both
        // titles; these are distinct from the constructor's initial flags.
        _destroyable = _raiseable = _selectableWhenDead = true;
        gff.readBool(_destroyable, "IsDestroyable");
        gff.readBool(_raiseable, "IsRaiseable");
        gff.readBool(_selectableWhenDead, "DeadSelectable");
    }
    gff.readBool(_notReorienting, "NotReorienting");
    gff.readByte(_bodyVariation, "BodyVariation");
    gff.readByte(_textureVar, "TextureVar");
    gff.readBool(_partyInteract, "PartyInteract");

    // index into creaturespeed.2da
    gff.readInt(_walkRate, "WalkRate");
    uint8_t movementRate;
    if (gff.readByte(movementRate, "MovementRate")) {
        _walkRate = movementRate;
    }
    gff.readBool(_isListening, "Listening");

    gff.readByte(_naturalAC, "NaturalAC");
    gff.readShort(_forcePoints, "ForcePoints");
    gff.readShort(_currentForce, "CurrentForce");
    _temporaryHitPointsRestored = gff.readInt(_temporaryHitPoints, "ReoneTempHP");
    _temporaryForcePointsRestored = gff.readInt(_temporaryForcePoints, "ReoneTempFP");
    gff.readInt(_bonusForcePoints, "BonusForcePoints");
    _temporaryForcePoints = narrowSignedResource(_temporaryForcePoints);
    gff.readShort(_refBonus, "refbonus");
    gff.readShort(_willBonus, "willbonus");
    gff.readShort(_fortBonus, "fortbonus");
    gff.readByte(_goodEvil, "GoodEvil");
    gff.readFloat(_challengeRating, "ChallengeRating");
    gff.readDword(_xp, "Experience");

    gff.readResRef(_onNotice, "ScriptOnNotice");
    gff.readResRef(_onSpellAt, "ScriptSpellAt");
    gff.readResRef(_onAttacked, "ScriptAttacked");
    gff.readResRef(_onDamaged, "ScriptDamaged");
    gff.readResRef(_onDisturbed, "ScriptDisturbed");
    gff.readResRef(_onEndRound, "ScriptEndRound");
    gff.readResRef(_onEndDialogue, "ScriptEndDialogu");
    gff.readResRef(_onDialogue, "ScriptDialogue");
    gff.readResRef(_onSpawn, "ScriptSpawn");
    gff.readResRef(_onDeath, "ScriptDeath");
    gff.readResRef(_onBlocked, "ScriptOnBlocked");

    // Only a saved creature record carries CreatnScrptFird; a blueprint leaves
    // the flag clear so a freshly materialized creature still spawns once.
    uint8_t spawnScriptFired = 0;
    if (gff.readByte(spawnScriptFired, "CreatnScrptFird")) {
        _spawnScriptFired = spawnScriptFired != 0;
    }

    deserializeName(gff);
    deserializeSoundSet(gff);
    deserializeBodyBag(gff);
    deserializeAttributes(gff);
    deserializePerception(gff);
    deserializeOwnedItemsAndEquipment(gff, identityContext);
}

void Creature::deserializeName(const resource::Gff &gff) {
    gff.readLocString(_firstName, "FirstName", _services.resource.strings);
    gff.readLocString(_lastName, "LastName", _services.resource.strings);

    _name = _firstName.str();
    const std::string &last = _lastName.str();
    if (!_name.empty() && !last.empty()) {
        _name += ' ';
    }
    _name += last;
}

void Creature::deserializeSoundSet(const resource::Gff &gff) {
    gff.readWord(_soundSetId, "SoundSetFile");
    if (_soundSetId == 0xffff) {
        return;
    }

    std::shared_ptr<TwoDA> soundSetTable(_services.resource.twoDas.get("soundset"));
    if (!soundSetTable) {
        return;
    }
    std::string soundSetResRef(soundSetTable->getString(_soundSetId, "resref"));
    if (!soundSetResRef.empty()) {
        _soundSet = _services.resource.soundSets.get(soundSetResRef);
    }
}

void Creature::deserializeBodyBag(const resource::Gff &gff) {
    gff.readByte(_bodyBagId, "BodyBag");
    if (_bodyBagId == 0xFF) {
        return;
    }

    std::shared_ptr<TwoDA> bodyBags(_services.resource.twoDas.get("bodybag"));
    if (!bodyBags) {
        return;
    }
    _bodyBag.name = _services.resource.strings.getText(bodyBags->getInt(_bodyBagId, "name"));
    _bodyBag.appearance = bodyBags->getInt(_bodyBagId, "appearance");
    _bodyBag.corpse = bodyBags->getBool(_bodyBagId, "corpse");
    return;
}

void Creature::deserializeAttributes(const resource::Gff &gff) {
    _levelForcePoints.clear();
    for (const auto &level : gff.getList("LvlStatList")) {
        uint8_t force = 0;
        level->readByte(force, "LvlStatForce");
        _levelForcePoints.push_back(force);
    }
    CreatureAttributes &attributes = _attributes;
    {
        uint8_t value;
        if (gff.readByte(value, "Str")) {
            attributes.setAbilityScore(Ability::Strength, value);
        }
        if (gff.readByte(value, "Dex")) {
            attributes.setAbilityScore(Ability::Dexterity, value);
        }
        if (gff.readByte(value, "Con")) {
            attributes.setAbilityScore(Ability::Constitution, value);
        }
        if (gff.readByte(value, "Int")) {
            attributes.setAbilityScore(Ability::Intelligence, value);
        }
        if (gff.readByte(value, "Wis")) {
            attributes.setAbilityScore(Ability::Wisdom, value);
        }
        if (gff.readByte(value, "Cha")) {
            attributes.setAbilityScore(Ability::Charisma, value);
        }
    }

    for (const auto &clazz : gff.getList("ClassList")) {
        deserializeClass(*clazz);
    }
    _spellLikeAbilities.clear();
    for (const auto &record : gff.getList("SpecAbilityList")) {
        SpellLikeAbility ability;
        record->readWord(ability.spell, "Spell");
        record->readByte(ability.flags, "SpellFlags");
        record->readByte(ability.casterLevel, "SpellCasterLevel");
        _spellLikeAbilities.push_back(ability);
    }

    int skillType = 0;
    for (const auto &skill : gff.getList("SkillList")) {
        attributes.setSkillRank(
            static_cast<SkillType>(skillType++), skill->getUint("Rank"));
    }

    for (const auto &feat : gff.getList("FeatList")) {
        auto featType = static_cast<FeatType>(feat->getUint("Feat"));
        _attributes.addFeat(featType);
    }
}

void Creature::deserializeClass(const resource::Gff &gff) {
    auto clazz = _services.game.classes.get(
        static_cast<ClassType>(gff.getInt("Class")));
    if (!clazz) {
        return;
    }

    int16_t level;
    if (gff.readShort(level, "ClassLevel")) {
        _attributes.addClassLevels(clazz.get(), level);
    }

    for (const auto &spell : gff.getList("KnownList0")) {
        auto spellType = static_cast<SpellType>(spell->getUint("Spell"));
        _attributes.addSpell(spellType);
    }
}

void Creature::deserializePerception(const resource::Gff &gff) {
    gff.readByte(_perceptionId, "PerceptionRange");
    if (_perceptionId == 0xFF) {
        return;
    }

    std::shared_ptr<TwoDA> ranges(_services.resource.twoDas.get("ranges"));
    if (!ranges) {
        return;
    }

    _perception.sightRange = ranges->getFloat(_perceptionId, "primaryrange");
    _perception.hearingRange = ranges->getFloat(_perceptionId, "secondaryrange");
}

std::vector<std::shared_ptr<Object>> Creature::ownedRuntimeObjects() const {
    auto result = Object::ownedRuntimeObjects();
    std::set<const Object *> seen;
    for (const auto &object : result) {
        if (object) {
            seen.insert(object.get());
        }
    }
    for (const auto &[_, item] : _equipment) {
        if (item && seen.insert(item.get()).second) {
            result.push_back(item);
        }
    }
    return result;
}

void Creature::deserializeOwnedItemsAndEquipment(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    const bool replaceItems = gff.has("ItemList");
    const bool replaceEquipment = gff.has("Equip_ItemList");
    if (!replaceItems && !replaceEquipment) return;

    if (!identityContext.isSerializedState()) {
        std::vector<std::shared_ptr<Object>> obsolete;
        if (replaceItems) {
            obsolete.insert(obsolete.end(), _items.begin(), _items.end());
        }
        if (replaceEquipment) {
            for (const auto &[_, item] : _equipment) {
                obsolete.push_back(item);
            }
        }
        std::vector<std::shared_ptr<Item>> replacementItems =
            replaceItems ? std::vector<std::shared_ptr<Item>> {} : _items;
        std::map<int, std::shared_ptr<Item>> replacementEquipment =
            replaceEquipment
                ? std::map<int, std::shared_ptr<Item>> {}
                : _equipment;
        ItemAttributes replacementAttributes;
        std::deque<EffectInstance> replacementEffects;
        _game.replaceRuntimeObjectGraph(
            obsolete,
            [&]() {
                if (replaceItems) {
                    for (const auto &itemGff : gff.getList("ItemList")) {
                        auto item = _game.newOwnedItem(*itemGff, identityContext);
                        item->setOwner(_id);
                        appendOwnedItemCandidate(
                            replacementItems, item, true);
                    }
                }
                if (replaceEquipment) {
                    for (const auto &itemGff : gff.getList("Equip_ItemList")) {
                        auto item = _game.newOwnedItem(*itemGff, identityContext);
                        int slot = item->isEquippable(InventorySlots::body)
                                       ? InventorySlots::body
                                       : InventorySlots::rightWeapon;
                        if (!item->isEquippable(getEquipabilitySlot(slot)) ||
                            replacementEquipment.count(slot) != 0) {
                            item->setOwner(_id);
                            appendOwnedItemCandidate(
                                replacementItems, item, true);
                            continue;
                        }
                        item->setOwner(_id);
                        item->setEquipped(true);
                        replacementEquipment.emplace(slot, std::move(item));
                    }
                }
                for (const auto &item : replacementItems) {
                    replacementAttributes.addItem(item, _services.game);
                }
                if (replaceEquipment) {
                    replacementEffects =
                        rebuildEquippedItemEffects(replacementEquipment);
                }
            },
            [&]() noexcept {
                _items = std::move(replacementItems);
                _equipment = std::move(replacementEquipment);
                _itemAttributes = std::move(replacementAttributes);
                if (replaceEquipment) {
                    replaceEffectState(std::move(replacementEffects));
                }
            });
        return;
    }

    std::vector<std::shared_ptr<Object>> obsolete;
    if (replaceItems) {
        obsolete.insert(obsolete.end(), _items.begin(), _items.end());
    }
    if (replaceEquipment) {
        for (const auto &[_, item] : _equipment) {
            obsolete.push_back(item);
        }
    }
    std::vector<std::shared_ptr<Item>> replacementItems =
        replaceItems ? std::vector<std::shared_ptr<Item>> {} : _items;
    std::map<int, std::shared_ptr<Item>> replacementEquipment =
        replaceEquipment
            ? std::map<int, std::shared_ptr<Item>> {}
            : _equipment;
    ItemAttributes replacementAttributes;
    std::deque<EffectInstance> replacementEffects;
    _game.replaceRuntimeObjectGraph(
        obsolete,
        [&]() {
            if (replaceItems) {
                for (const auto &itemGff : gff.getList("ItemList")) {
                    auto item = _game.newOwnedItem(*itemGff, identityContext);
                    item->captureSaveRecord(
                        *itemGff,
                        identityContext,
                        {SaveRecordOriginKind::ContainedItem, std::to_string(_id)});
                    item->setOwner(_id);
                    appendOwnedItemCandidate(
                        replacementItems, item, true);
                }
            }

            if (replaceEquipment) {
                for (const auto &itemGff : gff.getList("Equip_ItemList")) {
                    auto item = _game.newOwnedItem(*itemGff, identityContext);
                    item->captureSaveRecord(
                        *itemGff,
                        identityContext,
                        {SaveRecordOriginKind::EquippedItem, std::to_string(_id)});

                    std::optional<int> slot;
                    uint32_t slotMask = itemGff->type();
                    if (slotMask != 0 && (slotMask & (slotMask - 1)) == 0) {
                        int value = 0;
                        while ((slotMask >>= 1) != 0) {
                            ++value;
                        }
                        if (item->isEquippable(getEquipabilitySlot(value))) {
                            slot = value;
                        }
                    }
                    if (!slot && item->isEquippable(InventorySlots::body)) {
                        slot = InventorySlots::body;
                    } else if (!slot &&
                               item->isEquippable(InventorySlots::rightWeapon)) {
                        slot = InventorySlots::rightWeapon;
                    }

                    if (!slot) {
                        item->setOwner(_id);
                        appendOwnedItemCandidate(
                            replacementItems, item, true);
                        warn(str(boost::format("item is not equippable: %s") %
                                 replacementItems.back()->tag()));
                        continue;
                    }
                    if (replacementEquipment.count(*slot) != 0) {
                        throw ValidationException(
                            "Multiple saved items occupy equipment slot " +
                            std::to_string(*slot));
                    }
                    item->setOwner(_id);
                    item->setEquipped(true);
                    replacementEquipment.emplace(*slot, std::move(item));
                }
            }
            for (const auto &item : replacementItems) {
                replacementAttributes.addItem(item, _services.game);
            }
            if (replaceEquipment) {
                replacementEffects =
                    rebuildEquippedItemEffects(replacementEquipment);
            }
        },
        [&]() noexcept {
            if (replaceEquipment) {
                for (auto &[_, item] : _equipment) {
                    if (item) {
                        item->setEquipped(false);
                        item->setOwner(0);
                    }
                }
            }
            if (replaceItems) {
                for (auto &item : _items) {
                    if (item) {
                        item->setOwner(0);
                    }
                }
            }
            _items = std::move(replacementItems);
            _equipment = std::move(replacementEquipment);
            _itemAttributes = std::move(replacementAttributes);
            if (replaceEquipment) {
                replaceEffectState(std::move(replacementEffects));
            }
        });
    uint32_t previousAppearance = _appearance;
    updateDisguise();
    if (_appearance != previousAppearance) {
        loadAppearanceProperties();
    }
}

bool Creature::isPartyMember() const {
    return _game.party().isMember(*this);
}

void Creature::applyDamageEffect(
    int amount,
    const std::shared_ptr<Object> &damager) {

    if (_dead) {
        return;
    }

    if (amount == 0) {
        runDamagedScript();
        return;
    }

    applyHitPointDamage(amount, damager);
}

int Creature::selectMeleeAttackVariant(bool cinematic) {
    int variant;
    if (cinematic) {
        do {
            variant = randomInt(0, 4);
        } while (variant == _lastMeleeAttackVariant);
    } else {
        variant = randomInt(0, 1);
    }

    _lastMeleeAttackVariant = variant;
    return variant + 1;
}

int Creature::getDefense(const Creature *attacker, int damageFlags) const {
    return getDefenseBreakdown(attacker, damageFlags).total;
}

DamagePower Creature::calculateDamagePower(
    const Creature *target,
    const Item *weapon,
    bool offHand) const {

    int effectBonus = getAttackBonusBreakdown(
        target,
        weapon,
        offHand).effectBonus;
    return static_cast<DamagePower>(
        static_cast<uint8_t>(effectBonus));
}

void Creature::getDamageResistanceFeatBonuses(
    int damage,
    DamageResolution &resolution) const {

    int percentageRank = 0;
    if (_game.isTSL()) {
        percentageRank = std::max(
            getHighestOwnedFeatRank(*this, FeatType::IgnorePain1, 3),
            getHighestOwnedFeatRank(*this, FeatType::InnerStrength1, 3));
    }
    auto reduction = getResistanceFeatReduction(_game.isTSL(), damage, percentageRank,
        hasEffectiveFeat(FeatType::ImprovedToughness),
        hasEffectiveFeat(FeatType::WookieEndurance),
        _game.isTSL() && hasEffectiveFeat(static_cast<FeatType>(224)),
        _game.isTSL() && hasEffectiveFeat(static_cast<FeatType>(225)));
    resolution.percentageResistanceBonus = reduction.percentage;
    resolution.improvedToughnessBonus = reduction.improvedToughness;
    resolution.wookieeEnduranceBonus = reduction.endurance;
}

float Creature::movementRate(bool applyMobility) const {
    return getMovementRateFactor(_movementRate, _game.isTSL(), applyMobility,
        _game.isTSL() && applyMobility && hasEffectiveFeat(FeatType::Mobility));
}

bool Creature::stateControlsActions() const {
    return game::stateControlsActions(static_cast<CreatureState>(_effectState));
}

int Creature::maxForcePoints() const {
    const bool tsl = _game.isTSL();
    if (racialType() == RacialType::Droid) return 0;
    bool jedi = false;
    bool mastery = false;
    ClassType lastClass = ClassType::Invalid;
    for (const auto &[clazz, classLevel] : _attributes.classLevels()) {
        lastClass = clazz->type();
        jedi |= isForceUsingClass(lastClass, tsl);
        mastery |= lastClass == ClassType::JediConsular ||
            (tsl && (lastClass == ClassType::JediMaster || lastClass == ClassType::SithLord));
    }
    const int level = static_cast<uint8_t>(_attributes.getAggregateLevel());
    int64_t bonuses = hasEffectiveFeat(static_cast<FeatType>(116)) ? 40 : 0;
    if (tsl) bonuses += _bonusForcePoints;
    if (mastery && hasEffect(EffectType::PureEvilPowers)) bonuses += 50;
    const int wisdom = getEffectiveAbilityModifier(Ability::Wisdom);
    const int charisma = getEffectiveAbilityModifier(Ability::Charisma);
    if (_isPC && _game.party().controlledNpc() == -1) {
        // The PC-history branch tests the last class, not aggregate Jedi status.
        if (!isForceUsingClass(lastClass, tsl)) return 0;
        const auto signedByte = [](int value) {
            const int bits = static_cast<uint8_t>(value);
            return bits < 0x80 ? bits : bits - 0x100;
        };
        const int modifier = signedByte(wisdom) + (tsl ? 0 : signedByte(charisma));
        int64_t total = bonuses;
        for (int i = 0; i < level; ++i) {
            // Invalid/truncated histories cannot supply an invented level grant.
            const int gain = static_cast<size_t>(i) < _levelForcePoints.size() ? _levelForcePoints[i] : 0;
            if (gain != 0) total += std::max(1, gain + modifier);
        }
        // K2 clamps a negative sum before returning the signed word;
        // K1 returns the low signed word without that additional gate.
        if (tsl && total < 0) return 0;
        return narrowSignedResource(total);
    }
    if (!jedi) return 0;
    return forcePointMaximum(_forcePoints, level, wisdom, charisma, tsl, bonuses);
}

void Creature::setBonusForcePoints(int amount) {
    if (_game.isTSL()) _bonusForcePoints = amount;
}

int Creature::adjustedSpellForcePointCost(const Spell &spell) const {
    float multiplier = 1.0f;
    if (_isPC && (spell.alignment == 'G' || spell.alignment == 'E')) {
        const auto adjustments = _services.resource.twoDas.get("forceadjust");
        if (adjustments)
            multiplier = adjustments->getFloat(std::clamp<int>(_goodEvil / 10, 0, 10),
                spell.alignment == 'G' ? "goodcost" : "evilcost", 1.0f);
    }
    const auto module = _game.module();
    const int room = _game.isTSL() && module && module->area()
        ? module->area()->getRoomForceRating(position()) : 0;
    return adjustedForcePointCost(spell.forcePointCost, multiplier, _game.isTSL(),
        _isPC ? getEffectiveAbilityModifier(Ability::Charisma) : 0,
        _goodEvil, room, _currentForm);
}

int Creature::spellCasterLevel(const Spell &spell, bool itemOrCheat) const {
    if (!itemOrCheat) {
        for (const auto &[clazz, level] : _attributes.classLevels()) {
            if (!isForceUsingClass(clazz->type(), _game.isTSL())) continue;
            const auto required = spell.getClassLevelRequirement(clazz->type());
            if (level > 0 && required && *required >= 0 && *required != 0xff) return level;
        }
        return -1;
    }
    return std::max(10, 2 * static_cast<int>(spell.innateLevel) - 1);
}

bool Creature::readySpellLikeAbility(SpellType spell, int &casterLevel) const {
    for (const auto &ability : _spellLikeAbilities) {
        if (ability.spell == static_cast<uint16_t>(spell) && ability.flags == 1) {
            casterLevel = ability.casterLevel;
            return true;
        }
    }
    return false;
}

uint32_t Creature::forceItemMask() const {
    uint32_t mask = 0;
    for (const auto &[slot, item] : _equipment) {
        if (item) mask |= item->forceItemMask();
    }
    return mask;
}

bool Creature::consumeSpellLikeAbility(SpellType spell, int &casterLevel) {
    for (auto &ability : _spellLikeAbilities) {
        if (ability.spell != static_cast<uint16_t>(spell) || ability.flags != 1) continue;
        ability.flags = 0;
        casterLevel = ability.casterLevel;
        return true;
    }
    return false;
}

int Creature::forceBodyLevel() const {
    if (_game.isTSL()) {
        for (const auto &effect : effects())
            if (effect.serializedType == 110) return effect.integerParameter(0, -1);
    }
    return -1;
}

bool Creature::canPaySpellForcePointCost(const Spell &spell) const {
    ForcePointPools pools {_currentForce, _temporaryForcePoints, _currentHitPoints, _temporaryHitPoints};
    return payForcePointCharge(pools, forcePointCharge(adjustedSpellForcePointCost(spell), forceBodyLevel()));
}

bool Creature::commitSpellForcePointCost(const Spell &spell, int &cost) {
    cost = adjustedSpellForcePointCost(spell);
    ForcePointPools pools {_currentForce, _temporaryForcePoints, _currentHitPoints, _temporaryHitPoints};
    if (!payForcePointCharge(pools, forcePointCharge(cost, forceBodyLevel()))) return false;
    const int spentTemporaryHitPoints = _temporaryHitPoints - pools.temporaryHitPoints;
    _currentForce = pools.force;
    _temporaryForcePoints = pools.temporaryForce;
    _currentHitPoints = pools.hitPoints;
    consumeTemporaryHitPoints(spentTemporaryHitPoints);
    return true;
}

void Creature::damageForcePoints(int amount) {
    _currentForce = damagedForcePointPool(_currentForce, _temporaryForcePoints, amount);
}

void Creature::healForcePoints(int amount) {
    _currentForce = healedForcePointPool(_currentForce, _temporaryForcePoints, amount, maxForcePoints());
    if (_game.isTSL()) addForceHealingFeedback(_game, _services, *this, amount);
}

void Creature::regenerateForcePoints(int amount) {
    // Generic Heal selector 54 is distinct from direct EffectHealForcePoints.
    const int current = narrowSignedResource(static_cast<int64_t>(_currentForce) + _temporaryForcePoints);
    const int maximum = narrowSignedResource(maxForcePoints());
    int result = current + amount;
    if (result > maximum) {
        amount = maximum - current;
        result = maximum;
    }
    _currentForce = narrowSignedResource(result);
    addForceHealingFeedback(_game, _services, *this, amount);
}

void Creature::addTemporaryHitPoints(int amount, bool restoring) {
    if (!restoring || !_temporaryHitPointsRestored)
        _temporaryHitPoints = narrowSignedResource(_temporaryHitPoints) + amount;
}

int Creature::consumeTemporaryHitPoints(int amount) {
    amount = std::max(0, amount);
    const int remainder = consumeTemporaryResource(_temporaryHitPoints, amount);
    int spent = amount - remainder;
    std::vector<EffectId> depleted;
    // Commit every grant debit before removing packages: removal can execute
    // callbacks, remove siblings, or add effects and invalidate collection refs.
    for (auto &effect : _effects) {
        if (spent == 0) break;
        if (effect.type() != EffectType::TemporaryHitpoints ||
            std::find(_removingEffectApplications.begin(), _removingEffectApplications.end(),
                      effect.applicationOrder) != _removingEffectApplications.end()) continue;
        const int remaining = std::max(0, effect.integerParameter(0));
        const int debit = std::min(spent, remaining);
        if (debit == 0) continue;
        effect.setIntegerParameter(0, remaining - debit);
        spent -= debit;
        if (remaining == debit && std::find(depleted.begin(), depleted.end(), effect.id) == depleted.end())
            depleted.push_back(effect.id);
    }
    for (const auto id : depleted) removeEffectsById(id);
    return remainder;
}

void Creature::removeTemporaryHitPoints(int amount) {
    const bool wasAlive = !_dead && currentHitPoints() > 0;
    // The getter sign-extends a word, while the pool write is 32-bit.
    // Saved/mutated records can contain zero or negative remaining grants.
    _temporaryHitPoints = std::max(0, narrowSignedResource(_temporaryHitPoints) - amount);
    if (wasAlive && currentHitPoints() <= 0 && !_immortal)
        (void)applyDeathEffect(nullptr, false);
}

void Creature::addTemporaryForcePoints(int amount, bool restoring) {
    if (!restoring || !_temporaryForcePointsRestored)
        _temporaryForcePoints = narrowSignedResource(static_cast<int64_t>(_temporaryForcePoints) + amount);
}

void Creature::removeTemporaryForcePoints(int amount) {
    // Unlike temporary HP, removal subtracts the original grant even if spent.
    _temporaryForcePoints = narrowSignedResource(static_cast<int64_t>(_temporaryForcePoints) - amount);
}

void Creature::multiplyMovementRate(float multiplier) {
    _movementRate = clampMovementRate(_movementRate * multiplier);
}

void Creature::recomputeMovementRate(uint64_t removingId) {
    float rate = 1.0f;
    for (const auto &effect : effects()) {
        // Removal excludes all records with the removed group ID.
        if (effect.id == removingId) continue;
        if (!effect.hasLiveRuntimeSource()) continue;
        if (effect.serializedType == 28) rate += effect.integerParameter(0) / 100.0f;
        else if (effect.serializedType == 29) rate -= effect.integerParameter(0) / 100.0f;
    }
    _movementRate = clampMovementRate(rate);
}

void Creature::beginStateImmobilization() {
    if (!_movementTypeBeforeStateImmobilization) {
        _movementTypeBeforeStateImmobilization = _movementType;
    }
    setMovementType(MovementType::None);
}

void Creature::restoreMovementAfterState() {
    if (!_movementTypeBeforeStateImmobilization) {
        return;
    }

    MovementType previous = *_movementTypeBeforeStateImmobilization;
    _movementTypeBeforeStateImmobilization.reset();
    if (_dead ||
        _effectState == static_cast<int>(CreatureState::Stun) ||
        _effectState == static_cast<int>(CreatureState::Paralysis) ||
        _effectState == static_cast<int>(CreatureState::Sleep)) {
        setMovementType(MovementType::None);
        return;
    }
    setMovementType(previous);
}

void Creature::setInternalStateEffect(
    int state,
    int ambientState,
    uint64_t effectId) {

    _effectState = state;
    _effectAmbientState = ambientState;
    _internalStateEffectId = effectId;
    _stateSupportTimer.reset(0.0f);
    _animDirty = true;
}

void Creature::clearInternalStateEffect(uint64_t effectId) {
    if (_internalStateEffectId != effectId) {
        return;
    }
    _internalStateEffectId = 0;
    _effectState = 0;
    _effectAmbientState = 0;
    _activeStateRootId = 0;
    resumeStateDrivenAnimation();
    restoreMovementAfterState();
}

void Creature::addEffectIcon(int iconId) {
    ++_effectIconCounts[iconId];
}

void Creature::removeEffectIcon(int iconId) {
    auto it = _effectIconCounts.find(iconId);
    if (it == _effectIconCounts.end()) {
        return;
    }
    if (--it->second == 0) {
        _effectIconCounts.erase(it);
    }
}

void Creature::onStateRootApplied(const EffectInstance &pending) {
    if (pending.integerParameter(0) > _effectState) rebuildStateEffects(&pending);
}

void Creature::rebuildStateEffects(const EffectInstance *pending, uint64_t removingOrder) {
    std::optional<EffectInstance> winner;
    for (const auto &record : effects()) {
        if (record.applicationOrder == removingOrder || record.serializedType != 8 ||
            !stateHasConsumer(static_cast<CreatureState>(record.integerParameter(0)))) continue;
        if (!winner || record.integerParameter(0) > winner->integerParameter(0)) winner = record;
    }
    if (pending && (!winner || pending->integerParameter(0) > winner->integerParameter(0))) winner = *pending;
    const bool restoring = pending == nullptr || pending->restoring;
    const auto state = winner ? static_cast<CreatureState>(winner->integerParameter(0)) : CreatureState::None;
    _internalStateEffectId = 0;
    _effectState = static_cast<int>(state);
    _effectAmbientState = getStateAmbientCode(state);
    _activeStateRootId = winner ? winner->id : kUnassignedEffectId;
    for (size_t index = 0; index < _effects.size();) {
        const auto record = _effects[index];
        if (record.serializedType > 9) break;
        if (record.serializedType != 9) { ++index; continue; }
        const auto removed = removeEffectsById(record.id);
        // Apply advances after erasure; restoration consumes the shifted entry.
        if (!restoring || removed == 0) ++index;
    }
    resumeStateDrivenAnimation();
    if (!winner) { restoreMovementAfterState(); return; }
    auto internal = makeInternalStateInstance(*winner, restoring);
    internal.id = _game.allocateEffectId();
    setInternalStateEffect(static_cast<int>(state), getStateAmbientCode(state), internal.id);
    _activeStateRootId = winner->id;
    if (!applyEffect(internal)) clearInternalStateEffect(internal.id);
}

void Creature::onInternalStateRemoved(EffectId id) {
    if (_internalStateEffectId == id) _internalStateEffectId = kUnassignedEffectId;
    _effectAmbientState = getStateAmbientCode(static_cast<CreatureState>(_effectState));
    resumeStateDrivenAnimation();
}

void Creature::recomputeAIStateEffects(int pendingMask, uint64_t removingOrder) {
    int mask = pendingMask;
    for (const auto &effect : effects()) {
        if (effect.applicationOrder != removingOrder && effect.serializedType == 23) mask &= effect.integerParameter(0);
    }
    _effectAIStateMask = static_cast<uint16_t>(mask);
    if (!canMove()) setMovementType(MovementType::None);
}

void Creature::onEffectsRestored() {
    std::set<const Item *> restoredSources;
    std::set<const Item *> restoredDisguiseSources;
    for (const auto &effect : effects()) {
        if (effect.durationType() != DurationType::Equipped) continue;
        if (auto source = effect.boundCreator()) {
            if (auto *item = dyn_cast<Item>(source.get())) {
                if (isArmorClassEffect(effect)) restoredSources.insert(item);
                if (effect.serializedType == 62) restoredDisguiseSources.insert(item);
            }
        }
    }
    std::set<const Item *> visited;
    for (const auto &[slot, item] : _equipment) {
        if (!item || !visited.insert(item.get()).second) continue;
        std::deque<EffectInstance> missing;
        appendEquippedItemEffects(missing, slot, item, true);
        for (auto &effect : missing) {
            const bool restored = effect.serializedType == 62
                ? restoredDisguiseSources.count(item.get()) != 0
                : restoredSources.count(item.get()) != 0;
            if (!restored) applyEffect(std::move(effect));
        }
    }
    // Re-establish caches from the canonical records already restored by the
    // load coordinator. Never replay saves, initial damage or generated children.
    recomputeAIStateEffects();
    const EffectInstance *winner = nullptr;
    for (const auto &effect : effects()) {
        if (effect.serializedType == 8 &&
            (!winner || effect.integerParameter(0) > winner->integerParameter(0))) winner = &effect;
    }
    if (!winner) return;
    const int state = winner->integerParameter(0);
    for (const auto &effect : effects()) {
        if (effect.serializedType == 9 && effect.integerParameter(0) == state) {
            _activeStateRootId = winner->id;
            setInternalStateEffect(state, getStateAmbientCode(static_cast<CreatureState>(state)), effect.id);
            if (state == 4 || state == 5 || state == 6) beginStateImmobilization();
            break;
        }
    }
}

void Creature::resolveDamageShields(Creature &attacker) {
    auto owner = _game.getObjectById(id());
    // Advance the live index even when a callback removes the current shield;
    // appended shields participate in this same walk. Keep no record reference
    // across either application, since those callbacks may mutate the deque.
    for (size_t index = 0; index < _effects.size(); ++index) {
        const auto &shield = _effects[index];
        if (shield.type() != EffectType::DamageShield || !shield.hasLiveRuntimeSource()) continue;
        const int amount = shield.integerParameter(0) +
            rollDamageShieldContribution(_services, shield.integerParameter(1));
        const int flags = shield.integerParameter(2);
        auto effect = _game.newEffect<DamageEffect>(amount, flags, DamagePower::Normal);
        effect->setSaveFacingCreator(owner);
        attacker.applyEffect(effect, DurationType::Instant);
        auto visual = _game.newEffect<VisualEffect>(0, false, _services);
        visual->setSaveFacingCreator(owner);
        attacker.applyEffect(visual, DurationType::Instant);
    }
}

int Creature::getSpellLevel(bool applyNegativeLevels) const {
    int level = 0;
    size_t classIndex = 0;
    for (const auto &[clazz, classLevel] : _attributes.classLevels()) {
        int drained = 0;
        if (applyNegativeLevels) for (const auto &effect : effects()) {
            if (effect.serializedType == 82 && effect.integerParameter(1, -1) == static_cast<int>(classIndex))
                drained += effect.integerParameter(0);
        }
        level += std::max(0, static_cast<int>(static_cast<uint8_t>(classLevel)) -
            static_cast<int>(static_cast<uint8_t>(drained)));
        ++classIndex;
    }
    const bool balanced = isAutoBalanceEligible(_game.isTSL(), isPartyMember(),
                                                _autoBalanceContext.multiplierSet);
    const float multiplier = balanced
        ? _services.game.autoBalance.get(_autoBalanceContext.multiplierSet).levelMultiplier
        : 0.0f;
    const uint8_t globalLevel = balanced && _autoBalanceContext.playerLevelAtSpawn == 0
        ? static_cast<uint8_t>(_game.getGlobalNumber("G_PC_LEVEL")) : 0;
    return calculateSpellLevel(level, balanced,
        _autoBalanceContext.playerLevelAtSpawn, globalLevel, multiplier);
}

int Creature::getSpellSaveDC(int spellId) const {
    return calculateSpellSaveDC(_game.isTSL(), spellId, getSpellLevel(),
        getEffectiveAbilityModifier(Ability::Wisdom), getEffectiveAbilityModifier(Ability::Charisma),
        hasEffectiveFeat(FeatType::ForceFocusSense), hasEffectiveFeat(FeatType::ForceFocusAdvanced),
        hasEffectiveFeat(FeatType::ForceFocusMastery));
}

void Creature::playForceResistedAnimation() {
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(_sceneNode);
    if (body) body->playAnimation("fblock", nullptr, scene::AnimationProperties::fromFlags(
        scene::AnimationFlags::fireForget | scene::AnimationFlags::overlay));
}

void Creature::beginForcePush(const glm::vec3 &destination, float facing) {
    clearPath();
    _forcePushDestination = destination;
    _forcePushFacing = facing;
    setMovementType(MovementType::Run);
}

void Creature::endForcePush() {
    _forcePushDestination.reset();
    setMovementType(MovementType::None);
}

void Creature::updateForcePush(float dt) {
    if (!_forcePushDestination) return;
    auto module = _game.module();
    auto area = module ? module->area() : nullptr;
    if (!area || isDead()) { endForcePush(); return; }
    glm::vec2 delta = glm::vec2(*_forcePushDestination) - glm::vec2(position());
    float distance = glm::length(delta);
    if (distance == 0.0f) { endForcePush(); return; }
    float step = std::min(distance, 25.0f * dt);
    if (step <= 0.0f) return;
    bool moved = area->moveCreatureByDistance(
        _game.getObjectById<Creature>(id()), delta / distance, step);
    setFacing(_forcePushFacing);
    if (!moved || step == distance) endForcePush();
    else setMovementType(MovementType::Run);
}

void Creature::updateStateHeartbeat(float dt) {
    _stateSupportTimer.update(dt);
    if (!_stateSupportTimer.elapsed()) return;
    _stateSupportTimer.reset(randomInt(3000, 4199) / 1000.0f);
    if (_dead) return;
    if (!_onHeartbeat.empty()) _game.scriptRunner().run(_onHeartbeat, _id);
    int row;
    switch (static_cast<CreatureState>(_effectState)) {
    case CreatureState::Confusion: row = 3; break;
    case CreatureState::Fear: row = 2; break;
    default: return;
    }
    auto table = getRequiredTwoDA(_services.resource.twoDas, "statescripts");
    validateTwoDARow(*table, "statescripts", row);
    auto script = table->getString(row, "scriptname");
    if (!script.empty()) _game.scriptRunner().run(script, _id);
}

Creature::EffectStackCounts Creature::effectStackCounts() const {
    EffectStackCounts result;
    if (_effectIconCounts.empty()) {
        return result;
    }

    auto effectIcons = getRequiredTwoDA(
        _services.resource.twoDas,
        "effecticon");

    for (const auto &[iconId, count] : _effectIconCounts) {
        if (count <= 0 || iconId < 0 ||
            iconId >= effectIcons->getRowCount()) {
            continue;
        }

        auto good = effectIcons->getBoolOpt(iconId, "good");
        if (!good) {
            continue;
        }
        if (*good) {
            ++result.positive;
        } else {
            ++result.negative;
        }
    }
    return result;
}

} // namespace game

} // namespace reone
