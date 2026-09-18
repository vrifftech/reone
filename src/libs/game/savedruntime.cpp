/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "reone/game/savedruntime.h"

#include <set>

#include "reone/game/action/attackobject.h"
#include "reone/game/action/equipitem.h"
#include "reone/game/equipmentrules.h"
#include "reone/game/action/unequipitem.h"
#include "reone/game/action/usefeat.h"
#include "reone/game/action/followleader.h"
#include "reone/game/action/wait.h"
#include "reone/game/action/playanimation.h"
#include "reone/game/action/castspellatobject.h"
#include "reone/game/action/castspellatlocation.h"
#include "reone/game/d20/spells.h"
#include <cmath>
#include "reone/game/action/movetolocation.h"
#include "reone/game/action/movetoobject.h"
#include "reone/game/action/startconversation.h"
#include "reone/game/combat.h"
#include "reone/game/game.h"
#include "reone/game/location.h"
#include "reone/game/object/area.h"
#include "reone/game/script/savedsituation.h"

namespace reone {

namespace game {

std::shared_ptr<Object> SavedObjectReference::boundObject() const {
    return _object.resolve();
}

namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

struct SavedPhysicalAttack {
    SavedObjectReference target;
    FeatType feat {FeatType::Invalid};
    bool cutscene {false};
};

std::optional<SavedPhysicalAttack> decodePhysicalAttack(
    const SavedActionRecord &record) {
    if (record.actionId != 12 || record.declaredParameterCount != 10 ||
        record.parameters.size() != 10) {
        return std::nullopt;
    }
    static constexpr std::array<uint32_t, 10> types {
        1, 3, 1, 1, 1, 1, 1, 1, 1, 1};
    for (size_t index = 0; index < types.size(); ++index) {
        if (record.parameters[index].type != types[index]) {
            return std::nullopt;
        }
    }
    if (!std::holds_alternative<int32_t>(record.parameters[0].payload) ||
        !std::holds_alternative<SavedObjectReference>(
            record.parameters[1].payload)) {
        return std::nullopt;
    }
    for (size_t index = 2; index < record.parameters.size(); ++index) {
        if (!std::holds_alternative<int32_t>(
                record.parameters[index].payload)) {
            return std::nullopt;
        }
    }
    auto parameter = [&record](size_t index) {
        return std::get<int32_t>(record.parameters[index].payload);
    };
    if ((parameter(0) != 0 && parameter(0) != 1) ||
        parameter(2) != 1 || parameter(3) != 10009 ||
        parameter(4) != 1500 || parameter(5) != 1 ||
        parameter(7) != 0 || parameter(8) != 4 || parameter(9) != 0) {
        return std::nullopt;
    }
    auto feat = static_cast<FeatType>(parameter(6));
    if (feat != FeatType::Invalid && !isPhysicalAttackFeat(feat)) {
        return std::nullopt;
    }
    return SavedPhysicalAttack {
        std::get<SavedObjectReference>(record.parameters[1].payload), feat, parameter(0) != 0};
}

SavedField savedFieldFromGff(const resource::Gff::Field &field) {
    SavedField result;
    result.type = field.type;
    result.label = field.label;
    switch (field.type) {
    case resource::Gff::FieldType::Byte:
    case resource::Gff::FieldType::Word:
    case resource::Gff::FieldType::Dword:
        result.value = static_cast<uint64_t>(field.uintValue);
        break;
    case resource::Gff::FieldType::Dword64:
        result.value = field.uint64Value;
        break;
    case resource::Gff::FieldType::Char:
    case resource::Gff::FieldType::Short:
    case resource::Gff::FieldType::Int:
    case resource::Gff::FieldType::StrRef:
        result.value = static_cast<int64_t>(field.intValue);
        break;
    case resource::Gff::FieldType::Int64:
        result.value = field.int64Value;
        break;
    case resource::Gff::FieldType::Float:
        result.value = static_cast<double>(field.floatValue);
        break;
    case resource::Gff::FieldType::Double:
        result.value = field.doubleValue;
        break;
    case resource::Gff::FieldType::CExoString:
    case resource::Gff::FieldType::ResRef:
        result.value = field.strValue;
        break;
    case resource::Gff::FieldType::CExoLocString:
        result.value = SavedLocString {field.intValue, field.strValue};
        break;
    case resource::Gff::FieldType::Void:
        result.value = field.data;
        break;
    case resource::Gff::FieldType::Orientation:
        result.value = field.quatValue;
        break;
    case resource::Gff::FieldType::Vector:
        result.value = field.vecValue;
        break;
    case resource::Gff::FieldType::Struct:
    case resource::Gff::FieldType::List: {
        SavedStructChildren children;
        children.reserve(field.children.size());
        for (const auto &child : field.children) {
            children.push_back(std::make_shared<SavedStruct>(SavedStruct::fromGff(*child)));
        }
        result.value = std::move(children);
        break;
    }
    }
    return result;
}

std::vector<SavedField> collectUnsupportedFields(
    const resource::Gff &gff,
    const std::set<std::string> &known) {
    std::vector<SavedField> result;
    for (const auto &field : gff.fields()) {
        if (known.count(field.label) == 0) {
            result.push_back(savedFieldFromGff(field));
        }
    }
    return result;
}

UnsupportedSavedPayload unsupportedPayload(const resource::Gff &gff) {
    return UnsupportedSavedPayload {SavedStruct::fromGff(gff)};
}

SavedLocationValue savedLocationFromGff(const resource::Gff &gff) {
    SavedLocationValue result;
    result.position = glm::vec3(
        gff.getFloat("PositionX"),
        gff.getFloat("PositionY"),
        gff.getFloat("PositionZ"));
    result.orientation = glm::vec3(
        gff.getFloat("OrientationX"),
        gff.getFloat("OrientationY"),
        gff.getFloat("OrientationZ"));
    return result;
}

SavedScriptEvent savedScriptEventFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedScriptEvent result;
    result.type = static_cast<uint16_t>(gff.getUint("EventType"));
    for (const auto &item : gff.getList("IntList")) {
        result.integers.push_back(item->getInt("Parameter"));
    }
    for (const auto &item : gff.getList("FloatList")) {
        result.floats.push_back(item->getFloat("Parameter"));
    }
    for (const auto &item : gff.getList("StringList")) {
        result.strings.push_back(item->getString("Parameter"));
    }
    for (const auto &item : gff.getList("ObjectList")) {
        result.objects.push_back(SavedObjectReference::fromSerializedId(
            item->getUint("Parameter"), identityContext));
    }
    return result;
}

SavedSpellImpact savedSpellImpactFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedSpellImpact result;
    result.spellId = gff.getInt("SpellId");
    result.caster = SavedObjectReference::fromSerializedId(
        gff.getUint("CasterId"), identityContext);
    result.target = SavedObjectReference::fromSerializedId(
        gff.getUint("TargetId"), identityContext);
    result.area = SavedObjectReference::fromSerializedId(
        gff.getUint("AreaId"), identityContext);
    result.item = SavedObjectReference::fromSerializedId(
        gff.getUint("ItemId"), identityContext);
    result.script = gff.getString("Script");
    result.targetPosition = glm::vec3(
        gff.getFloat("TargetPosX"),
        gff.getFloat("TargetPosY"),
        gff.getFloat("TargetPosZ"));
    result.finalForceCost = gff.getInt("FinalForceCost");
    result.casterLevel = gff.getInt("ReoneCastLevel", -1);
    result.metaMagic = gff.getInt("ReoneMetaMagic", 0);
    result.targetFacing = gff.getFloat("ReoneFacing", 0.0f);
    return result;
}

SavedBodyBag savedBodyBagFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedBodyBag result;
    result.object = SavedObjectReference::fromSerializedId(
        gff.getUint("BodyBagId"), identityContext);
    result.position = glm::vec3(
        gff.getFloat("PositionX"),
        gff.getFloat("PositionY"),
        gff.getFloat("PositionZ"));
    return result;
}

SavedCombatAttack savedCombatAttackFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedCombatAttack result;
    result.data = SavedStruct::fromGff(gff);
    auto &fields = *result.fields;
    // LoadData defaults scalar fields to zero, unlike ClearAttackData.
    fields.group = static_cast<uint8_t>(gff.getUint("AttackGroup", 0));
    fields.animationLength = static_cast<uint16_t>(gff.getUint("AnimationLength", 0));
    fields.missedBy = gff.getUint("MissedBy", 0);
    fields.result = static_cast<uint8_t>(gff.getUint("AttackResult", 0));
    fields.reactionDelay = static_cast<uint16_t>(gff.getUint("ReaxnDelay", 0));
    fields.reactionAnimation = static_cast<uint16_t>(gff.getUint("ReaxnAnimation", 0));
    fields.reactionAnimationLength = static_cast<uint16_t>(gff.getUint("ReaxnAnimLength", 0));
    fields.concealment = static_cast<uint8_t>(gff.getUint("Concealment", 0));
    fields.ranged = gff.getInt("RangedAttack", 0);
    fields.sneakAttack = gff.getInt("SneakAttack", 0);
    fields.weaponAttackType = static_cast<uint8_t>(gff.getUint("WeaponAttackType", 0));
    fields.rangedTarget = {gff.getFloat("RangedTargetX"),
                           gff.getFloat("RangedTargetY"),
                           gff.getFloat("RangedTargetZ")};
    const auto &damage = gff.getList("DamageList");
    // storage has 15 entries. Bound malformed oversized lists rather
    // than reproducing the out-of-bounds write.
    for (size_t i = 0; i < std::min(damage.size(), fields.damage.size()); ++i) {
        if (damage[i]) fields.damage[i] = static_cast<int16_t>(damage[i]->getInt("DamageValue", 0));
    }
    fields.killingBlow = static_cast<uint8_t>(gff.getUint("KillingBlow", 0));
    fields.coupDeGrace = static_cast<uint8_t>(gff.getUint("CoupDeGrace", 0));
    fields.criticalThreat = static_cast<uint8_t>(gff.getUint("CriticalThreat", 0));
    fields.deflected = static_cast<uint8_t>(gff.getUint("AttackDeflected", 0));
    fields.attackDebugText = gff.getString("AttackDebugText");
    fields.damageDebugText = gff.getString("DamageDebugText");
    result.history->type = static_cast<uint16_t>(gff.getUint("AttackType", 0));
    result.history->mode = static_cast<uint8_t>(gff.getUint("AttackMode", 0));
    result.reactionObject = SavedObjectReference::fromSerializedId(
        gff.getUint("ReactObject"), identityContext);
    result.ammoItem = SavedObjectReference::fromSerializedId(
        gff.getUint("AmmoItem"), identityContext);
    return result;
}

SavedFeedbackMessage savedFeedbackMessageFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedFeedbackMessage result;
    result.data = SavedStruct::fromGff(gff);
    for (const auto &item : gff.getList("ObjectIDList")) {
        result.objects.push_back(SavedObjectReference::fromSerializedId(
            item->getUint("ObjectValue"), identityContext));
    }
    return result;
}

SavedTalentValue savedTalentFromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedTalentValue result;
    result.id = gff.getInt("ID");
    result.type = gff.getInt("Type");
    result.multiClass = static_cast<uint8_t>(gff.getUint("MultiClass"));
    result.item = SavedObjectReference::fromSerializedId(
        gff.getUint("Item"), identityContext);
    // The game passes "ItemPropertyIndex" to a 16-byte GFF label API; the
    // on-wire label is therefore truncated to this value.
    result.itemPropertyIndex = gff.getInt("ItemPropertyInde");
    result.casterLevel = static_cast<uint8_t>(gff.getUint("CasterLevel"));
    result.metaType = static_cast<uint8_t>(gff.getUint("MetaType"));
    return result;
}

struct SavedMoveToPoint {
    glm::vec3 destination {0.0f};
    SavedObjectReference area;
    SavedObjectReference target;
    bool run {false};
    float range {0.0f};
    float timeout {0.0f};
    bool forcedPending {false};
    bool forcedActive {false};
    uint32_t expiryDay {0};
    uint32_t expiryTime {0};
};

std::optional<SavedMoveToPoint> decodeMoveToPoint(const SavedActionRecord &record) {
    if (record.actionId != 1 || record.declaredParameterCount != 13 ||
        record.parameters.size() != 13) {
        return std::nullopt;
    }
    static constexpr std::array<uint32_t, 13> types {
        2, 2, 2, 3, 3, 1, 2, 1, 2, 2, 2, 1, 1};
    for (size_t i = 0; i < types.size(); ++i) {
        if (record.parameters[i].type != types[i]) {
            return std::nullopt;
        }
    }
    bool payloads =
        std::holds_alternative<float>(record.parameters[0].payload) &&
        std::holds_alternative<float>(record.parameters[1].payload) &&
        std::holds_alternative<float>(record.parameters[2].payload) &&
        std::holds_alternative<SavedObjectReference>(record.parameters[3].payload) &&
        std::holds_alternative<SavedObjectReference>(record.parameters[4].payload) &&
        std::holds_alternative<int32_t>(record.parameters[5].payload) &&
        std::holds_alternative<float>(record.parameters[6].payload) &&
        std::holds_alternative<int32_t>(record.parameters[7].payload) &&
        std::holds_alternative<float>(record.parameters[8].payload) &&
        std::holds_alternative<float>(record.parameters[9].payload) &&
        std::holds_alternative<float>(record.parameters[10].payload) &&
        std::holds_alternative<int32_t>(record.parameters[11].payload) &&
        std::holds_alternative<int32_t>(record.parameters[12].payload);
    if (!payloads) {
        return std::nullopt;
    }

    SavedMoveToPoint result;
    result.destination = glm::vec3(
        std::get<float>(record.parameters[0].payload),
        std::get<float>(record.parameters[1].payload),
        std::get<float>(record.parameters[2].payload));
    result.area = std::get<SavedObjectReference>(record.parameters[3].payload);
    result.target = std::get<SavedObjectReference>(record.parameters[4].payload);
    int32_t flags = std::get<int32_t>(record.parameters[5].payload);
    result.run = (flags & 1) != 0;
    result.range = std::get<float>(record.parameters[6].payload);
    int32_t subtype = std::get<int32_t>(record.parameters[7].payload);
    result.timeout = std::get<float>(record.parameters[8].payload);
    glm::vec2 offset(
        std::get<float>(record.parameters[9].payload),
        std::get<float>(record.parameters[10].payload));
    int32_t day = std::get<int32_t>(record.parameters[11].payload);
    int32_t time = std::get<int32_t>(record.parameters[12].payload);
    bool timed = (flags & 4) != 0;
    bool ordinary = !timed && day == 0 && time == 0 && result.timeout == 0.0f;
    result.forcedPending = timed && result.timeout > 0.0f && day == 0 && time == 0;
    // Only non-negative day/time are required. A time of day larger than the
    // current day length is unnormalized, not invalid: records written before
    // the day length became Mod_MinPerHour-derived used a fixed 24-hour scale,
    // and composing them into absolute milliseconds carries the excess into
    // later days. Snapshots written from here on are always normalized.
    result.forcedActive = !timed && (day != 0 || time != 0) &&
                          result.timeout == 0.0f && day >= 0 && time >= 0;
    bool valid =
        std::isfinite(result.destination.x) && std::isfinite(result.destination.y) &&
        std::isfinite(result.destination.z) && std::isfinite(result.range) &&
        result.range >= 0.0f && std::isfinite(result.timeout) &&
        std::isfinite(offset.x) && std::isfinite(offset.y) &&
        offset == glm::vec2(0.0f) && subtype == 0 && (flags & ~5) == 0 &&
        !result.area.isInvalid() &&
        (ordinary || result.forcedPending || result.forcedActive) &&
        (result.target.isInvalid() ? result.range == 0.0f : !ordinary);
    if (!valid) {
        return std::nullopt;
    }
    result.expiryDay = static_cast<uint32_t>(day);
    result.expiryTime = static_cast<uint32_t>(time);
    return result;
}

bool bindReference(const Game &game, SavedObjectReference &reference, bool &allBound) {
    if (reference.isInvalid()) {
        return true;
    }
    bool bound = game.bindSavedObjectReference(reference);
    allBound = allBound && bound;
    return bound;
}

} // namespace

SavedStruct SavedStruct::fromGff(const resource::Gff &gff) {
    SavedStruct result;
    result.type = gff.type();
    result.fields.reserve(gff.fields().size());
    for (const auto &field : gff.fields()) {
        result.fields.push_back(savedFieldFromGff(field));
    }
    return result;
}

SerializedScriptSituation SerializedScriptSituation::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SerializedScriptSituation result;
    result.codeSize = gff.getInt("CodeSize");
    result.code = gff.getData("Code");
    result.crc = gff.getUint("CRC");
    result.instructionPointer = gff.getInt("InstructionPtr");
    result.secondaryPointer = gff.getInt("SecondaryPtr");
    result.scriptName = gff.getString("Name");
    result.stackSize = gff.getInt("StackSize");
    result.unsupportedFields = collectUnsupportedFields(
        gff,
        {"CodeSize", "Code", "CRC", "InstructionPtr", "SecondaryPtr", "Name", "StackSize", "Stack"});

    auto stackStruct = gff.findStruct("Stack");
    if (!stackStruct) {
        return result;
    }
    result.basePointer = stackStruct->getInt("BasePointer");
    result.stackPointer = stackStruct->getInt("StackPointer");
    result.totalSize = stackStruct->getInt("TotalSize");
    for (const auto &item : stackStruct->getList("Stack")) {
        SavedVmStackValue value;
        int8_t type = 0;
        item->readChar(type, "Type");
        value.type = type;
        switch (static_cast<SavedVmStackType>(type)) {
        case SavedVmStackType::Integer:
            value.payload = item->getInt("Value");
            break;
        case SavedVmStackType::Float:
            value.payload = item->getFloat("Value");
            break;
        case SavedVmStackType::String:
            value.payload = item->getString("Value");
            break;
        case SavedVmStackType::Object:
            value.payload = SavedObjectReference::fromSerializedId(
                item->getUint("Value"), identityContext);
            break;
        case SavedVmStackType::Effect: {
            auto structure = item->findStruct("GameDefinedStrct");
            value.payload = structure ? SavedVmStackPayload(EffectInstance::fromGff(
                                                    *structure, identityContext))
                                      : SavedVmStackPayload(unsupportedPayload(*item));
            break;
        }
        case SavedVmStackType::Event: {
            auto structure = item->findStruct("GameDefinedStrct");
            value.payload = structure ? SavedVmStackPayload(savedScriptEventFromGff(
                                                    *structure, identityContext))
                                      : SavedVmStackPayload(unsupportedPayload(*item));
            break;
        }
        case SavedVmStackType::Location: {
            auto structure = item->findStruct("GameDefinedStrct");
            value.payload = structure ? SavedVmStackPayload(savedLocationFromGff(*structure))
                                      : SavedVmStackPayload(unsupportedPayload(*item));
            break;
        }
        case SavedVmStackType::Talent: {
            auto structure = item->findStruct("GameDefinedStrct");
            value.payload = structure ? SavedVmStackPayload(savedTalentFromGff(
                                                    *structure, identityContext))
                                      : SavedVmStackPayload(unsupportedPayload(*item));
            break;
        }
        default:
            value.payload = unsupportedPayload(*item);
            break;
        }
        result.stack.push_back(std::move(value));
    }
    return result;
}

bool SerializedScriptSituation::bindObjectReferences(const Game &game) {
    if (_runtimeSession && *_runtimeSession != game._runtimeSessionGeneration) {
        _referencesBound = false;
        return false;
    }
    if (!_runtimeSession) {
        _runtimeSession = game._runtimeSessionGeneration;
    }

    bool allBound = true;
    for (auto &entry : stack) {
        std::visit(
            Overloaded {
                [](UnsupportedSavedPayload &) {},
                [](int32_t &) {},
                [](float &) {},
                [](std::string &) {},
                [&game, &allBound](SavedObjectReference &reference) {
                    bindReference(game, reference, allBound);
                },
                [&game, &allBound](EffectInstance &effect) {
                    allBound = game.bindEffectCreator(effect) && allBound;
                },
                [&game, &allBound](SavedScriptEvent &event) {
                    for (auto &reference : event.objects) {
                        bindReference(game, reference, allBound);
                    }
                },
                [](SavedLocationValue &) {},
                [&game, &allBound](SavedTalentValue &talent) {
                    bindReference(game, talent.item, allBound);
                },
            },
            entry.payload);
    }
    _referencesBound = allBound;
    return allBound;
}

bool SerializedScriptSituation::isBoundToCurrentRuntimeSession(const Game &game) const {
    return _referencesBound && _runtimeSession &&
           *_runtimeSession == game._runtimeSessionGeneration;
}

SavedActionParameter SavedActionParameter::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedActionParameter result;
    result.type = gff.getUint("Type");
    switch (static_cast<SavedActionParameterType>(result.type)) {
    case SavedActionParameterType::Integer:
        result.payload = gff.getInt("Value");
        break;
    case SavedActionParameterType::Float:
        result.payload = gff.getFloat("Value");
        break;
    case SavedActionParameterType::Object:
        result.payload = SavedObjectReference::fromSerializedId(
            gff.getUint("Value"), identityContext);
        break;
    case SavedActionParameterType::String:
        result.payload = gff.getString("Value");
        break;
    case SavedActionParameterType::ScriptSituation: {
        auto value = gff.findStruct("Value");
        result.payload = value ? SavedActionParameterPayload(
                                     SerializedScriptSituation::fromGff(
                                         *value, identityContext))
                               : SavedActionParameterPayload(unsupportedPayload(gff));
        break;
    }
    default:
        result.payload = unsupportedPayload(gff);
        break;
    }
    return result;
}

bool SavedActionParameter::bindObjectReferences(const Game &game) {
    if (auto reference = std::get_if<SavedObjectReference>(&payload)) {
        return reference->isInvalid() || game.bindSavedObjectReference(*reference);
    }
    if (auto situation = std::get_if<SerializedScriptSituation>(&payload)) {
        return situation->bindObjectReferences(game);
    }
    return true;
}


namespace {
using GffState = resource::Gff;
using StateField = resource::Gff::Field;
std::shared_ptr<GffState> scalarState(const std::vector<int> &values) {
    std::vector<std::shared_ptr<GffState>> fields;
    for (int value : values) fields.push_back(GffState::Builder().type(0).field(StateField::newInt("Value", value)).build());
    return GffState::Builder().type(0).field(StateField::newList("Values", std::move(fields))).build();
}
int stateValue(const resource::Gff &g, size_t index, int fallback = 0) {
    const auto &v = g.getList("Values");
    return index < v.size() ? v[index]->getInt("Value", fallback) : fallback;
}
}

std::shared_ptr<resource::Gff> SavedWeaponImpact::toGff() const {
    std::vector<std::shared_ptr<GffState>> apps, messages;
    for (const auto &a : applications) {
        const auto &e = a.effectOutcome;
        auto g = scalarState({static_cast<int>(a.subtype), a.parameter, a.emitEffectOutcome,
            e.present, e.saveType, e.effectType, e.saveMode, e.saveRoll, e.modifierTotal,
            e.baseSave, e.finalTotal, e.difficultyClass, e.outcome});
        g->fields().push_back(StateField::newFloat("Duration", a.duration));
        apps.push_back(std::move(g));
    }
    for (const auto &entry : feedback) {
        if (const auto *a = std::get_if<AbilityDrainFeedback>(&entry))
            messages.push_back(scalarState({0, static_cast<int>(a->ability), a->amount, a->durationSeconds}));
        else if (const auto *v = std::get_if<SavingThrowFeedback>(&entry))
            messages.push_back(scalarState({1, static_cast<int>(v->savingThrow), v->base, v->modifier, v->roll, v->difficultyClass}));
    }
    return GffState::Builder().type(0x6666).field(StateField::newDword("ReoneWeaponHit", 1))
        .field(StateField::newDword("Source", source.id)).field(StateField::newStruct("Damage", damage.toGff()))
        .field(StateField::newList("Applications", std::move(apps)))
        .field(StateField::newList("Feedback", std::move(messages))).build();
}
SavedWeaponImpact SavedWeaponImpact::fromGff(const resource::Gff &g, const SerializedIdentityContext &ids) {
    SavedWeaponImpact result;
    result.source = SavedObjectReference::fromSerializedId(g.getUint("Source", kSavedRuntimeInvalidObjectId), ids);
    if (auto d = g.findStruct("Damage")) result.damage = EffectInstance::fromGff(*d, ids);
    for (const auto &v : g.getList("Applications")) {
        ItemOnHitApplication a;
        a.subtype = static_cast<ItemOnHitSubtype>(stateValue(*v, 0)); a.parameter = stateValue(*v, 1);
        a.emitEffectOutcome = stateValue(*v, 2) != 0; a.duration = v->getFloat("Duration");
        auto &e = a.effectOutcome;
        e.present = stateValue(*v, 3) != 0; e.saveType = stateValue(*v, 4); e.effectType = stateValue(*v, 5);
        e.saveMode = stateValue(*v, 6); e.saveRoll = stateValue(*v, 7); e.modifierTotal = stateValue(*v, 8);
        e.baseSave = stateValue(*v, 9); e.finalTotal = stateValue(*v, 10);
        e.difficultyClass = stateValue(*v, 11); e.outcome = stateValue(*v, 12, -1);
        result.applications.push_back(std::move(a));
    }
    for (const auto &v : g.getList("Feedback")) {
        if (stateValue(*v, 0) == 0) result.feedback.push_back(AbilityDrainFeedback {
            static_cast<Ability>(stateValue(*v, 1)), stateValue(*v, 2), stateValue(*v, 3)});
        else result.feedback.push_back(SavingThrowFeedback {static_cast<SavingThrow>(stateValue(*v, 1)),
            stateValue(*v, 2), stateValue(*v, 3), stateValue(*v, 4), stateValue(*v, 5)});
    }
    return result;
}

std::shared_ptr<resource::Gff> SavedPhysicalAction::toGff() const {
    std::vector<std::shared_ptr<GffState>> refs, effects, records;
    for (const auto &ref : sources) refs.push_back(GffState::Builder().type(0)
        .field(StateField::newDword("Object", ref.id)).build());
    for (const auto &effect : secondaryEffects) effects.push_back(effect.toGff());
    for (const auto &history : histories) {
        auto g = GffState::Builder().type(0x2222).build(); history.writeFields(*g); records.push_back(std::move(g));
    }
    return GffState::Builder().type(0).field(StateField::newDword("Version", 1))
        .field(StateField::newStruct("State", state)).field(StateField::newList("Sources", std::move(refs)))
        .field(StateField::newList("Effects", std::move(effects))).field(StateField::newList("History", std::move(records))).build();
}
SavedPhysicalAction SavedPhysicalAction::fromGff(const resource::Gff &g, const SerializedIdentityContext &ids) {
    SavedPhysicalAction result;
    if (g.getUint("Version") != 1) return result;
    result.state = g.findStruct("State");
    for (const auto &v : g.getList("Sources")) result.sources.push_back(SavedObjectReference::fromSerializedId(
        v->getUint("Object", kSavedRuntimeInvalidObjectId), ids));
    for (const auto &v : g.getList("Effects")) result.secondaryEffects.push_back(EffectInstance::fromGff(*v, ids));
    for (const auto &v : g.getList("History")) result.histories.push_back(savedCombatAttackFromGff(*v, ids));
    return result;
}
bool SavedPhysicalAction::valid() const {
    if (!state || state->getInt("Phase", -1) < 0 || state->getInt("Phase") > 5) return false;
    const auto size = state->getList("Attacks").size();
    return sources.size() == size * 2 && histories.size() == size && secondaryEffects.size() == size &&
        std::isfinite(state->getFloat("Time")) && state->getFloat("Time") >= 0.0f;
}

bool SavedCastAction::valid() const {
    return spellId >= 0 && phase >= 0 && phase <= 7 && (path <= 3 || (path >= 5 && path <= 9) || path == 11) &&
        std::isfinite(elapsed) && elapsed >= 0.0f &&
        std::isfinite(conjureTime) && conjureTime >= 0.0f &&
        std::isfinite(castTime) && castTime >= 0.0f &&
        std::isfinite(catchTime) && catchTime >= 0.0f &&
        std::isfinite(projectileTime) && projectileTime >= 0.0f &&
        (!(flags & Released) || (flags & Committed)) &&
        (!(flags & Committed) || (flags & CommitAttempted)) &&
        (!(flags & ItemCast) || itemProperty >= 0);
}

SavedCastAction SavedCastAction::fromGff(const resource::Gff &g, const SerializedIdentityContext &ids) {
    SavedCastAction s;
    if (g.getUint("Version") != 1) return s;
    s.spellId = g.getInt("Spell", -1);
    s.target = SavedObjectReference::fromSerializedId(g.getUint("Target", kSavedRuntimeInvalidObjectId), ids);
    s.item = SavedObjectReference::fromSerializedId(g.getUint("Item", kSavedRuntimeInvalidObjectId), ids);
    s.position = g.getVector("Position"); s.facing = g.getFloat("Facing");
    s.itemProperty = g.getInt("Property", -1); s.itemCasterLevel = g.getInt("ItemLevel", -1);
    s.casterLevel = g.getInt("CasterLevel"); s.forceCost = g.getInt("ForceCost");
    s.phase = g.getInt("Phase"); s.elapsed = g.getFloat("Elapsed");
    s.conjureTime = g.getFloat("ConjureTime"); s.castTime = g.getFloat("CastTime"); s.catchTime = g.getFloat("CatchTime");
    s.projectileTime = g.getFloat("FlightTime"); s.presentationId = g.getUint64("Presentation");
    s.flags = g.getUint("Flags"); s.path = static_cast<uint8_t>(g.getUint("Path"));
    return s;
}

std::shared_ptr<resource::Gff> SavedCastAction::toGff() const {
    using G = resource::Gff;
    return G::Builder().type(0)
        .field(G::Field::newDword("Version", 1)).field(G::Field::newInt("Spell", spellId))
        .field(G::Field::newDword("Target", target.id)).field(G::Field::newDword("Item", item.id))
        .field(G::Field::newVector("Position", position)).field(G::Field::newFloat("Facing", facing))
        .field(G::Field::newInt("Property", itemProperty)).field(G::Field::newInt("ItemLevel", itemCasterLevel))
        .field(G::Field::newInt("CasterLevel", casterLevel)).field(G::Field::newInt("ForceCost", forceCost))
        .field(G::Field::newInt("Phase", phase)).field(G::Field::newFloat("Elapsed", elapsed))
        .field(G::Field::newFloat("ConjureTime", conjureTime)).field(G::Field::newFloat("CastTime", castTime)).field(G::Field::newFloat("CatchTime", catchTime))
        .field(G::Field::newFloat("FlightTime", projectileTime)).field(G::Field::newDword64("Presentation", presentationId))
        .field(G::Field::newDword("Flags", flags)).field(G::Field::newByte("Path", path)).build();
}

SavedRoundClock SavedRoundClock::fromGff(const resource::Gff &g, const SerializedIdentityContext &ids) {
    SavedRoundClock s;
    s.id = g.getUint64("Id"); s.slot = g.getInt("Slot"); s.state = g.getInt("State");
    s.elapsed = g.getFloat("Elapsed"); s.duration = g.getFloat("Duration", 3.0f);
    s.pauseRemaining = g.getFloat("PauseRemaining");
    s.pauseOwner = SavedObjectReference::fromSerializedId(g.getUint("PauseOwner", kSavedRuntimeInvalidObjectId), ids);
    s.master = SavedObjectReference::fromSerializedId(g.getUint("Master", kSavedRuntimeInvalidObjectId), ids);
    s.engaged = SavedObjectReference::fromSerializedId(g.getUint("Engaged", kSavedRuntimeInvalidObjectId), ids);
    return s;
}
std::shared_ptr<resource::Gff> SavedRoundClock::toGff() const {
    using G = resource::Gff;
    return G::Builder().type(0).field(G::Field::newDword64("Id", id))
        .field(G::Field::newInt("Slot", slot)).field(G::Field::newInt("State", state))
        .field(G::Field::newFloat("Elapsed", elapsed)).field(G::Field::newFloat("Duration", duration))
        .field(G::Field::newFloat("PauseRemaining", pauseRemaining))
        .field(G::Field::newDword("PauseOwner", pauseOwner.id))
        .field(G::Field::newDword("Master", master.id)).field(G::Field::newDword("Engaged", engaged.id)).build();
}

SavedProjectile SavedProjectile::fromGff(const resource::Gff &g, const SerializedIdentityContext &ids) {
    SavedProjectile p;
    auto ref = [&](const resource::Gff &r, const char *key) {
        return SavedObjectReference::fromSerializedId(r.getUint(key, kSavedRuntimeInvalidObjectId), ids);
    };
    p.id = g.getUint64("Id"); p.kind = g.getInt("Kind");
    p.caster = ref(g, "Caster"); p.weapon = ref(g, "Weapon");
    p.spellId = g.getInt("Spell", -1); p.path = g.getInt("Path"); p.model = g.getString("Model");
    p.leg = g.getInt("Leg"); p.released = g.getBool("Released");
    p.initialized = g.getBool("Initialized"); p.clockwise = g.getBool("Clockwise");
    p.elapsed = g.getFloat("Elapsed"); p.position = g.getVector("Position");
    p.velocity = g.getVector("Velocity"); p.orientation = g.getOrientation("Orientation");
    p.acceleration = g.getVector("Acceleration"); p.parryBlocked = g.getBool("ParryBlocked");
    p.activationDelay = g.getFloat("ActivationDelay"); p.travelRate = g.getFloat("TravelRate");
    p.sourceHook = g.getString("SourceHook"); p.orientationMode = g.getInt("OrientMode");
    p.targetHook = g.getString("TargetHook", "impact");
    p.combatResult = g.getInt("CombatResult"); p.soundVariant = g.getInt("SoundVariant");
    for (const auto &v : g.getList("Legs")) p.legs.push_back({ref(*v, "Source"), ref(*v, "Target"),
        v->getVector("Origin"), v->getVector("Destination"), v->getFloat("Duration"), v->getBool("Reacted"),
        v->getInt("Motion", 1), v->getVector("TargetOffset"), v->getString("TargetHook"),
        v->getBool("OwnTargetHook"), v->getFloat("StopRadius")});
    return p;
}
std::shared_ptr<resource::Gff> SavedProjectile::toGff() const {
    using G = resource::Gff;
    std::vector<std::shared_ptr<G>> entries;
    for (const auto &l : legs) entries.push_back(G::Builder().type(0)
        .field(G::Field::newDword("Source", l.source.id)).field(G::Field::newDword("Target", l.target.id))
        .field(G::Field::newVector("Origin", l.origin)).field(G::Field::newVector("Destination", l.destination))
        .field(G::Field::newFloat("Duration", l.duration)).field(G::Field::newByte("Reacted", l.reacted))
        .field(G::Field::newInt("Motion", l.motion)).field(G::Field::newVector("TargetOffset", l.targetOffset))
        .field(G::Field::newCExoString("TargetHook", l.targetHook))
        .field(G::Field::newByte("OwnTargetHook", l.ownsTargetHook))
        .field(G::Field::newFloat("StopRadius", l.stopRadius)).build());
    return G::Builder().type(0).field(G::Field::newDword("Version", kind == 2 ? 3 : 2))
        .field(G::Field::newDword64("Id", id)).field(G::Field::newInt("Kind", kind))
        .field(G::Field::newDword("Caster", caster.id)).field(G::Field::newDword("Weapon", weapon.id))
        .field(G::Field::newInt("Spell", spellId)).field(G::Field::newInt("Path", path))
        .field(G::Field::newResRef("Model", model)).field(G::Field::newInt("Leg", leg))
        .field(G::Field::newByte("Released", released)).field(G::Field::newByte("Initialized", initialized))
        .field(G::Field::newByte("Clockwise", clockwise)).field(G::Field::newFloat("Elapsed", elapsed))
        .field(G::Field::newVector("Position", position)).field(G::Field::newVector("Velocity", velocity))
         .field(G::Field::newVector("Acceleration", acceleration))
        .field(G::Field::newByte("ParryBlocked", parryBlocked))
        .field(G::Field::newFloat("ActivationDelay", activationDelay))
        .field(G::Field::newFloat("TravelRate", travelRate))
        .field(G::Field::newCExoString("SourceHook", sourceHook))
        .field(G::Field::newCExoString("TargetHook", targetHook))
        .field(G::Field::newInt("CombatResult", combatResult))
        .field(G::Field::newInt("SoundVariant", soundVariant))
        .field(G::Field::newInt("OrientMode", orientationMode))
        .field(G::Field::newOrientation("Orientation", orientation)).field(G::Field::newList("Legs", std::move(entries))).build();
}
bool SavedProjectile::bindObjectReferences(const Game &game) {
    const bool live = game.bindSavedObjectReference(caster);
    game.bindSavedObjectReference(weapon);
    for (auto &l : legs) { game.bindSavedObjectReference(l.source); game.bindSavedObjectReference(l.target); }
    return live;
}

SavedScheduledAction SavedScheduledAction::fromGff(
    const resource::Gff &gff, const SerializedIdentityContext &ids) {
    SavedScheduledAction record;
    record.timer = gff.getInt("ActionTimer");
    record.animation = static_cast<uint16_t>(gff.getUint("Animation"));
    record.animationTime = gff.getInt("AnimationTime");
    record.numAttacks = gff.getInt("NumAttacks");
    record.type = static_cast<uint8_t>(gff.getUint("ActionType"));
    record.target = SavedObjectReference::fromSerializedId(gff.getUint("Target", kSavedRuntimeInvalidObjectId), ids);
    record.retargettable = static_cast<uint8_t>(gff.getUint("Retargettable"));
    record.inventorySlot = gff.getUint("InventorySlot");
    record.repository = SavedObjectReference::fromSerializedId(gff.getUint("TargetRepository", kSavedRuntimeInvalidObjectId), ids);
    record.applied = gff.getInt("ReoneApplied") != 0;
    record.remainingPause = gff.getFloat("ReonePause");
    if (auto command = gff.findStruct("ReoneCommand")) {
        record.command = SavedActionRecord::fromGff(*command, ids);
    } else if (record.type == 13 && (record.numAttacks == 0 || record.numAttacks == 1)) {
        SavedActionRecord command;
        command.actionId = record.numAttacks == 0 ? 68 : 69;
        if (record.numAttacks == 0) command.parameters.push_back({3, record.target});
        command.declaredParameterCount = static_cast<uint16_t>(command.parameters.size());
        record.command = std::move(command);
    } else if (record.type == 14) {
        SavedActionRecord command;
        command.actionId = 71;
        record.command = std::move(command);
    } else if (record.isEquipment()) {
        SavedActionRecord command;
        command.actionId = record.type == 6 ? 8 : 11;
        command.declaredParameterCount = 3;
        command.parameters.push_back({3, record.target});
        if (record.type == 6) command.parameters.push_back({1, static_cast<int32_t>(record.inventorySlot)});
        else command.parameters.push_back({3, record.repository});
        command.parameters.push_back({1, int32_t {0}});
        record.command = std::move(command);
    }
    record.unsupportedFields = collectUnsupportedFields(gff,
        {"ActionTimer", "Animation", "AnimationTime", "NumAttacks", "ActionType",
         "Target", "Retargettable", "InventorySlot", "TargetRepository",
         "ReoneCommand", "ReoneApplied", "ReonePause"});
    return record;
}

bool SavedScheduledAction::bindObjectReferences(const Game &game) {
    const bool targetBound = game.bindSavedObjectReference(target);
    const bool repositoryBound = game.bindSavedObjectReference(repository);
    const bool commandBound = !command || command->bindObjectReferences(game);
    return targetBound && repositoryBound && commandBound;
}

SavedActionRecord SavedActionRecord::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedActionRecord result;
    result.actionId = gff.getUint("ActionId");
    result.scheduled = gff.getBool("ReoneScheduled");
    result.groupActionId = static_cast<uint16_t>(gff.getUint("GroupActionId"));
    result.declaredParameterCount = static_cast<uint16_t>(gff.getUint("NumParams"));
    for (const auto &parameter : gff.getList("Paramaters")) {
        result.parameters.push_back(SavedActionParameter::fromGff(
            *parameter, identityContext));
    }
    if (auto value = gff.findStruct("ReoneCast")) result.cast = SavedCastAction::fromGff(*value, identityContext);
    if (auto value = gff.findStruct("ReoneRound")) result.round = SavedRoundClock::fromGff(*value, identityContext);
    if (auto value = gff.findStruct("ReonePhysical")) result.physical = SavedPhysicalAction::fromGff(*value, identityContext);
    result.unsupportedFields = collectUnsupportedFields(
        gff,
        {"ActionId", "GroupActionId", "NumParams", "Paramaters", "ReoneCast", "ReoneRound", "ReonePhysical", "ReoneScheduled"});
    return result;
}

SavedExecutionSupport SavedActionRecord::executionSupport() const {
    if (actionId == 15 && cast) return cast->valid()
        ? SavedExecutionSupport::Executable : SavedExecutionSupport::RepresentableButUnsupported;
    if ((actionId == 8 || actionId == 11) &&
        declaredParameterCount == 3 && parameters.size() == 3 &&
        parameters[0].type == static_cast<uint32_t>(SavedActionParameterType::Object) &&
        std::holds_alternative<SavedObjectReference>(parameters[0].payload) &&
        parameters[2].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        std::holds_alternative<int32_t>(parameters[2].payload)) {
        const bool validSecond = actionId == 8
            ? parameters[1].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
              std::holds_alternative<int32_t>(parameters[1].payload) &&
              equipmentSlotFromMask(static_cast<uint32_t>(std::get<int32_t>(parameters[1].payload))).has_value()
            : parameters[1].type == static_cast<uint32_t>(SavedActionParameterType::Object) &&
              std::holds_alternative<SavedObjectReference>(parameters[1].payload);
        return validSecond ? SavedExecutionSupport::Executable
                           : SavedExecutionSupport::RepresentableButUnsupported;
    }
    if (actionId == 63 && declaredParameterCount == 1 && parameters.size() == 1 &&
        parameters[0].type == 1 && std::holds_alternative<int32_t>(parameters[0].payload))
        return SavedExecutionSupport::Executable;
    if (actionId == 61 && declaredParameterCount == 0 && parameters.empty()) {
        return SavedExecutionSupport::Executable;
    }
    if (actionId == 12) {
        if (physical && !physical->valid()) return SavedExecutionSupport::RepresentableButUnsupported;
        return decodePhysicalAttack(*this)
                   ? SavedExecutionSupport::Executable
                   : SavedExecutionSupport::RepresentableButUnsupported;
    }
    if (actionId == 24 && declaredParameterCount == 3 && parameters.size() == 3 &&
        parameters[0].type == static_cast<uint32_t>(SavedActionParameterType::Object) &&
        parameters[1].type == static_cast<uint32_t>(SavedActionParameterType::String) &&
        parameters[2].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        std::holds_alternative<SavedObjectReference>(parameters[0].payload) &&
        std::holds_alternative<std::string>(parameters[1].payload) &&
        std::holds_alternative<int32_t>(parameters[2].payload)) {
        return SavedExecutionSupport::Executable;
    }
    if (actionId == 1 && declaredParameterCount == 13 && parameters.size() == 13) {
        return decodeMoveToPoint(*this)
                   ? SavedExecutionSupport::Executable
                   : SavedExecutionSupport::RepresentableButUnsupported;
    }
    if (actionId == 17 && declaredParameterCount == 5 && parameters.size() == 5 &&
        parameters[0].type == static_cast<uint32_t>(SavedActionParameterType::Object) &&
        parameters[1].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        parameters[2].type == static_cast<uint32_t>(SavedActionParameterType::Float) &&
        parameters[3].type == static_cast<uint32_t>(SavedActionParameterType::Float) &&
        parameters[4].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        std::holds_alternative<SavedObjectReference>(parameters[0].payload) &&
        std::holds_alternative<int32_t>(parameters[1].payload) &&
        std::holds_alternative<float>(parameters[2].payload) &&
        std::holds_alternative<float>(parameters[3].payload) &&
        std::holds_alternative<int32_t>(parameters[4].payload)) {
        return SavedExecutionSupport::Executable;
    }
    if (actionId == 6 && declaredParameterCount == 5 && parameters.size() == 5 &&
        parameters[0].type == static_cast<uint32_t>(SavedActionParameterType::Object) &&
        parameters[1].type == static_cast<uint32_t>(SavedActionParameterType::Float) &&
        parameters[2].type == static_cast<uint32_t>(SavedActionParameterType::Float) &&
        parameters[3].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        parameters[4].type == static_cast<uint32_t>(SavedActionParameterType::Integer) &&
        std::holds_alternative<SavedObjectReference>(parameters[0].payload) &&
        std::holds_alternative<float>(parameters[1].payload) &&
        std::holds_alternative<float>(parameters[2].payload) &&
        std::holds_alternative<int32_t>(parameters[3].payload) &&
        std::holds_alternative<int32_t>(parameters[4].payload)) {
        return SavedExecutionSupport::Executable;
    }
    if (actionId == 30 && !parameters.empty() && std::holds_alternative<float>(parameters.front().payload)) {
        return SavedExecutionSupport::Executable;
    }
    if (actionId == 37 && declaredParameterCount == 1 &&
        parameters.size() == 1 &&
        parameters.front().type ==
            static_cast<uint32_t>(SavedActionParameterType::ScriptSituation) &&
        std::holds_alternative<SerializedScriptSituation>(parameters.front().payload)) {
        return SavedExecutionSupport::Executable;
    }
    return SavedExecutionSupport::RepresentableButUnsupported;
}

std::shared_ptr<Action> SavedActionRecord::toRuntimeAction(
    Game &game, const SavedScriptSituationImporter *importer) const {
    if (actionId == 15 && cast && cast->valid()) {
        auto spell = game.getSpell(static_cast<SpellType>(cast->spellId));
        if (!spell) return nullptr;
        if (cast->flags & SavedCastAction::LocationTarget) {
            auto action = game.newAction<CastSpellAtLocationAction>(spell,
                std::make_shared<Location>(cast->position, cast->facing), 0,
                (cast->flags & SavedCastAction::Cheat) != 0,
                static_cast<ProjectilePathType>(cast->path), (cast->flags & SavedCastAction::Instant) != 0);
            action->restoreCastState(*cast); action->attachSavedAction(*this); return action;
        }
        auto target = cast->target.boundObject();
        if (!target) return nullptr;
        std::optional<std::shared_ptr<Item>> item;
        if (cast->flags & SavedCastAction::ItemCast) {
            item = std::dynamic_pointer_cast<Item>(cast->item.boundObject());
            if (!*item && !(cast->flags & SavedCastAction::Released)) return nullptr;
        }
        auto action = game.newAction<CastSpellAtObjectAction>(spell, target, item,
            (cast->flags & SavedCastAction::Cheat) != 0, 0, 0,
            static_cast<ProjectilePathType>(cast->path), (cast->flags & SavedCastAction::Instant) != 0,
            cast->itemProperty >= 0 ? std::optional<size_t>(cast->itemProperty) : std::nullopt,
            cast->itemCasterLevel >= 0 ? std::optional<int>(cast->itemCasterLevel) : std::nullopt);
        action->restoreCastState(*cast); action->attachSavedAction(*this); return action;
    }
    if (executionSupport() != SavedExecutionSupport::Executable) {
        return nullptr;
    }
    if (actionId == 63) {
        auto action = game.newAction<CombatDispatchAction>(std::get<int32_t>(parameters[0].payload));
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 8 || actionId == 11) {
        auto item = std::dynamic_pointer_cast<Item>(
            std::get<SavedObjectReference>(parameters[0].payload).boundObject());
        if (!item) return nullptr;
        const int32_t flags = std::get<int32_t>(parameters[2].payload);
        std::shared_ptr<Action> action;
        if (actionId == 8) {
            action = game.newAction<EquipItemAction>(
                item, *equipmentSlotFromMask(static_cast<uint32_t>(std::get<int32_t>(parameters[1].payload))), flags);
        } else {
            const auto &reference = std::get<SavedObjectReference>(parameters[1].payload);
            auto container = std::dynamic_pointer_cast<Item>(reference.boundObject());
            if (!reference.isInvalid() && !container) return nullptr;
            action = game.newAction<UnequipItemAction>(item, flags, std::move(container));
        }
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 61) {
        auto action = game.newAction<FollowLeaderAction>();
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 30) {
        auto action = game.newAction<WaitAction>(std::get<float>(parameters.front().payload));
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 12) {
        auto decoded = decodePhysicalAttack(*this);
        if (!decoded) {
            return nullptr;
        }
        auto target = decoded->target.boundObject();
        if (!target) {
            return nullptr;
        }
        std::shared_ptr<Action> action;
        if (decoded->feat == FeatType::Invalid) {
            action = game.newAction<AttackObjectAction>(std::move(target));
        } else {
            action = game.newAction<UseFeatAction>(
                decoded->feat, std::move(target));
        }
        action->setCutsceneAttack(decoded->cutscene);
        if (physical) {
            if (auto *attack = dyn_cast<AttackObjectAction>(action.get())) attack->restorePhysicalState(*physical);
            else if (auto *feat = dyn_cast<UseFeatAction>(action.get())) feat->restorePhysicalState(*physical);
        }
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 24) {
        auto target = std::get<SavedObjectReference>(parameters[0].payload).boundObject();
        const auto &dialog = std::get<std::string>(parameters[1].payload);
        int32_t privateConversation = std::get<int32_t>(parameters[2].payload);
        if (!target || dialog.size() > 16 ||
            (privateConversation != 0 && privateConversation != 1)) {
            return nullptr;
        }
        auto action = game.newAction<StartConversationAction>(
            std::move(target), dialog, privateConversation != 0);
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 1) {
        auto decoded = decodeMoveToPoint(*this);
        if (!decoded || !std::dynamic_pointer_cast<Area>(decoded->area.boundObject())) {
            return nullptr;
        }
        if (decoded->target.isInvalid()) {
            MoveToLocationAction::ForcedState state;
            state.areaId = decoded->area.id;
            state.active = decoded->forcedActive;
            state.expiryMilliseconds =
                static_cast<uint64_t>(decoded->expiryDay) *
                    game.millisecondsPerWorldDay() +
                decoded->expiryTime;
            auto location = std::make_shared<Location>(decoded->destination, 0.0f);
            auto action = game.newAction<MoveToLocationAction>(
                std::move(location), decoded->run,
                decoded->forcedPending || decoded->forcedActive,
                decoded->forcedPending ? decoded->timeout : 0.0f, state);
            action->attachSavedAction(*this);
            return action;
        }
        auto target = decoded->target.boundObject();
        if (!target) {
            return nullptr;
        }
        MoveToObjectAction::ForcedState state;
        state.destination = decoded->destination;
        state.areaId = decoded->area.id;
        state.active = decoded->forcedActive;
        state.expiryMilliseconds =
            static_cast<uint64_t>(decoded->expiryDay) *
                game.millisecondsPerWorldDay() +
            decoded->expiryTime;
        auto action = game.newAction<MoveToObjectAction>(
            std::move(target), decoded->run, decoded->range,
            decoded->forcedPending ? decoded->timeout : 0.0f, state,
            decoded->forcedPending || decoded->forcedActive);
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 17) {
        auto target = std::get<SavedObjectReference>(parameters[0].payload).boundObject();
        auto run = std::get<int32_t>(parameters[1].payload);
        auto moveRange = std::get<float>(parameters[2].payload);
        auto useRadius = std::get<float>(parameters[3].payload);
        auto moveFlags = std::get<int32_t>(parameters[4].payload);
        if (!target || (run != 0 && run != 1) ||
            !std::isfinite(moveRange) || moveRange < 0.0f ||
            !std::isfinite(useRadius) || useRadius != moveRange ||
            moveFlags != 1) {
            return nullptr;
        }
        auto action = game.newAction<MoveToObjectAction>(
            std::move(target), run != 0, moveRange);
        action->attachSavedAction(*this);
        return action;
    }
    if (actionId == 6) {
        auto animation = std::get<SavedObjectReference>(parameters[0].payload).id;
        auto speed = std::get<float>(parameters[1].payload);
        auto duration = std::get<float>(parameters[2].payload);
        auto start = std::get<int32_t>(parameters[3].payload);
        auto looping = std::get<int32_t>(parameters[4].payload);
        bool validAnimation = animation <= 41 ||
                              (animation >= 43 && animation <= 46) ||
                              (animation >= 100 && animation <= 110) ||
                              (animation >= 112 && animation <= 124) ||
                              (animation >= 200 && animation <= 213);
        if (!validAnimation ||
            !std::isfinite(speed) || !std::isfinite(duration) ||
            (start != 0 && start != 1) || (looping != 0 && looping != 1)) {
            return nullptr;
        }
        auto action = game.newAction<PlayAnimationAction>(
            static_cast<AnimationType>(animation), speed, duration,
            start == 0, looping != 0);
        action->attachSavedAction(*this);
        return action;
    }
    if (!importer) {
        return nullptr;
    }
    auto result = importer->import(
        std::get<SerializedScriptSituation>(parameters.front().payload));
    if (!result) {
        return nullptr;
    }
    auto action = game.newAction<SavedDoCommandAction>(std::move(result.continuation));
    action->attachSavedAction(*this);
    return action;
}

bool SavedActionRecord::bindObjectReferences(const Game &game) {
    bool allBound = true;
    if (cast) {
        if (!(cast->flags & SavedCastAction::LocationTarget))
            allBound = game.bindSavedObjectReference(cast->target) && allBound;
        if (cast->flags & SavedCastAction::ItemCast) {
            bool itemBound = game.bindSavedObjectReference(cast->item);
            if (!(cast->flags & SavedCastAction::Released)) allBound = itemBound && allBound;
        }
    }
    if (round) {
        game.bindSavedObjectReference(round->pauseOwner); game.bindSavedObjectReference(round->master);
        game.bindSavedObjectReference(round->engaged);
    }
    if (physical) {
        for (auto &source : physical->sources) if (!source.isInvalid()) game.bindSavedObjectReference(source);
        for (auto &effect : physical->secondaryEffects) if (effect.serializedType) game.bindEffectCreator(effect);
        for (auto &history : physical->histories) {
            game.bindSavedObjectReference(history.reactionObject); game.bindSavedObjectReference(history.ammoItem);
        }
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        auto &parameter = parameters[index];
        // ActionId 6 reuses the serialized type-3 storage slot for an animation
        // identifier rather than an object identity.
        if (actionId == 6 && index == 0) continue;
        allBound = parameter.bindObjectReferences(game) && allBound;
    }
    return allBound;
}

SavedActionQueue SavedActionQueue::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext,
    const std::string &label) {
    SavedActionQueue result;
    for (const auto &action : gff.getList(label)) {
        result.actions.push_back(SavedActionRecord::fromGff(
            *action, identityContext));
    }
    return result;
}

void SavedCombatAttack::writeFields(resource::Gff &record) const {
    using Field = resource::Gff::Field;
    auto put = [](resource::Gff &owner, Field field) {
        auto &destination = owner.fields();
        auto found = std::find_if(destination.begin(), destination.end(),
            [&](const auto &existing) { return existing.label == field.label; });
        if (found == destination.end()) destination.push_back(std::move(field));
        else *found = std::move(field);
    };
    const AttackEventFields cleared;
    const auto &value = fields ? *fields : cleared;
    const AttackHistory emptyHistory;
    const auto &header = history ? *history : emptyHistory;
    put(record, Field::newByte("AttackGroup", value.group));
    put(record, Field::newWord("AnimationLength", value.animationLength));
    put(record, Field::newDword("MissedBy", value.missedBy));
    put(record, Field::newByte("AttackResult", value.result));
    put(record, Field::newDword("ReactObject", reactionObject.id));
    put(record, Field::newWord("ReaxnDelay", value.reactionDelay));
    put(record, Field::newWord("ReaxnAnimation", value.reactionAnimation));
    put(record, Field::newWord("ReaxnAnimLength", value.reactionAnimationLength));
    put(record, Field::newByte("Concealment", value.concealment));
    put(record, Field::newWord("AttackType", header.type));
    put(record, Field::newByte("AttackMode", header.mode));
    put(record, Field::newInt("RangedAttack", value.ranged));
    put(record, Field::newInt("SneakAttack", value.sneakAttack));
    put(record, Field::newByte("WeaponAttackType", value.weaponAttackType));
    put(record, Field::newFloat("RangedTargetX", value.rangedTarget[0]));
    put(record, Field::newFloat("RangedTargetY", value.rangedTarget[1]));
    put(record, Field::newFloat("RangedTargetZ", value.rangedTarget[2]));
    std::vector<std::shared_ptr<resource::Gff>> damage;
    const auto &previous = record.getList("DamageList");
    damage.reserve(value.damage.size());
    for (size_t i = 0; i < value.damage.size(); ++i) {
        // Preserve unknown child fields without mutating a shared input shadow.
        auto entry = i < previous.size() && previous[i]
            ? std::make_shared<resource::Gff>(0xdaee, previous[i]->fields())
            : std::make_shared<resource::Gff>(0xdaee, std::vector<Field> {});
        entry->setType(0xdaee);
        put(*entry, Field::newShort("DamageValue", static_cast<int16_t>(value.damage[i])));
        damage.push_back(std::move(entry));
    }
    put(record, Field::newList("DamageList", std::move(damage)));
    put(record, Field::newByte("KillingBlow", value.killingBlow));
    put(record, Field::newByte("CoupDeGrace", value.coupDeGrace));
    put(record, Field::newByte("CriticalThreat", value.criticalThreat));
    put(record, Field::newByte("AttackDeflected", value.deflected));
    put(record, Field::newDword("AmmoItem", ammoItem.id));
    put(record, Field::newCExoString("AttackDebugText", value.attackDebugText));
    put(record, Field::newCExoString("DamageDebugText", value.damageDebugText));
}

SavedEventRecord SavedEventRecord::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    SavedEventRecord result;
    result.day = gff.getUint("Day");
    result.time = gff.getUint("Time");
    result.object = SavedObjectReference::fromSerializedId(
        gff.getUint("ObjectId"), identityContext);
    result.caller = SavedObjectReference::fromSerializedId(
        gff.getUint("CallerId"), identityContext);
    result.eventId = gff.getUint("EventId");
    result.unsupportedFields = collectUnsupportedFields(
        gff,
        {"Day", "Time", "ObjectId", "CallerId", "EventId", "EventData"});

    auto data = gff.findStruct("EventData");
    if (!data) {
        result.payload = std::monostate {};
        return result;
    }
    switch (static_cast<SavedEventType>(result.eventId)) {
    case SavedEventType::Timed:
        result.payload = SerializedScriptSituation::fromGff(
            *data, identityContext);
        break;
    case SavedEventType::RemoveFromArea:
        result.payload = SavedBytePayload {static_cast<uint8_t>(data->getUint("Value"))};
        break;
    case SavedEventType::ApplyEffect:
    case SavedEventType::RemoveEffect:
        result.payload = EffectInstance::fromGff(*data, identityContext);
        break;
    case SavedEventType::SpellImpact:
    case SavedEventType::ItemOnHitSpellImpact:
        if (result.eventId == static_cast<uint32_t>(SavedEventType::ItemOnHitSpellImpact) &&
            data->getUint("ReoneWeaponHit") == 1)
            result.payload = SavedWeaponImpact::fromGff(*data, identityContext);
        else result.payload = savedSpellImpactFromGff(*data, identityContext);
        break;
    case SavedEventType::PlayAnimation:
    case SavedEventType::ControllerRumble:
        result.payload = SavedIntPayload {data->getInt("Value")};
        break;
    case SavedEventType::SignalEvent:
    case SavedEventType::SummonCreature:
    case SavedEventType::AreaTransition:
        result.payload = savedScriptEventFromGff(*data, identityContext);
        break;
    case SavedEventType::SpawnBodyBag:
        result.payload = savedBodyBagFromGff(*data, identityContext);
        break;
    case SavedEventType::OnMeleeAttacked:
    case SavedEventType::BroadcastSafeProjectile:
        result.payload = savedCombatAttackFromGff(*data, identityContext);
        break;
    case SavedEventType::BroadcastAoo:
        result.payload = SavedBroadcastAoo {
            SavedObjectReference::fromSerializedId(
                data->getUint("Value"), identityContext)};
        break;
    case SavedEventType::FeedbackMessage:
        result.payload = savedFeedbackMessageFromGff(
            *data, identityContext);
        break;
    default:
        result.payload = unsupportedPayload(*data);
        break;
    }
    return result;
}

SavedExecutionSupport SavedEventRecord::executionSupport() const {
    if (eventId == static_cast<uint32_t>(SavedEventType::ItemOnHitSpellImpact) &&
        std::holds_alternative<SavedWeaponImpact>(payload)) return SavedExecutionSupport::Executable;
    if ((eventId == static_cast<uint32_t>(SavedEventType::SpellImpact) ||
         eventId == static_cast<uint32_t>(SavedEventType::ItemOnHitSpellImpact)) &&
        std::holds_alternative<SavedSpellImpact>(payload)) return SavedExecutionSupport::Executable;
    if (eventId == static_cast<uint32_t>(SavedEventType::BroadcastSafeProjectile)) {
        const auto *attack = std::get_if<SavedCombatAttack>(&payload);
        if (attack && attack->fields && attack->fields->result >= 1 && attack->fields->result <= 10)
            return SavedExecutionSupport::Executable;
        return SavedExecutionSupport::RepresentableButUnsupported;
    }
    if (eventId == static_cast<uint32_t>(SavedEventType::OnMeleeAttacked) &&
        (std::holds_alternative<SavedCombatAttack>(payload) ||
         std::holds_alternative<std::monostate>(payload))) return SavedExecutionSupport::Executable;
    if (eventId == static_cast<uint32_t>(SavedEventType::SignalEvent)) {
        const auto *event = std::get_if<SavedScriptEvent>(&payload);
        if (event && (event->type == 2 || event->type == 11)) return SavedExecutionSupport::Executable;
    }
    if (eventId == static_cast<uint32_t>(SavedEventType::DestroyObject) &&
        std::holds_alternative<std::monostate>(payload)) return SavedExecutionSupport::Executable;
    if (eventId == static_cast<uint32_t>(SavedEventType::ForcedAction)) {
        return SavedExecutionSupport::Discarded;
    }
    if (eventId == static_cast<uint32_t>(SavedEventType::Timed) &&
        std::holds_alternative<SerializedScriptSituation>(payload)) {
        return SavedExecutionSupport::Executable;
    }
    if ((eventId == static_cast<uint32_t>(SavedEventType::ApplyEffect) ||
         eventId == static_cast<uint32_t>(SavedEventType::RemoveEffect)) &&
        std::holds_alternative<EffectInstance>(payload)) {
        return SavedExecutionSupport::Executable;
    }
    return SavedExecutionSupport::RepresentableButUnsupported;
}

bool SavedEventRecord::shouldRestore() const {
    return eventId > 0 &&
           eventId <= static_cast<uint32_t>(SavedEventType::ControllerRumble) &&
           eventId != static_cast<uint32_t>(SavedEventType::ForcedAction);
}

bool SavedEventRecord::bindObjectReferences(const Game &game) {
    bool allBound = true;
    bindReference(game, object, allBound);
    bindReference(game, caller, allBound);
    if (auto situation = std::get_if<SerializedScriptSituation>(&payload)) {
        allBound = situation->bindObjectReferences(game) && allBound;
    } else if (auto effect = std::get_if<EffectInstance>(&payload)) {
        allBound = game.bindEffectCreator(*effect) && allBound;
    } else if (auto hit = std::get_if<SavedWeaponImpact>(&payload)) {
        bindReference(game, hit->source, allBound);
        allBound = game.bindEffectCreator(hit->damage) && allBound;
    } else if (auto spell = std::get_if<SavedSpellImpact>(&payload)) {
        bindReference(game, spell->caster, allBound);
        bindReference(game, spell->target, allBound);
        bindReference(game, spell->area, allBound);
        bindReference(game, spell->item, allBound);
    } else if (auto event = std::get_if<SavedScriptEvent>(&payload)) {
        for (auto &reference : event->objects) {
            bindReference(game, reference, allBound);
        }
    } else if (auto bodyBag = std::get_if<SavedBodyBag>(&payload)) {
        bindReference(game, bodyBag->object, allBound);
    } else if (auto broadcast = std::get_if<SavedBroadcastAoo>(&payload)) {
        bindReference(game, broadcast->target, allBound);
    } else if (auto combat = std::get_if<SavedCombatAttack>(&payload)) {
        bindReference(game, combat->reactionObject, allBound);
        bindReference(game, combat->ammoItem, allBound);
    } else if (auto feedback = std::get_if<SavedFeedbackMessage>(&payload)) {
        for (auto &reference : feedback->objects) {
            bindReference(game, reference, allBound);
        }
    }
    return allBound;
}

SavedEventQueue SavedEventQueue::fromGff(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext,
    const std::string &label) {
    SavedEventQueue result;
    for (const auto &event : gff.getList(label)) {
        result.events.push_back(SavedEventRecord::fromGff(
            *event, identityContext));
    }
    return result;
}

} // namespace game

} // namespace reone
