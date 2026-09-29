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

#include "reone/game/object/door.h"

#include "reone/game/action/opendoor.h"
#include "reone/game/attack.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include "reone/graphics/di/services.h"
#include "reone/resource/2da.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/2das.h"
#include "reone/resource/provider/gffs.h"
#include "reone/resource/provider/models.h"
#include "reone/resource/provider/scripts.h"
#include "reone/resource/provider/walkmeshes.h"
#include "reone/resource/resources.h"
#include "reone/resource/strings.h"
#include "reone/scene/di/services.h"
#include "reone/scene/graphs.h"
#include "reone/scene/node/model.h"
#include "reone/scene/types.h"

#include "reone/game/di/services.h"
#include "reone/game/object/area.h"
#include "reone/game/object/module.h"
#include "reone/game/object/trigger.h"
#include "reone/game/party.h"
#include "reone/game/script/runner.h"
#include "reone/game/animations.h"
#include "reone/game/game.h"
#include "reone/game/event.h"
#include "reone/game/object/creature.h"
#include "reone/game/reputes.h"

using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::script;

namespace reone {

namespace game {

void Door::deserialize(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    std::string templateRes;
    if (!identityContext.isSerializedState() &&
        gff.readResRef(templateRes, "TemplateResRef")) {
        if (auto utd = _services.resource.gffs.get(templateRes, ResType::Utd)) {
            deserializeAll(*utd, SerializedIdentityContext::templateResource(templateRes));
        }
    }
    deserializeAll(gff, identityContext);
    loadAppearance();
    applyRestingState();
    updateTransform();
}

void Door::deserializeAll(
    const resource::Gff &gff,
    const SerializedIdentityContext &identityContext) {
    deserializeRuntimeState(gff, identityContext);
    if (gff.readString(_tag, "Tag")) {
        boost::to_lower(_tag);
    }
    if (gff.readLocString(_locName, "LocName", _services.resource.strings)) {
        _name = _locName.str();
    }

    // Only the low byte of the appearance names the door's look.
    uint32_t appearance = _appearance;
    if (gff.readDword(appearance, "Appearance")) _appearance = static_cast<uint8_t>(appearance);
    gff.readByte(_genericType, "GenericType");
    {
        // OpenState names the resting state the door was authored in. Every K1
        // and K2 blueprint that sets it sets AnimationState to the same value,
        // so OpenState on its own is authoritative.
        uint8_t openState;
        if (gff.readByte(openState, "OpenState")) {
            switch (openState) {
            case 0:
                _state = DoorState::Closed;
                break;
            case 1:
                _state = DoorState::Opened1;
                break;
            case 3:
                _state = DoorState::Destroyed;
                break;
            case 2:
                _state = DoorState::Opened2;
                break;
            default:
                warn(str(boost::format("Door: unsupported OpenState %d, treating as closed") % static_cast<int>(openState)));
                _state = DoorState::Closed;
                break;
            }
        }
    }
    gff.readBool(_autoRemoveKey, "AutoRemoveKey");
    {
        float bearing;
        if (gff.readFloat(bearing, "Bearing")) {
            _orientation = glm::quat(glm::vec3(0.0f, 0.0f, bearing));
        }
    }
    gff.readFloat(_position[0], "X");
    gff.readFloat(_position[1], "Y");
    gff.readFloat(_position[2], "Z");
    gff.readEnum(_faction, "Faction");
    gff.readByte(_fort, "Fort");
    gff.readByte(_will, "Will");
    gff.readByte(_ref, "Ref");
    if (gff.readShort(_hitPoints, "HP")) {
        _maxHitPoints = _hitPoints;
    }
    int16_t currentHitPoints = static_cast<int16_t>(_currentHitPoints);
    if (gff.readShort(currentHitPoints, "CurrentHP")) _currentHitPoints = currentHitPoints;
    const auto findField = [&gff](std::string_view label) -> const Gff::Field * {
        const auto &fields = gff.fields();
        const auto it = std::find_if(fields.begin(), fields.end(),
            [label](const Gff::Field &field) { return field.label == label; });
        return it != fields.end() ? &*it : nullptr;
    };
    // A successfully read Invulnerable byte takes precedence over Plot,
    // including an explicit zero.
    auto protection = findField("Invulnerable");
    if (!protection || protection->type != Gff::FieldType::Byte) {
        protection = findField("Plot");
    }
    if (protection && protection->type == Gff::FieldType::Byte) {
        _plot = protection->uintValue != 0;
    }
    gff.readBool(_minOneHP, "Min1HP");
    if (gff.readString(_keyName, "KeyName")) {
        boost::to_lower(_keyName);
    }
    gff.readBool(_keyRequired, "KeyRequired");
    gff.readByte(_openLockDC, "OpenLockDC");
    gff.readByte(_closeLockDC, "CloseLockDC");
    gff.readByte(_secretDoorDC, "SecretDoorDC");
    gff.readResRef(_conversation, "Conversation");
    gff.readWord(_portraitId, "PortraitId");
    gff.readByte(_hardness, "Hardness");
    gff.readResRef(_onClosed, "OnClosed");
    gff.readResRef(_onDamaged, "OnDamaged");
    gff.readResRef(_onDeath, "OnDeath");
    gff.readResRef(_onDisarm, "OnDisarm");
    gff.readResRef(_onHeartbeat, "OnHeartbeat");
    gff.readResRef(_onLock, "OnLock");
    gff.readResRef(_onMeleeAttacked, "OnMeleeAttacked");
    gff.readResRef(_onOpen, "OnOpen");
    gff.readResRef(_onSpellCastAt, "OnSpellCastAt");
    gff.readResRef(_onTrapTriggered, "OnTrapTriggered");
    gff.readResRef(_onUnlock, "OnUnlock");
    gff.readResRef(_onUserDefined, "OnUserDefined");
    gff.readResRef(_onClick, "OnClick");
    gff.readResRef(_onFailToOpen, "OnFailToOpen");
    gff.readResRef(_onDialog, "OnDialog");
    gff.readByte(_trapType, "TrapType");
    gff.readBool(_trapDisarmable, "TrapDisarmable");
    gff.readBool(_trapDetectable, "TrapDetectable");
    // A saved door without a DC keeps the current one as shifted for difficulty.
    // Placed doors take their DCs from the blueprint alone.
    if (!gff.readByte(_disarmDC, "DisarmDC") && identityContext.isSerializedState())
        _disarmDC = static_cast<uint8_t>(trapDisarmDC());
    if (!gff.readByte(_trapDetectDC, "TrapDetectDC") && identityContext.isSerializedState())
        _trapDetectDC = static_cast<uint8_t>(trapDetectDC());
    gff.readByte(_trapFlag, "TrapFlag");
    gff.readBool(_trapOneShot, "TrapOneShot");
    gff.readInt(_ownerDemolitionsSkill, "OwnerDemolitions");
    gff.readBool(_locked, "Locked");
    gff.readBool(_lockable, "Lockable");
    gff.readByte(_linkedToFlags, "LinkedToFlags");
    gff.readString(_linkedTo, "LinkedTo");
    gff.readResRef(_linkedToModule, "LinkedToModule");
    gff.readWord(_loadScreenId, "LoadScreenID");
    gff.readLocString(_description, "Description", _services.resource.strings);
    if (const auto field = findField("Static"); field && field->type == Gff::FieldType::Byte) {
        _static = field->uintValue != 0;
    }
    if (_static) _plot = true;
    gff.readBool(_notBlastable, "NotBlastable");
    gff.readLocString(_transitionDestin, "TransitionDestin", _services.resource.strings);

    // FIXME: deserialize EffectList, ActionList
}

// The appearance picks the door's look. A door with one takes
// its model from doortypes and is never used precisely; one without takes both
// from its generic type in genericdoors. The walkmeshes follow the model.
void Door::loadAppearance() {
    std::string modelName;
    if (_appearance != 0) {
        std::shared_ptr<TwoDA> types(_services.resource.twoDas.get("doortypes"));
        modelName = boost::to_lower_copy(types->getString(_appearance, "model"));
        _preciseUse = false;
        _blocksSight = types->getInt(_appearance, "blocksight", 1) != 0;
    } else {
        std::shared_ptr<TwoDA> doors(_services.resource.twoDas.get("genericdoors"));
        modelName = boost::to_lower_copy(doors->getString(_genericType, "modelname"));
        _preciseUse = doors->getInt(_genericType, "preciseuse", 0) != 0;
        _blocksSight = doors->getInt(_genericType, "blocksight", 1) != 0;
    }

    _linkedTransitionGeometry.clear();
    auto walkmeshClosed = _services.resource.walkmeshes.get(modelName + "0", ResType::Dwk);
    if (walkmeshClosed) {
        loadLinkedTransitionGeometry(*walkmeshClosed);
    }

    auto model = _services.resource.models.get(modelName);
    if (!model) {
        return;
    }
    auto &sceneGraph = _services.scene.graphs.get(_sceneName);

    auto modelSceneNode = sceneGraph.newModel(*model, ModelUsage::Door);
    modelSceneNode->setUser(*this);
    // Under Force Sight a door is drawn see-through.
    modelSceneNode->setForceSightStyle(scene::ModelSceneNode::ForceSightStyle::Translucent);
    // modelSceneNode->setDrawDistance(_game.options().graphics.drawDistance);
    _sceneNode = std::move(modelSceneNode);

    if (walkmeshClosed) {
        _walkmeshClosed = sceneGraph.newWalkmesh(*walkmeshClosed);
        _walkmeshClosed->setUser(*this);
    }

    auto walkmeshOpen1 = _services.resource.walkmeshes.get(modelName + "1", ResType::Dwk);
    if (walkmeshOpen1) {
        _walkmeshOpen1 = sceneGraph.newWalkmesh(*walkmeshOpen1);
        _walkmeshOpen1->setUser(*this);
        _walkmeshOpen1->setEnabled(false);
    }

    auto walkmeshOpen2 = _services.resource.walkmeshes.get(modelName + "2", ResType::Dwk);
    if (walkmeshOpen2) {
        _walkmeshOpen2 = sceneGraph.newWalkmesh(*walkmeshOpen2);
        _walkmeshOpen2->setUser(*this);
        _walkmeshOpen2->setEnabled(false);
    }

    // A door authored open must load directly into its opened pose, with the
    // matching walkmesh, rather than resting shut and playing an opening
    // transition it already finished before the module was entered.
    applyRestingState();
}

void Door::applyRestingState() {
    _transition = DoorTransition::None;
    _open = _state != DoorState::Closed;
    enableStateWalkmeshes();
    applyRestingPose();
}

void Door::enableStateWalkmeshes() {
    if (_walkmeshClosed) {
        _walkmeshClosed->setEnabled(_state == DoorState::Closed);
    }
    if (_walkmeshOpen1) {
        _walkmeshOpen1->setEnabled(_state == DoorState::Opened1);
    }
    if (_walkmeshOpen2) {
        _walkmeshOpen2->setEnabled(_state == DoorState::Opened2);
    }
}

void Door::applyRestingPose() {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model) {
        return;
    }
    // Every K1 and K2 door model carries these as zero-length pose animations.
    // They finish on the update they are played, and a finished non-fire-forget
    // channel is retained, so the pose holds.
    switch (_state) {
    case DoorState::Opened1:
        model->playAnimation("opened1");
        break;
    case DoorState::Opened2:
        model->playAnimation("opened2");
        break;
    case DoorState::Destroyed:
        model->playAnimation(_services.game.animations.getNameById(330));
        break;
    case DoorState::Closed:
    default:
        model->playAnimation("closed");
        break;
    }
}

void Door::loadLinkedTransitionGeometry(const Walkmesh &walkmesh) {
    AABB bounds;
    for (const auto &face : walkmesh.faces) {
        for (const auto &vertex : face.indices) {
            bounds.expand(walkmesh.vertices[vertex]);
        }
    }
    if (bounds.isDegenerate() || bounds.min().x == bounds.max().x || bounds.min().y == bounds.max().y) {
        return;
    }

    float z = bounds.min().z;
    _linkedTransitionGeometry = {
        glm::vec3(bounds.min().x, bounds.min().y, z),
        glm::vec3(bounds.min().x, bounds.max().y, z),
        glm::vec3(bounds.max().x, bounds.max().y, z),
        glm::vec3(bounds.max().x, bounds.min().y, z)};
}

bool Door::isSelectable() const {
    // A door already swinging open is on its way out of reach even though it is
    // not open yet, and offering it again would only re-run OnOpen.
    return !_static && !_open && !isOpening();
}

void Door::damage(
    int amount,
    const std::shared_ptr<Object> &damager) {
    if (isDead() || _plot || _notBlastable) {
        return;
    }
    if (amount <= 0) {
        return;
    }

    setLastDamager(damager);
    uint32_t damagerId = getLastDamager();
    const int hitPointsBefore = currentHitPoints();
    if (amount == std::numeric_limits<int>::max()) {
        _currentHitPoints = isMinOneHP() ? 1 : 0;
    } else {
        int adjustedAmount = applyDamageToHitPoints(amount, hitPointsBefore);
        _game.floatingText().addDamage(
            *this, amount, adjustedAmount, damagerId);
    }

    // Detached objects have no world queue for a damaged notification.
    if (_game.module())
        _game.queueScriptEvent(*this, damager.get(), Event(4, {amount}, {}, {}, {}));

    if (!isDead()) {
        return;
    }

    _locked = false;
    open();
    onOpen(damagerId);
    runDeathScript();
}

void Door::open() {
    if (isOpening()) {
        // Already swinging open. Restarting would rewind the leaf and re-run
        // whatever the caller does around the transition.
        return;
    }
    if (_state != DoorState::Closed && _transition == DoorTransition::None) {
        return;
    }

    // A door that is swinging aside is still in the doorway. K2 will not
    // let the player through one until the opening animation has finished, so
    // the closed walkmesh stays enabled for the whole transition and only comes
    // off when the door has physically arrived at its opened pose.
    beginTransition(DoorTransition::Opening);
}

void Door::close() {
    if (isClosing()) {
        return;
    }
    if (_state == DoorState::Closed && _transition == DoorTransition::None) {
        // Already shut and blocking. Closing again must not replay the
        // transition, which would briefly reopen the doorway to collision.
        return;
    }

    // The mirror image: a door that is swinging shut has not sealed the doorway
    // yet. Scripts routinely close a door behind somebody who is still walking
    // through it - K2 103PER closes TO102PER on the same beat that sends the
    // player through it, and DOR_PER03 takes 2.667 seconds to shut - so the
    // doorway stays passable until the door reaches its closed pose.
    beginTransition(DoorTransition::Closing);
}

void Door::beginTransition(DoorTransition transition) {
    // _state is left alone. It names where the door still physically stands,
    // and everything that can obstruct or admit a creature is read off it, so
    // the door keeps the collision of the state it is leaving until it arrives.
    _transition = transition;

    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (model) {
        model->playAnimation(transitionAnimation(transition));
    }
    if (isTransitionComplete()) {
        // No model, or no such animation on it: there is nothing to wait for,
        // so the door arrives immediately rather than being stranded in a
        // transition that can never complete.
        finishTransition();
    }
}

void Door::finishTransition() {
    _state = transitionTarget(_transition);
    applyRestingState();
}

void Door::update(float dt) {
    // A dead door runs no heartbeat.
    updateStampedHeartbeat(_onHeartbeat, !isDead());
    Object::update(dt);
    updateMineBlast(dt);

    if (_transition != DoorTransition::None && isTransitionComplete()) {
        finishTransition();
    }
}

bool Door::isTransitionComplete() const {
    auto model = std::static_pointer_cast<ModelSceneNode>(_sceneNode);
    if (!model) {
        return true;
    }
    // Something other than this transition owns the model - a reversal, or an
    // animation the door never had - so there is nothing left to wait for.
    if (model->activeAnimationName() != transitionAnimation(_transition)) {
        return true;
    }
    return model->isAnimationFinished();
}

const char *Door::transitionAnimation(DoorTransition transition) {
    return transition == DoorTransition::Opening ? "opening1" : "closing1";
}

DoorState Door::transitionTarget(DoorTransition transition) {
    return transition == DoorTransition::Opening ? DoorState::Opened1 : DoorState::Closed;
}

DoorState Door::actionState() const {
    return _transition == DoorTransition::None ? _state : transitionTarget(_transition);
}

std::optional<std::array<glm::vec3, 2>> Door::actionPoints(DoorState state) const {
    std::shared_ptr<WalkmeshSceneNode> node;
    switch (state) {
    case DoorState::Closed:
        node = _walkmeshClosed;
        break;
    case DoorState::Opened1:
        node = _walkmeshOpen1;
        break;
    case DoorState::Opened2:
        node = _walkmeshOpen2;
        break;
    default:
        return std::nullopt;
    }
    // Without a walkmesh the points lie at the door's own origin.
    std::array<glm::vec3, 2> points {_position, _position};
    if (!node) {
        return points;
    }
    const Walkmesh &walkmesh = node->walkmesh();
    for (size_t i = 0; i < points.size(); ++i) {
        // A point placed absolutely stands in for the relative one.
        const glm::vec3 &absolute = walkmesh.absoluteUsePositions[i];
        const glm::vec3 &local = absolute != glm::vec3(0.0f) ? absolute : walkmesh.relativeUsePositions[i];
        points[i] = glm::vec3(_transform * glm::vec4(local + walkmesh.position, 1.0f));
    }
    return points;
}

glm::vec3 Door::nearestActionPoint(const glm::vec3 &from, bool closed) const {
    // A point beyond this squared distance does not compete with a closed
    // state point when the door's own state offers none.
    static constexpr float kNoPointDistance2 = 1000.0f;

    Area *area = spatialArea();
    auto walkable = [&](const glm::vec3 &point) {
        return area && area->groundHeight(point).has_value();
    };
    auto distance2 = [&](const glm::vec3 &point) {
        const glm::vec3 offset(from - point);
        return glm::dot(offset, offset);
    };

    const DoorState state = closed ? DoorState::Closed : actionState();
    std::optional<glm::vec3> best;
    float bestDistance2 = kNoPointDistance2;
    if (auto points = actionPoints(state)) {
        const auto &[first, second] = *points;
        const float firstDistance2 = distance2(first);
        const float secondDistance2 = distance2(second);
        if (secondDistance2 > firstDistance2) {
            if (walkable(first)) {
                best = first;
                bestDistance2 = firstDistance2;
            } else if (walkable(second)) {
                best = second;
                bestDistance2 = secondDistance2;
            }
        } else if (walkable(second)) {
            best = second;
            bestDistance2 = secondDistance2;
        }
    }
    if (state != DoorState::Closed) {
        const auto closedPoints = actionPoints(DoorState::Closed);
        for (const glm::vec3 &point : *closedPoints) {
            if (bestDistance2 > distance2(point) && walkable(point)) {
                return point;
            }
        }
    }
    return best.value_or(glm::vec3(0.0f));
}

void Door::onOpen(uint32_t triggererId) {
    if (_onOpen.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onOpen,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastOpenedBy, Variable::ofObject(triggererId)},
         {script::ArgKind::ClickingObject, Variable::ofObject(triggererId)},
         {script::ArgKind::EnteringObject, Variable::ofObject(triggererId)}});
}

void Door::onLocked() {
    if (_onLock.empty()) {
        return;
    }
    _game.scriptRunner().run(_onLock, {{script::ArgKind::Caller, Variable::ofObject(_id)}});
}

void Door::onFailToOpen(uint32_t openerId, bool quiet) {
    if (!_onFailToOpen.empty()) {
        _game.scriptRunner().run(
            _onFailToOpen,
            {{script::ArgKind::Caller, Variable::ofObject(_id)},
             {script::ArgKind::ClickingObject, Variable::ofObject(openerId)}});
    }
    if (!_locked || quiet) return;
    // A door with an order to open itself says nothing.
    for (const auto &action : actions()) {
        auto *opening = dyn_cast<OpenDoorAction>(action.get());
        if (opening && opening->door().get() == this) return;
    }
    // Otherwise the creature the player controls hears the door is locked.
    static constexpr int kLockedStrRef = 1437;
    auto leader = _game.party().getLeader();
    if (leader && leader->id() == openerId) _game.addFeedbackMessage(kLockedStrRef);
}

void Door::runDamagedScript() {
    if (_onDamaged.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onDamaged,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastAttacker, Variable::ofObject(getLastDamager())},
         {script::ArgKind::LastDamager, Variable::ofObject(getLastDamager())}});
}

void Door::runDeathScript() {
    if (_onDeath.empty()) {
        return;
    }
    _game.scriptRunner().run(
        _onDeath,
        {{script::ArgKind::Caller, Variable::ofObject(_id)},
         {script::ArgKind::LastAttacker, Variable::ofObject(getLastDamager())},
         {script::ArgKind::LastDamager, Variable::ofObject(getLastDamager())}});
}

void Door::enterDestroyedState() {
    _state = DoorState::Destroyed;
    applyRestingState();
}

void Door::receiveDamagedSignal(const std::shared_ptr<Object> &damager) {
    setLastDamager(damager);
    setLastHostileActor(damager ? damager->id() : script::kObjectInvalid);
    runDamagedScript();
}

void Door::receiveAttackEvent(uint32_t attackerId, const AttackEventFields *fields) {
    recordReceivedAttack(attackerId, fields ? fields->weaponAttackType : 0);
    if (!isRuntimeLive()) return;
    // The melee-attacked script runs whether or not the object is destroyed.
    _game.scriptRunner().run(_onMeleeAttacked, id());
    // An armed trap goes off when it is struck from close by.
    static constexpr float kTrapBashRange = 4.0f;
    auto attacker = std::dynamic_pointer_cast<Creature>(_game.getObjectById(attackerId));
    if (_trapFlag && attacker && getSquareDistanceTo(*attacker) <= kTrapBashRange * kTrapBashRange)
        triggerTrap(attacker, false);
}

void Door::receiveDeathSignal(uint32_t killerId) {
    setLastKiller(killerId);
    _game.scriptRunner().run(_onDeath, {
        {script::ArgKind::Caller, Variable::ofObject(_id)}});
}

void Door::setLocked(bool locked) {
    _locked = locked;
}

bool canBashDoor(const Door &door) {
    return door.isLocked() &&
           door.isSelectable() &&
           !door.isDead() &&
           !door.plotFlag() &&
           !door.isNotBlastable() &&
           (door.hitPoints() > 0 || door.currentHitPoints() > 0);
}

void Door::updateTransform() {
    Object::updateTransform();

    if (_walkmeshOpen1) {
        _walkmeshOpen1->setLocalTransform(_transform);
    }
    if (_walkmeshOpen2) {
        _walkmeshOpen2->setLocalTransform(_transform);
    }
    if (_walkmeshClosed) {
        _walkmeshClosed->setLocalTransform(_transform);
    }
}

void Door::applyDamageEffect(
    int amount,
    const std::shared_ptr<Object> &damager,
    std::optional<DamageReaction>) {


    if (amount == 0) {
        _game.floatingText().addDamage(*this, 0, 0, getLastDamager());
        if (_game.module())
            _game.queueScriptEvent(*this, damager.get(), Event(4, {amount}, {}, {}, {}));
        return;
    }

    damage(amount, damager);
}

int Door::savingThrow(SavingThrow save) const {
    switch (save) {
    case SavingThrow::Fortitude:
        return _fort;
    case SavingThrow::Reflex:
        return _ref;
    case SavingThrow::Will:
        return _will;
    default:
        return 0;
    }
}

int Door::trapDetectDC() const {
    if (!_game.isTSL()) return _trapDetectDC;
    const int dc = _trapDetectDC + _game.trapDifficultyModifier();
    return dc <= 0 ? 1 : static_cast<uint8_t>(dc);
}

int Door::trapDisarmDC() const {
    if (!_game.isTSL()) return _disarmDC;
    const auto dc = static_cast<uint8_t>(_disarmDC + _game.trapDifficultyModifier());
    return dc == 0 ? 1 : dc;
}

// Traps

static constexpr int kTrapFriendlyStanding = 89;
static constexpr int kTrapTriggeredStrRef = 1461;
static constexpr float kMineBlastDelay = 3.0f;
static constexpr float kMineOpenDelay = 1.0f;
static constexpr int kMineBlastSuccessStrRef = 128534;
static constexpr int kMineBlastFailureStrRef = 128533;
static constexpr int kMineBlastReportStrRef = 128535;

bool Door::isTrapHostileTo(const Creature &creature) const {
    return getObjectReputation(*this, creature, _game) <= kTrapFriendlyStanding &&
           _faction != creature.faction();
}

void Door::triggerTrap(const std::shared_ptr<Creature> &caller, bool force) {
    if (!caller || caller->hasEffectImmunity(ImmunityType::Trap)) return;
    if (!force && !isTrapHostileTo(*caller)) return;
    if (_trapFlag) {
        if (_game.party().getLeader() == caller) {
            _game.messageLog().add(
                MessageLog::kFeedbackMessageType,
                MessageLog::Style::Normal,
                _game.getFeedbackText(kTrapTriggeredStrRef));
        }
        _game.scriptRunner().run(_onTrapTriggered, {
            {script::ArgKind::Caller, script::Variable::ofObject(_id)},
            {script::ArgKind::EnteringObject, script::Variable::ofObject(caller->id())}});
        if (_trapOneShot) _trapFlag = 0;
    }
    _trapDetection.detectedBy.clear();
    removeLinkedMine();
}

void Door::disarmTrap(const Object &caller) {
    if (_trapFlag) {
        _trapFlag = 0;
        _game.scriptRunner().run(_onDisarm, {
            {script::ArgKind::Caller, script::Variable::ofObject(_id)},
            {script::ArgKind::LastDisarmed, script::Variable::ofObject(caller.id())}});
    }
    _trapDetection.detectedBy.clear();
}

void Door::armMine(int trapType, int detectDC, int disarmDC, int ownerDemolitionsSkill,
                    const std::shared_ptr<Trigger> &linkedMine,
                    const std::shared_ptr<Creature> &setter, int blastBonus, bool blast) {
    _trapFlag = 1;
    _trapType = static_cast<uint8_t>(trapType);
    if (auto traps = _services.resource.twoDas.get("traps"))
        _onTrapTriggered = boost::to_lower_copy(traps->getString(_trapType, "trapscript"));
    _trapDetectDC = static_cast<uint8_t>(detectDC);
    _disarmDC = static_cast<uint8_t>(disarmDC);
    _trapDetectable = true;
    _trapOneShot = true;
    _ownerDemolitionsSkill = ownerDemolitionsSkill;
    // TSL mines on doors and placeables cannot be disarmed; KotOR ones can.
    _trapDisarmable = !_game.isTSL();
    _linkedMine = linkedMine;
    _mineSetter = setter;
    _mineBlastBonus = blastBonus;
    _mineBlastDelay = blast ? kMineBlastDelay : -1.0f;
}

void Door::removeLinkedMine() {
    if (auto mine = _linkedMine.resolve()) {
        if (auto area = _game.module() ? _game.module()->area() : nullptr) area->destroyObject(*mine);
    }
    _linkedMine.reset();
}

void Door::updateMineBlast(float dt) {
    if (_mineOpenDelay >= 0.0f) {
        _mineOpenDelay -= dt;
        if (_mineOpenDelay < 0.0f) openAfterBlast();
    }
    if (_mineBlastDelay < 0.0f) return;
    _mineBlastDelay -= dt;
    if (_mineBlastDelay >= 0.0f) return;
    _mineBlastDelay = -1.0f;
    if (!_trapFlag) return;
    auto setter = _mineSetter.resolve();
    const int rank = setter ? setter->getUnopposedSkillRank(SkillType::Demolitions) : 0;
    const int total = rank / 2 + _mineBlastBonus + 20;
    const int dc = _openLockDC;
    const bool success = total >= dc;
    if (success) {
        _mineOpenDelay = kMineOpenDelay;
        _game.scriptRunner().run(_onUnlock, {{script::ArgKind::Caller, script::Variable::ofObject(_id)}});
    }
    _game.setCustomToken(1, _game.getFeedbackText(success ? kMineBlastSuccessStrRef : kMineBlastFailureStrRef));
    _game.setCustomToken(2, std::to_string(total));
    _game.setCustomToken(3, "20");
    _game.setCustomToken(4, std::to_string(rank));
    _game.setCustomToken(5, std::to_string(_mineBlastBonus));
    _game.setCustomToken(6, std::to_string(dc));
    _game.messageLog().add(
        MessageLog::kFeedbackMessageType,
        MessageLog::Style::Normal,
        _game.getFeedbackText(kMineBlastReportStrRef));
    _trapFlag = 0;
    _trapDetection.detectedBy.clear();
    removeLinkedMine();
}

void Door::openAfterBlast() {
    _locked = false;
    open();
}

// END Traps

} // namespace game

} // namespace reone
