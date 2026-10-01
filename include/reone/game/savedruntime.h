/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "reone/resource/gff.h"
#include "reone/script/types.h"

#include "effect.h"
#include "effect/damage.h"
#include "attack.h"
#include "runtimeref.h"

namespace reone {

namespace game {

class Action;
class Game;
class Object;
class SavedScriptSituationImporter;

constexpr uint32_t kSavedRuntimeInvalidObjectId = 0x7f000000;
// Saved identities above the invalid ID form the character namespace. Player
// characters take them downward from this ceiling; ordinary objects stay below
// the invalid ID.
constexpr uint32_t kSavedRuntimeMaxCharacterObjectId = 0x7fffffff;
// The game creates the structural Module before loading its saved object graph.
// Its contextual object-reference target is therefore slot 0 even though the
// IFO contains no owned Module record or explicit Module ObjectId field.
constexpr uint32_t kSavedRuntimeModuleObjectId = 0;

/** A saved object identity which is deliberately unbound while B is built. */
struct SavedObjectReference {
    uint32_t id {kSavedRuntimeInvalidObjectId};

    SavedObjectReference() = default;
    explicit SavedObjectReference(uint32_t id) : id(id) {}
    /** The runtime's invalid object is the saved invalid object. */
    static SavedObjectReference fromRuntimeId(uint32_t id) {
        return id == script::kObjectInvalid ? SavedObjectReference() : SavedObjectReference(id);
    }
    static SavedObjectReference fromSerializedId(
        uint32_t id,
        SerializedIdentityContext identityContext) {
        SavedObjectReference result(id);
        result._serializedIdentityContext = std::move(identityContext);
        return result;
    }
    /**
     * An object slot holding a value that names no object. It binds to
     * nothing and is saved as the value itself.
     */
    static SavedObjectReference rawValue(uint32_t value) {
        SavedObjectReference result(value);
        result._rawValue = true;
        return result;
    }

    bool isInvalid() const { return id == kSavedRuntimeInvalidObjectId; }
    bool isRawValue() const { return _rawValue; }
    bool isSerializedIdentity() const {
        return _serializedIdentityContext.has_value();
    }
    const std::optional<SerializedIdentityContext> &serializedIdentityContext() const {
        return _serializedIdentityContext;
    }
    std::shared_ptr<Object> boundObject() const;

private:
    friend class Game;
    RuntimeObjectRef<Object> _object;
    std::optional<uint64_t> _runtimeSession;
    std::optional<uint64_t> _savedGraph;
    std::optional<SerializedIdentityContext> _serializedIdentityContext;
    bool _rawValue {false};
};

struct SavedLocString {
    int32_t strRef {0};
    std::string text;
};

struct SavedStruct;
using SavedStructChildren = std::vector<std::shared_ptr<SavedStruct>>;
using SavedFieldValue = std::variant<
    int64_t,
    uint64_t,
    double,
    std::string,
    ByteBuffer,
    glm::vec3,
    glm::quat,
    SavedLocString,
    SavedStructChildren>;

/**
 * Semantic GFF field fallback for a genuinely unsupported typed payload.
 * Known action/event/VM payloads use their concrete models below.
 */
struct SavedField {
    resource::Gff::FieldType type {resource::Gff::FieldType::Int};
    std::string label;
    SavedFieldValue value {int64_t {0}};
};

struct SavedStruct {
    uint32_t type {0};
    std::vector<SavedField> fields;

    static SavedStruct fromGff(const resource::Gff &gff);
};

struct UnsupportedSavedPayload {
    /**
     * Opaque compatibility shadow only. It is never executed, and an arbitrary
     * DWORD in it is never taken for an ObjectId. Consequently, a modded
     * unknown payload that embeds an undocumented object reference can only be
     * preserved numerically; it is not promised to survive graph renumbering.
     * Once a structure is known to carry references it must receive a typed
     * model and symmetric translation instead of remaining here.
     */
    SavedStruct data;
};

struct SavedLocationValue {
    glm::vec3 position {0.0f};
    glm::vec3 orientation {0.0f};
};

struct SavedScriptEvent {
    uint16_t type {0};
    std::vector<int32_t> integers;
    std::vector<float> floats;
    std::vector<std::string> strings;
    std::vector<SavedObjectReference> objects;
};

/** Game-defined talent value used by VM structure 3. */
struct SavedTalentValue {
    int32_t id {-1};
    int32_t type {-1};
    uint8_t multiClass {0};
    SavedObjectReference item;
    int32_t itemPropertyIndex {-1};
    uint8_t casterLevel {0xff};
    uint8_t metaType {0xff};
};

enum class SavedVmStackType : int8_t {
    Integer = 3,
    Float = 4,
    String = 5,
    Object = 6,
    Effect = 16,
    Event = 17,
    Location = 18,
    Talent = 19,
};

using SavedVmStackPayload = std::variant<
    UnsupportedSavedPayload,
    int32_t,
    float,
    std::string,
    SavedObjectReference,
    EffectInstance,
    SavedScriptEvent,
    SavedLocationValue,
    SavedTalentValue>;

struct SavedVmStackValue {
    int8_t type {0};
    SavedVmStackPayload payload {UnsupportedSavedPayload {}};
};

enum class ScriptSituationResumeSupport {
    UnsupportedSavedSnapshot,
    ValidatedImport,
};

/** The game STORE_STATE continuation, kept separate from live ExecutionState. */
struct SerializedScriptSituation {
    int32_t codeSize {0};
    ByteBuffer code;
    uint32_t crc {0};
    int32_t instructionPointer {0};
    int32_t secondaryPointer {0};
    std::string scriptName;
    int32_t stackSize {0};
    int32_t basePointer {0};
    int32_t stackPointer {0};
    int32_t totalSize {0};
    std::vector<SavedVmStackValue> stack;
    std::vector<SavedField> unsupportedFields;

    static SerializedScriptSituation fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    ScriptSituationResumeSupport resumeSupport() const {
        return ScriptSituationResumeSupport::ValidatedImport;
    }
    bool bindObjectReferences(const Game &game);
    bool isBoundToCurrentRuntimeSession(const Game &game) const;

private:
    friend class SavedScriptSituationImporter;
    std::optional<uint64_t> _runtimeSession;
};

enum class SavedActionParameterType : uint32_t {
    Unsupported = 0,
    Integer = 1,
    Float = 2,
    Object = 3,
    String = 4,
    ScriptSituation = 5,
};

using SavedActionParameterPayload = std::variant<
    UnsupportedSavedPayload,
    int32_t,
    float,
    SavedObjectReference,
    std::string,
    SerializedScriptSituation>;

struct SavedActionParameter {
    uint32_t type {0};
    SavedActionParameterPayload payload {UnsupportedSavedPayload {}};

    static SavedActionParameter fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    bool bindObjectReferences(const Game &game);
};

enum class SavedExecutionSupport {
    Executable,
    RepresentableButUnsupported,
    Discarded,
};

/** In-progress cast state, separate from the saved queue parameters. */
struct SavedCastAction {
    int spellId {-1};
    SavedObjectReference target;
    SavedObjectReference item;
    glm::vec3 position {0.0f};
    float facing {0.0f};
    int itemProperty {-1};
    int itemCasterLevel {-1};
    int casterLevel {0};
    int metaMagic {255};
    int castingClass {kUnselectedCastingClass};
    int associatedFeat {-1};
    int selectedClass {-1};
    int selectedLevel {kUnspecifiedCasterLevel};
    int forceCost {0};
    int phase {0};
    float elapsed {0.0f};
    float conjureTime {0.0f};
    float castTime {0.0f};
    float catchTime {0.0f};
    float projectileTime {0.0f};
    uint64_t presentationId {0};
    uint32_t flags {0};
    uint8_t path {0};
    enum Flag : uint32_t {
        LocationTarget = 1, ItemCast = 2, Cheat = 4, Instant = 8,
        Started = 16, CommitAttempted = 32, Committed = 64,
        Released = 128, MovementOwned = 256, Fake = 1024
    };
    bool valid() const;
    static SavedCastAction fromGff(const resource::Gff &, const SerializedIdentityContext &);
    std::shared_ptr<resource::Gff> toGff() const;
};

struct SavedRoundClock {
    uint64_t id {0};
    int slot {0};
    int state {0};
    float elapsed {0.0f};
    float duration {3.0f};
    float pauseRemaining {0.0f};
    SavedObjectReference pauseOwner;
    SavedObjectReference master;
    SavedObjectReference engaged;
    // The command took over the round from an earlier, finished command.
    bool joined {false};
    static SavedRoundClock fromGff(const resource::Gff &, const SerializedIdentityContext &);
    std::shared_ptr<resource::Gff> toGff() const;
};

/** Presentation-only state. Gameplay hits are saved by the existing event queue. */
struct SavedProjectile {
    struct Leg {
        SavedObjectReference source;
        SavedObjectReference target;
        glm::vec3 origin {0.0f};
        glm::vec3 destination {0.0f};
        float duration {0.0f};
        int motion {1}; // homing, ballistic, accelerating, spiral, linked, burst
        glm::vec3 targetOffset {0.0f};
        std::string targetHook;
        bool ownsTargetHook {false};
        float stopRadius {0.0f};
        int surface {-1}; // a grenade leg's ground material at its destination
    };
    uint64_t id {0};
    int kind {0}; // 0: spell; 1: saber route; 2: combat broadcast
    SavedObjectReference caster;
    SavedObjectReference weapon;
    int spellId {-1};
    int path {0};
    std::string model;
    std::vector<Leg> legs;
    int leg {0};
    bool released {false};
    bool clockwise {false};
    float elapsed {0.0f};
    glm::vec3 position {0.0f};
    glm::vec3 velocity {0.0f};
    glm::vec3 acceleration {0.0f};
    float activationDelay {0.0f};
    float travelRate {0.0f};
    std::string sourceHook;
    std::string targetHook {"impact"};
    int combatResult {0};
    int soundVariant {0};
    int orientationMode {0};
    glm::quat orientation {1.0f, 0.0f, 0.0f, 0.0f};
    static SavedProjectile fromGff(const resource::Gff &, const SerializedIdentityContext &);
    std::shared_ptr<resource::Gff> toGff() const;
    bool bindObjectReferences(const Game &);
};

// Item on-hit work of an attack. An attack's saved continuation also keeps
// the feedback its round has not yet released here.
struct SavedWeaponImpact {
    SavedObjectReference source;
    std::vector<ItemOnHitApplication> applications;
    std::vector<DeferredCombatFeedback> feedback;
    static SavedWeaponImpact fromGff(const resource::Gff &, const SerializedIdentityContext &);
    std::shared_ptr<resource::Gff> toGff() const;
};

/**
 * An attack record's 25 saved fields. The saved structure is kept whole, so
 * fields the game does not use are written back as they were read.
 */
struct SavedCombatAttack {
    SavedStruct data;
    // Live event nodes borrow the attack record; loaded nodes own a new record.
    std::shared_ptr<AttackHistory> history {std::make_shared<AttackHistory>()};
    std::shared_ptr<AttackEventFields> fields {std::make_shared<AttackEventFields>()};
    SavedObjectReference reactionObject;
    SavedObjectReference ammoItem;

    /** Overlay the represented fields onto the preserved attack-data structure. */
    void writeFields(resource::Gff &record) const;
};


struct SavedPhysicalAction {
    std::shared_ptr<resource::Gff> state;
    std::vector<SavedObjectReference> sources;
    std::vector<std::vector<EffectInstance>> targetEffects;
    std::vector<SavedCombatAttack> histories;
    // A ranged round's shared attack record.
    std::optional<SavedCombatAttack> roundRecord;
    static SavedPhysicalAction fromGff(const resource::Gff &, const SerializedIdentityContext &);
    std::shared_ptr<resource::Gff> toGff() const;
    bool valid() const;
};

struct SavedActionRecord {
    bool scheduled {false};
    uint32_t actionId {0};
    uint16_t groupActionId {0};
    uint16_t declaredParameterCount {0};
    std::vector<SavedActionParameter> parameters;
    std::optional<SavedCastAction> cast;
    std::optional<SavedRoundClock> round;
    std::optional<SavedPhysicalAction> physical;
    std::vector<SavedField> unsupportedFields;

    static SavedActionRecord fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    SavedExecutionSupport executionSupport() const;
    std::shared_ptr<Action> toRuntimeAction(
        Game &game,
        const SavedScriptSituationImporter *importer = nullptr) const;
    bool bindObjectReferences(const Game &game);
};

/** A scheduled round operation, distinct from the ordinary command list. */
struct SavedScheduledAction {
    int32_t timer {0};
    uint16_t animation {0};
    int32_t animationTime {1500};
    int32_t numAttacks {0};
    uint8_t type {0};
    SavedObjectReference target;
    uint8_t retargettable {1};
    uint32_t inventorySlot {0};
    SavedObjectReference repository;
    std::optional<SavedActionRecord> command;
    bool applied {false};
    float remainingPause {0.0f};
    std::vector<SavedField> unsupportedFields;

    static SavedScheduledAction fromGff(const resource::Gff &, const SerializedIdentityContext &);
    bool bindObjectReferences(const Game &game);
    bool isEquipment() const { return type == 6 || type == 7; }
};

/** Parsed per-object queue; publication waits until B object registration. */
struct SavedActionQueue {
    std::vector<SavedActionRecord> actions;

    static SavedActionQueue fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext,
        const std::string &label = "ActionList");
};

enum class SavedEventType : uint32_t {
    Timed = 1,
    EnteredTrigger = 2,
    LeftTrigger = 3,
    RemoveFromArea = 4,
    ApplyEffect = 5,
    CloseObject = 6,
    OpenObject = 7,
    SpellImpact = 8,
    PlayAnimation = 9,
    SignalEvent = 10,
    DestroyObject = 11,
    Unlock = 12,
    Lock = 13,
    RemoveEffect = 14,
    OnMeleeAttacked = 15,
    DecrementStackSize = 16,
    SpawnBodyBag = 17,
    ForcedAction = 18,
    ItemOnHitSpellImpact = 19,
    BroadcastAoo = 20,
    BroadcastSafeProjectile = 21,
    FeedbackMessage = 22,
    AbilityEffectApplied = 23,
    SummonCreature = 24,
    AcquireItem = 25,
    AreaTransition = 26,
    ControllerRumble = 27,
};

struct SavedBytePayload {
    uint8_t value {0};
};
struct SavedIntPayload {
    int32_t value {0};
};
struct SavedSpellImpact {
    int32_t spellId {0};
    SavedObjectReference caster;
    SavedObjectReference target;
    SavedObjectReference area;
    SavedObjectReference item;
    std::string script;
    glm::vec3 targetPosition {0.0f};
    int32_t finalForceCost {0};
    struct CapturedContext {
        int32_t casterLevel {0};
        int32_t metaMagic {255};
        float targetFacing {0.0f};
        int32_t castingClass {kUnselectedCastingClass};
        int32_t itemCasterLevel {0};
        std::optional<int> levelOverride;
    };
    // Captured events carry these values. Uncaptured events use the owner context;
    // absence is distinct from an incomplete captured context.
    std::optional<CapturedContext> capturedContext;
    std::shared_ptr<resource::Gff> toGff() const;
};


struct SavedBodyBag {
    SavedObjectReference object;
    glm::vec3 position {0.0f};
};

/**
 * An open-object event raised during play: the caller opens the target's
 * inventory. The event is saved without data, so a loaded one opens nothing.
 */
struct SavedOpenObject {};

/** The game EVENT_BROADCAST_AOO stores an ObjectId in its generic DWORD Value. */
struct SavedBroadcastAoo {
    SavedObjectReference target;
};

/** The type of a feedback-log message that reports a saving throw. */
constexpr uint32_t kSavingThrowFeedbackMessageType = 1;
/** A saving-throw message names its saver by its seventh integer. */
constexpr size_t kFeedbackSaverIndex = 6;

/**
 * A queued feedback-log message: its type, integer, float, object and
 * string lists in data, with the object list's references alongside.
 */
struct SavedFeedbackMessage {
    SavedStruct data;
    std::vector<SavedObjectReference> objects;
    /** The saving object a saving-throw message names by its last integer. */
    SavedObjectReference saver;
};

using SavedEventPayload = std::variant<
    std::monostate,
    UnsupportedSavedPayload,
    SerializedScriptSituation,
    EffectInstance,
    SavedBytePayload,
    SavedIntPayload,
    SavedSpellImpact,
    SavedWeaponImpact,
    SavedScriptEvent,
    SavedBodyBag,
    SavedOpenObject,
    SavedBroadcastAoo,
    SavedCombatAttack,
    SavedFeedbackMessage>;

struct SavedEventRecord {
    /** Absolute world/game timestamp, never a wall-clock or derived delay. */
    uint32_t day {0};
    uint32_t time {0};
    SavedObjectReference object;
    SavedObjectReference caller;
    uint32_t eventId {0};
    SavedEventPayload payload;
    std::vector<SavedField> unsupportedFields;

    static SavedEventRecord fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);
    SavedExecutionSupport executionSupport() const;
    bool shouldRestore() const;
    bool bindObjectReferences(const Game &game);
};

/** Parsed module/server queue; list order is the authoritative queue order. */
struct SavedEventQueue {
    std::vector<SavedEventRecord> events;

    static SavedEventQueue fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext,
        const std::string &label = "EventQueue");
};

} // namespace game

} // namespace reone
