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

#pragma once

#include <array>
#include <limits>
#include <optional>
#include <set>
#include <vector>

#include "reone/script/enginetype.h"

#include "saveprovenance.h"
#include "runtimeref.h"
#include "types.h"

namespace reone {

namespace resource {
class Gff;
}

namespace script {
struct Variable;
}

namespace game {

class Object;
class Creature;
class Game;

using EffectId = uint64_t;

constexpr EffectId kUnassignedEffectId = 0;
constexpr uint32_t kSavedEffectInvalidObjectId = 0x7f000000;

enum class EffectIdImportResult {
    Unassigned,
    Imported,
    Existing,
};

enum class EffectExpiryOrigin {
    None,
    LoadedAbsoluteGameTime,
    RuntimeCountdown,
    RuntimeAbsoluteGameTime,
};

/** Runtime-session owner for the global exposed-effect identity namespace. */
class EffectIdNamespace {
public:
    static constexpr EffectId kFirstId = 1;

    EffectId allocate();
    EffectIdImportResult importId(EffectId id);
    bool setNextId(EffectId id);
    void reset();

    EffectId nextId() const { return _nextId; }
    bool contains(EffectId id) const { return _ids.count(id) != 0; }
    size_t size() const { return _ids.size(); }

private:
    EffectId _nextId {kFirstId};
    std::set<EffectId> _ids;
};

/** Result of the single application callback, before collection admission. */
enum class EffectApplicationResult {
    Rejected, // No accepted operation and no retained record.
    Applied,  // Operation completed; do not retain or apply it again.
    Retained, // Publish the record; derived modifiers may read it thereafter.
};

enum class EffectRemovalResult { Retained, Removed };

// Selectors for the script commands, not names for internal erasers.
enum class ScriptEffectRemovalMatch { PackageId, Integer0, TypeAndFirstTwoIntegers };

struct EffectPackageApplicationResult {
    size_t accepted {0};
    size_t rejected {0};
    bool partial() const { return accepted != 0 && rejected != 0; }
};

struct EffectInstance;

class Effect : public script::CopyableEngineType {
public:
    Effect(EffectType type) :
        _type(type) {
    }

    Effect(const Effect &) = default;
    std::shared_ptr<script::EngineType> cloneForScript() const override;
    uint64_t scriptValueId() const override { return _saveFacingId; }
    std::shared_ptr<Effect> cloneEffect() const {
        return std::static_pointer_cast<Effect>(cloneForScript());
    }
    void setSaveFacingId(EffectId id) { _saveFacingId = id; }

    /** Apply once; distinguish rejection, a completed operation, and retention. */
    virtual EffectApplicationResult onApply(Object &object, EffectInstance &instance);
    virtual EffectRemovalResult onRemove(Object &object, const EffectInstance &instance);
    virtual void onUpdate(Object &object, const EffectInstance &instance, float dt);
    /** Release executable payload owned by the outgoing Area lifetime. */
    virtual void retireAreaRuntime(
        const std::set<const Object *> &retainedObjects) {}

    /**
     * Build the serialized effect value carried by a live VM continuation.
     *
     * Runtime effects retain executable C++ behavior, while this description
     * carries the save-facing fields and object bindings. Subclasses configure
     * their parameters through the protected helpers.
     */
    virtual EffectInstance saveFacingInstance() const;
    void setSaveFacingCreator(const std::shared_ptr<Object> &creator);
    void setSaveFacingSpellId(int32_t spellId);
    virtual void setSubType(uint16_t category);
    virtual bool setVersusAlignment(int lawChaos, int goodEvil);
    virtual bool setVersusRacialType(int racialType);
    void captureSaveFacingScriptArguments(
        const std::vector<script::Variable> &arguments,
        const Game &game);

    EffectType type() const { return _type; }

protected:
    void setSaveFacingInteger(size_t index, int32_t value);
    void setSaveFacingFloat(size_t index, float value);
    void setSaveFacingString(size_t index, std::string value);
    void setSaveFacingObject(
        size_t index,
        const std::shared_ptr<Object> &object);

    EffectType _type;

private:
    EffectId _saveFacingId {kUnassignedEffectId};
    uint16_t _saveFacingSubType {0x8};
    std::vector<int32_t> _saveFacingIntegers;
    std::array<float, 4> _saveFacingFloats {};
    std::array<std::string, 6> _saveFacingStrings {};
    RuntimeObjectRef<Object> _saveFacingCreator;
    std::array<RuntimeObjectRef<Object>, 4> _saveFacingObjects;
    uint32_t _saveFacingSpellId {std::numeric_limits<uint32_t>::max()};
    bool _saveFacingRepresentable {true};
};

/** Preserve each concrete executable class when copying a live VM effect. */
template <typename T>
class CopyableEffect : public Effect {
public:
    using Effect::Effect;
    std::shared_ptr<script::EngineType> cloneForScript() const override {
        return std::make_shared<T>(static_cast<const T &>(*this));
    }
};

/**
 * Semantic state of one applied effect.
 *
 * The executable Effect is optional: saved records whose behavior Reone does
 * not implement still remain typed, queryable and removable without losing
 * their saved identity or payload.
 */
struct EffectInstance {
    std::shared_ptr<Effect> effect;
    EffectId id {kUnassignedEffectId};
    // Object-local observation order; independent of saved/package identity.
    uint64_t applicationOrder {0};
    // Dispatch context only; never serialized as an extra saved field.
    bool restoring {false};
    uint16_t serializedType {0};
    uint16_t subType {0};
    float duration {0.0f};
    /**
     * Derived observation of the canonical expiry day/time. Pending values use
     * RuntimeCountdown until admission assigns their absolute world deadline.
     * Saved Duration remains the authored (possibly handler-normalized) value.
     */
    std::optional<float> remainingDuration;
    uint32_t expiryDay {0};
    uint32_t expiryTime {0};
    EffectExpiryOrigin expiryOrigin {EffectExpiryOrigin::None};
    uint32_t creatorId {0};
    uint32_t spellId {std::numeric_limits<uint32_t>::max()};
    int32_t exposed {0};
    bool skipOnLoad {false};
    std::vector<int32_t> integerParameters;
    std::array<float, 4> floatParameters {};
    std::array<std::string, 6> stringParameters {};
    std::array<uint32_t, 4> objectParameters {
        kSavedEffectInvalidObjectId,
        kSavedEffectInvalidObjectId,
        kSavedEffectInvalidObjectId,
        kSavedEffectInvalidObjectId,
    };
    RuntimeObjectRef<Object> creator;
    std::array<RuntimeObjectRef<Object>, 4> objectParameterObjects;
    std::optional<SerializedIdentityContext> serializedReferenceContext;

    static EffectInstance fromGff(
        const resource::Gff &gff,
        const SerializedIdentityContext &identityContext);

    DurationType durationType() const;
    void setDuration(DurationType type, float seconds);
    void setIntegerParameter(size_t index, int32_t value);
    void materialize();
    std::shared_ptr<resource::Gff> toGff() const;
    /** Descendant reconstructed by a retained root, not an independent VM link. */
    void markGeneratedForLoad() { exposed = 0; skipOnLoad = true; }
    /** New executable payload under this existing package and bound source. */
    EffectInstance linkedChild(const std::shared_ptr<Effect> &child) const;
    EffectType type() const;
    int32_t integerParameter(size_t index, int32_t defaultValue = 0) const;
    uint16_t semanticSubType() const { return subType & 0x18; }
    bool hasStableId() const { return id != kUnassignedEffectId; }
    bool hasSerializedObjectReferences() const {
        return serializedReferenceContext.has_value();
    }
    bool shouldRestoreOnLoad() const;
    bool isScriptEnumerable() const {
        const auto lifetime = durationType();
        return exposed != 0 && serializedType != 67 &&
               (lifetime == DurationType::Temporary || lifetime == DurationType::Permanent);
    }
    bool hasSerializableTemporalProvenance() const {
        return durationType() != DurationType::Temporary ||
               expiryOrigin != EffectExpiryOrigin::None;
    }

    std::shared_ptr<Object> boundCreator() const;
    std::shared_ptr<Object> boundObjectParameter(size_t index) const;
    bool appliesVersus(const Creature *creature) const;
    /** Equipped effects stop contributing as soon as their exact Item retires. */
    bool hasLiveRuntimeSource() const;

    /**
     * Rebase live object bindings at an Area lifetime boundary.
     *
     * Effects are durable gameplay state, but their object identities are not:
     * a saved-graph identity and a module-owned runtime object both retire with
     * the outgoing Area. Only bindings to explicitly retained session objects
     * survive, rewritten as runtime-session identities.
     */
    void retireAreaRuntimeBindings(
        const std::set<const Object *> &retainedObjects);

private:
    friend class Game;
    std::optional<uint64_t> _runtimeSession;
    std::optional<uint64_t> _savedGraph;
    bool bindCreator(const std::shared_ptr<Object> &object);
    bool bindObjectParameter(size_t index, const std::shared_ptr<Object> &object);
};

/**
 * A serialized EffectInstance used as an NWScript engine value.
 *
 * This deliberately reuses the save-facing effect model. Applying it through
 * Object::applyEffect preserves that semantic payload instead of inventing a
 * second, save-only effect representation.
 */
class SavedEffectValue : public CopyableEffect<SavedEffectValue> {
public:
    explicit SavedEffectValue(EffectInstance instance);
    const EffectInstance &instance() const { return _instance; }
    uint64_t scriptValueId() const override { return _instance.id; }
    EffectInstance saveFacingInstance() const override { return _instance; }
    void setSubType(uint16_t category) override;
    bool setVersusAlignment(int lawChaos, int goodEvil) override;
    bool setVersusRacialType(int racialType) override;

private:
    EffectInstance _instance;
};

} // namespace game

} // namespace reone
