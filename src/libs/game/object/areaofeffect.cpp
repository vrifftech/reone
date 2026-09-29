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

#include "reone/game/object/areaofeffect.h"

#include <array>
#include <cmath>
#include <limits>

#include <boost/algorithm/string.hpp>

#include "reone/game/d20/spell.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/effect/areaofeffect.h"
#include "reone/game/event.h"
#include "reone/game/game.h"
#include "reone/game/location.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/savedruntime.h"
#include "reone/game/script/runner.h"
#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/2das.h"

using namespace reone::resource;

namespace reone {

namespace game {

static constexpr int kEnteredEvent = 12;
static constexpr int kExitedEvent = 13;
// A saved orientation shorter than this faces +Y.
static constexpr float kMinOrientationLength2 = 1.0e-4f;

void AreaOfEffect::deserialize(
    const Gff &gff,
    const SerializedIdentityContext &identityContext) {
    deserializeRuntimeState(gff, identityContext);
    if (gff.readString(_tag, "Tag")) {
        boost::to_lower(_tag);
    }
    gff.readInt(_areaEffectId, "AreaEffectId");
    const auto shape = static_cast<Shape>(gff.getUint("Shape"));
    if (shape == Shape::Rectangle) {
        setShape(shape, gff.getFloat("Width"), gff.getFloat("Length"));
    } else if (shape == Shape::Circle) {
        setShape(shape, gff.getFloat("Radius"), 0.0f);
    }
    _metaMagic = static_cast<int>(gff.getUint("MetaMagicType"));
    gff.readInt(_spellSaveDC, "SpellSaveDC");
    gff.readInt(_spellLevel, "SpellLevel");
    for (const char *field : {kCarrierReference, kLastEnteredReference, kLastLeftReference}) {
        uint32_t id = kSavedRuntimeInvalidObjectId;
        if (gff.readDword(id, field) && id != kSavedRuntimeInvalidObjectId) {
            _savedReferenceIds.insert_or_assign(field, id);
        }
    }
    _remainingDuration = static_cast<float>(gff.getUint("Duration")) / 1000.0f;
    _durationType = static_cast<DurationType>(gff.getUint("DurationType", static_cast<uint32_t>(DurationType::Permanent)));
    gff.readDword(_heartbeatDay, "LastHrtbtDay");
    gff.readDword(_heartbeatTime, "LastHrtbtTime");
    // The enter, exit, heartbeat and user-defined scripts are saved but not
    // read back: a loaded area of effect runs no scripts.

    gff.readFloat(_position.x, "PositionX");
    gff.readFloat(_position.y, "PositionY");
    gff.readFloat(_position.z, "PositionZ");
    const glm::vec3 orientation(
        gff.getFloat("OrientationX"), gff.getFloat("OrientationY"), gff.getFloat("OrientationZ"));
    const float facing = glm::dot(orientation, orientation) > kMinOrientationLength2
        ? std::atan2(orientation.y, orientation.x)
        : glm::half_pi<float>();
    _orientation = glm::quat(glm::vec3(0.0f, 0.0f, objectFacingFromScript(facing)));
    updateTransform();
}

void AreaOfEffect::loadAreaEffect(int areaEffectId) {
    _areaEffectId = areaEffectId;
    auto table = getRequiredTwoDA(_services.resource.twoDas, "vfx_persistent");
    const std::string shape = table->getString(areaEffectId, "shape");
    if (shape == "C") {
        setShape(Shape::Circle, table->getFloat(areaEffectId, "radius"), 0.0f);
    } else if (shape == "R") {
        setShape(Shape::Rectangle, table->getFloat(areaEffectId, "width"), table->getFloat(areaEffectId, "length"));
    }
    _onEnter = table->getString(areaEffectId, "onenter");
    _onHeartbeat = table->getString(areaEffectId, "heartbeat");
    _onExit = table->getString(areaEffectId, "onexit");
    _tag = boost::to_lower_copy(table->getString(areaEffectId, "label"));
}

void AreaOfEffect::overrideScripts(const std::string &onEnter, const std::string &heartbeat, const std::string &onExit) {
    if (!onEnter.empty()) _onEnter = onEnter;
    if (!heartbeat.empty()) _onHeartbeat = heartbeat;
    if (!onExit.empty()) _onExit = onExit;
}

void AreaOfEffect::setCreator(const std::shared_ptr<Object> &creator) {
    _savedReferences[kCreatorReference] = creator;
    auto *creature = creator ? dyn_cast<Creature>(creator.get()) : nullptr;
    if (!creature || creature->effectSpellId() == std::numeric_limits<uint32_t>::max()) {
        return;
    }
    const int spellId = static_cast<int>(creature->effectSpellId());
    _metaMagic = creature->spellCastContext().metaMagic;
    _spellSaveDC = creature->getSpellSaveDC(spellId);
    // A spell cast through a class takes that class's level; any other source
    // takes twice the spell's innate level less one.
    const int castingClass = creature->spellCastContext().castingClass;
    if (castingClass < kSpellLikeAbilityClass) {
        _spellLevel = creature->adjustedClassLevel(castingClass);
    } else if (auto spell = _services.game.spells.get(static_cast<SpellType>(spellId))) {
        _spellLevel = 2 * static_cast<int>(spell->innateLevel) - 1;
    }
}

void AreaOfEffect::setCarrier(const std::shared_ptr<Creature> &carrier) {
    _savedReferences[kCarrierReference] = std::static_pointer_cast<Object>(carrier);
}

void AreaOfEffect::setDuration(DurationType type, float seconds) {
    _durationType = type;
    if (seconds > 0.0f) {
        _remainingDuration = seconds;
    }
}

void AreaOfEffect::setShape(Shape shape, float radiusOrWidth, float length) {
    _shape = shape;
    if (shape == Shape::Rectangle) {
        _width = radiusOrWidth;
        _length = length;
        _radius = std::sqrt(0.25f * (_width * _width + _length * _length));
    } else {
        _radius = radiusOrWidth;
    }
}

glm::vec3 AreaOfEffect::center() const {
    if (_shape == Shape::Circle) {
        if (auto carrier = this->carrier()) return carrier->position();
    }
    return _position;
}

void AreaOfEffect::fixCorners() {
    if (_shape != Shape::Rectangle) return;
    const float facing = getFacing();
    const glm::vec2 along(-std::sin(facing), std::cos(facing));
    const glm::vec2 across(-along.y, along.x);
    const glm::vec2 halfLength(along * (0.5f * _length));
    const glm::vec2 halfWidth(across * (0.5f * _width));
    const glm::vec2 position(_position);
    _corners = {
        position + halfLength + halfWidth,
        position + halfLength - halfWidth,
        position - halfLength - halfWidth,
        position - halfLength + halfWidth};
}

// The circle, or the circle around the rectangle, is tested in three
// dimensions. A rectangle also needs the point on the inner side of each edge
// between the corners fixed where it was placed.
bool AreaOfEffect::contains(const glm::vec3 &point) const {
    const glm::vec3 offset(point - center());
    if (glm::dot(offset, offset) > _radius * _radius) return false;
    if (_shape == Shape::Circle) return true;

    for (size_t i = 0; i < _corners.size(); ++i) {
        const glm::vec2 edge(_corners[(i + 1) % _corners.size()] - _corners[i]);
        const glm::vec2 toPoint(glm::vec2(point) - _corners[i]);
        if (toPoint.y * edge.x - toPoint.x * edge.y > 0.0f) return false;
    }
    return true;
}

void AreaOfEffect::update(float dt) {
    updateStampedHeartbeat(_onHeartbeat);
    if (isCarried()) followCarrier();
    if (_durationType == DurationType::Temporary) {
        if (_remainingDuration <= dt) {
            destroy();
            return;
        }
        _remainingDuration -= dt;
    }
    Object::update(dt);
}

bool AreaOfEffect::isCarried() const {
    return _savedReferences.count(kCarrierReference) != 0 || _savedReferenceIds.count(kCarrierReference) != 0;
}

// A carried area of effect goes when its carrier leaves the area. Only a
// circle goes along with it; a rectangle stays where it was placed.
void AreaOfEffect::followCarrier() {
    auto carrier = this->carrier();
    if (!carrier || carrier->spatialArea() != spatialArea()) {
        destroy();
        return;
    }
    if (_shape != Shape::Circle) return;
    scanOccupants();
    setPosition(carrier->position());
}

void AreaOfEffect::scanOccupants(bool announce) {
    const glm::vec3 origin(center());
    for (Object *object : spatialArea()->objectsInXRange(origin.x - _radius, origin.x + _radius)) {
        if (object->type() != ObjectType::Creature) continue;
        updateOccupancy(_game.getObjectById<Creature>(object->id()), announce);
    }
}

void AreaOfEffect::updateOccupancy(const std::shared_ptr<Creature> &creature, bool announce) {
    const bool inside = contains(creature->position());
    const bool known = _occupants.count(creature->id()) != 0;
    if (inside == known) return;
    if (inside) {
        _occupants.insert(creature->id());
    } else {
        _occupants.erase(creature->id());
    }
    if (announce) {
        _game.queueScriptEvent(*this, creature.get(),
            Event(inside ? kEnteredEvent : kExitedEvent, {}, {}, {}, {creature}));
    }
}

// The carrier stays inside without a signal. The others inside leave, and
// the creatures at the new place enter.
void AreaOfEffect::jumpToCarrier() {
    auto carrier = this->carrier();
    _occupants.erase(carrier->id());
    const glm::vec3 origin(center());
    for (Object *object : spatialArea()->objectsInXRange(origin.x - _radius, origin.x + _radius)) {
        if (object->type() != ObjectType::Creature || _occupants.count(object->id()) == 0 ||
            !contains(object->position())) continue;
        auto creature = _game.getObjectById<Creature>(object->id());
        _occupants.erase(creature->id());
        _game.queueScriptEvent(*this, creature.get(), Event(kExitedEvent, {}, {}, {}, {creature}));
    }
    setPosition(carrier->position());
    _occupants.insert(carrier->id());
    scanOccupants();
}

void AreaOfEffect::releaseOccupants() {
    const glm::vec3 origin(center());
    for (Object *object : spatialArea()->objectsInXRange(origin.x - _radius, origin.x + _radius)) {
        if (object->type() != ObjectType::Creature || _occupants.count(object->id()) == 0 ||
            !contains(object->position())) continue;
        _occupants.erase(object->id());
        receiveExitedSignal(_game.getObjectById(object->id()));
    }
}

void AreaOfEffect::receiveEnteredSignal(const std::shared_ptr<Object> &entering) {
    _savedReferences[kLastEnteredReference] = entering;
    _game.scriptRunner().run(_onEnter, {
        {script::ArgKind::Caller, script::Variable::ofObject(_id)},
        {script::ArgKind::EnteringObject, script::Variable::ofObject(entering ? entering->id() : script::kObjectInvalid)}});
}

void AreaOfEffect::receiveExitedSignal(const std::shared_ptr<Object> &exiting) {
    _savedReferences[kLastLeftReference] = exiting;
    _game.scriptRunner().run(_onExit, {
        {script::ArgKind::Caller, script::Variable::ofObject(_id)},
        {script::ArgKind::ExitingObject, script::Variable::ofObject(exiting ? exiting->id() : script::kObjectInvalid)}});
}

void AreaOfEffect::destroy() {
    _game.postObjectDestruction(*this, nullptr, 0.0f, true);
}

EffectApplicationResult AreaOfEffectEffect::onApply(Object &object, EffectInstance &instance) {
    if (!isa<Creature>(&object)) return EffectApplicationResult::Rejected;
    // A creature outside any area carries the effect without a shape.
    Area *area = object.spatialArea();
    if (!area) return EffectApplicationResult::Retained;
    auto areaOfEffect = area->spawnAreaOfEffect(
        instance, object.position(), objectFacingFromScript(0.0f),
        object.game().getObjectById<Creature>(object.id()));
    instance.objectParameters[0] = areaOfEffect->id();
    instance.objectParameterObjects[0] = std::static_pointer_cast<Object>(areaOfEffect);
    return EffectApplicationResult::Retained;
}

EffectRemovalResult AreaOfEffectEffect::onRemove(Object &object, const EffectInstance &instance) {
    if (isa<Creature>(&object)) {
        if (auto areaOfEffect = instance.boundObjectParameter(0)) {
            object.game().postObjectDestruction(*areaOfEffect, nullptr, 0.0f, true);
        }
    }
    return EffectRemovalResult::Removed;
}

} // namespace game

} // namespace reone
