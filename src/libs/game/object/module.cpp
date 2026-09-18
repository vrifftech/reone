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

#include "reone/game/object/module.h"
#include "reone/game/castspell.h"
#include "reone/game/d20/spells.h"
#include "reone/game/location.h"

#include "reone/game/action/attackobject.h"
#include "reone/game/action/opencontainer.h"
#include "reone/game/action/opendoor.h"
#include "reone/game/action/startconversation.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/projectiles.h"
#include "reone/game/party.h"
#include "reone/game/script/savedsituation.h"
#include "reone/game/reputes.h"
#include "reone/game/script/runner.h"
#include "reone/resource/di/services.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/provider/gffs.h"
#include "reone/resource/resources.h"
#include "reone/resource/strings.h"
#include "reone/system/exception/validation.h"
#include "reone/system/logutil.h"

#include "../action/commonactions.h"
#include "../savedeventdispatch.h"

#include <algorithm>
#include <boost/algorithm/string/case_conv.hpp>

using namespace reone::graphics;
using namespace reone::resource;
using namespace reone::scene;

namespace reone {

namespace game {

static bool canUseSecurityOnPlaceable(const Placeable &placeable, const Creature &actor) {
    return placeable.hasInventory() &&
           placeable.isLocked() &&
           !placeable.isKeyRequired() &&
           actor.attributes().hasSkill(SkillType::Security);
}

static bool canBashPlaceable(const Placeable &placeable, const Creature &actor, const IReputes &reputes) {
    return placeable.hasInventory() &&
           placeable.isLocked() &&
           placeable.isSelectable() &&
           !placeable.isDead() &&
           !placeable.plotFlag() &&
           !placeable.isNotBlastable() &&
           reputes.getIsEnemy(actor.faction(), placeable.faction()) &&
           (placeable.hitPoints() > 0 || placeable.currentHitPoints() > 0);
}

void Module::load(std::string name, const Gff &ifo, bool restoreSavedWorld) {
    auto parsed = resource::generated::parseIFO(ifo);
    if (parsed.Mod_Entry_Area.empty()) {
        throw ValidationException("Mod_Entry_Area must not be empty");
    }
    auto are = _services.resource.gffs.get(parsed.Mod_Entry_Area, ResType::Are);
    if (!are) {
        throw ResourceNotFoundException(
            "Area ARE not found: " + parsed.Mod_Entry_Area);
    }
    auto git = _services.resource.gffs.get(parsed.Mod_Entry_Area, ResType::Git);
    if (!git) {
        throw ResourceNotFoundException(
            "Area GIT not found: " + parsed.Mod_Entry_Area);
    }
    load(std::move(name), ifo, *are, *git, restoreSavedWorld);
    if (!restoreSavedWorld) {
        runSpawnScripts();
    }
}

void Module::load(
    std::string name,
    const Gff &ifo,
    const Gff &are,
    const Gff &git,
    bool restoreSavedWorld) {
    _name = std::move(name);
    if (restoreSavedWorld) {
        _game.captureSaveResourceShadow(
            {SaveResourceKind::ModuleIfo, _name}, ifo);
    }
    _publishedSavedEvents.clear();
    _savedEventsPublished = false;

    auto ifoParsed = resource::generated::parseIFO(ifo);
    _isSaveGame = restoreSavedWorld && ifoParsed.Mod_IsSaveGame != 0;
    _projectilesAwaitingRestore = restoreSavedWorld;
    if (restoreSavedWorld) {
        const auto identityContext =
            SerializedIdentityContext::moduleGraph(_name);
        deserializeRuntimeState(ifo, identityContext);
        deserializeSavedEventQueue(ifo, identityContext);
        _savedProjectiles.clear();
        for (const auto &record : ifo.getList("ReoneProjectiles")) {
            const auto version = record->getUint("Version");
            if (version == 1 || version == 2 || version == 3) {
                _savedProjectiles.push_back(SavedProjectile::fromGff(*record, identityContext));
            }
        }
        loadLimboCreatures(ifo);
    } else {
        _savedEventQueue = SavedEventQueue {};
        _savedProjectiles.clear();
        _savedEventLive.clear();
    }
    loadInfo(ifoParsed);
    loadArea(ifoParsed, are, git, restoreSavedWorld);

    _area->initCameras(_info.entryPosition, _info.entryFacing);

    loadPlayer();

}

void Module::activate() {
    _area->activate();
}

void Module::loadInfo(const resource::generated::IFO &ifo) {
    // Mod_Name is a localized string: KotOR II modules carry a talk table
    // reference, while KotOR modules usually carry the text inline. LocString
    // resolves whichever form is present, and yields an empty string when
    // neither is.
    _localizedName = LocString(
                         ifo.Mod_Name.first,
                         ifo.Mod_Name.second,
                         _services.resource.strings)
                         .str();

    // Entry location

    _info.entryArea = ifo.Mod_Entry_Area;
    if (_info.entryArea.empty()) {
        throw ValidationException("Mod_Entry_Area must not be empty");
    }

    _info.entryPosition.x = ifo.Mod_Entry_X;
    _info.entryPosition.y = ifo.Mod_Entry_Y;
    _info.entryPosition.z = ifo.Mod_Entry_Z;

    float dirX = ifo.Mod_Entry_Dir_X;
    float dirY = ifo.Mod_Entry_Dir_Y;
    _info.entryFacing = -glm::atan(dirX, dirY);

    _info.onModLoad = boost::to_lower_copy(ifo.Mod_OnModLoad);
    _info.onModStart = boost::to_lower_copy(ifo.Mod_OnModStart);
}

void Module::loadArea(
    const resource::generated::IFO &ifo,
    const Gff &are,
    const Gff &git,
    bool restoreSavedWorld) {
    reone::info("Load area '" + _info.entryArea + "'");

    const auto identityContext = restoreSavedWorld
                                     ? SerializedIdentityContext::moduleGraph(_name)
                                     : SerializedIdentityContext::templateResource(_name);
    std::shared_ptr<Area> candidateArea;
    std::vector<std::shared_ptr<Object>> noObsolete;
    _game.replaceRuntimeObjectGraph(
        noObsolete,
        [&]() {
            if (restoreSavedWorld) {
                auto area = std::find_if(
                    ifo.Mod_Area_list.begin(),
                    ifo.Mod_Area_list.end(),
                    [this](const auto &entry) {
                        return entry.Area_Name == _info.entryArea;
                    });
                uint32_t areaId = area == ifo.Mod_Area_list.end()
                                      ? 1
                                      : area->ObjectId;
                candidateArea = _game.newSavedArea(areaId, identityContext);
            } else {
                candidateArea = _game.newArea();
            }
            candidateArea->load(
                _info.entryArea, are, git, identityContext);
        },
        [&]() noexcept {
            _area = std::move(candidateArea);
        });

}

void Module::runSpawnScripts() {
    _area->runSpawnScripts();
}
void Module::loadLimboCreatures(const resource::Gff &ifo) {
    _limboCreatures.clear();
    const auto identityContext = SerializedIdentityContext::moduleGraph(_name);
    for (const auto &creatureGff : ifo.getList("Creature List")) {
        auto creature = _game.newCreature(*creatureGff, identityContext);
        creature->captureSaveRecord(
            *creatureGff,
            identityContext,
            {SaveRecordOriginKind::ModuleLimboCreature, _name});
        _limboCreatures.push_back(std::move(creature));
    }
}

void Module::loadPlayer() {
    _player = std::make_unique<Player>(*this, *_area, *_area->getCamera(CameraType::ThirdPerson), _game.party());
}

void Module::loadParty(const std::string &entry, bool preserveSavedPlacement) {
    glm::vec3 position(0.0f);
    float facing = 0.0f;
    getEntryPoint(entry, position, facing);

    _area->loadParty(position, facing, preserveSavedPlacement);
    _area->onPartyLeaderMoved(true);
    _area->update3rdPersonCameraFacing();

    // Where the party stands is restored from the save; the area's authored
    // OnEnter still runs. The game defers that script and hands it the
    // load-from-save answer captured when the enter event was made, so the
    // script sees the restore it was created during. Here the script runs
    // inline, still inside the load, so it reads the same answer from the
    // live value without anything needing to carry it.
    _area->runOnEnterScript();
}

void Module::runOnLoadScript() {
    if (_info.onModLoad.empty()) {
        return;
    }
    _game.scriptRunner().run(_info.onModLoad, id());
}

void Module::runOnStartScript() {
    if (_info.onModStart.empty()) {
        return;
    }
    _game.scriptRunner().run(_info.onModStart, id());
}

void Module::getEntryPoint(const std::string &waypoint, glm::vec3 &position, float &facing) const {
    position = _info.entryPosition;
    facing = _info.entryFacing;

    if (!waypoint.empty()) {
        std::shared_ptr<Object> object(
            _area->getObjectByTag(boost::to_lower_copy(waypoint)));
        if (object) {
            position = object->position();
            facing = object->getFacing();
        } else {
            debug("Module entry '" + waypoint + "' not found; using default entry");
        }
    }
}

bool Module::handle(const input::Event &event) {
    if (_player && _player->handle(event))
        return true;
    if (_area->handle(event))
        return true;

    switch (event.type) {
    case input::EventType::MouseMotion:
        if (handleMouseMotion(event.motion))
            return true;
        break;
    case input::EventType::MouseButtonDown:
        if (handleMouseButtonDown(event.button))
            return true;
        break;
    case input::EventType::KeyDown:
        if (handleKeyDown(event.key))
            return true;
        break;
    default:
        break;
    }

    return false;
}

bool Module::handleMouseMotion(const input::MouseMotionEvent &event) {
    CursorType cursor = CursorType::Default;

    auto object = _area->getObjectAt(event.x, event.y);
    if (object && object->isSelectable()) {
        auto objectPtr = _game.getObjectById(object->id());
        _area->hilightObject(objectPtr);

        switch (object->type()) {
        case ObjectType::Creature: {
            if (object->isDead()) {
                cursor = CursorType::Pickup;
            } else {
                auto creature = static_cast<Creature *>(object);
                cursor = isHostileToPartyLeader(*creature) ? CursorType::Attack : CursorType::Talk;
            }
            break;
        }
        case ObjectType::Door:
            cursor = CursorType::Door;
            break;
        case ObjectType::Placeable:
            cursor = CursorType::Pickup;
            break;
        default:
            break;
        }
    } else {
        _area->hilightObject(nullptr);
    }

    _game.setCursorType(cursor);

    return true;
}

bool Module::handleMouseButtonDown(const input::MouseButtonEvent &event) {
    if (event.button != input::MouseButton::Left) {
        return false;
    }
    auto object = _area->getObjectAt(event.x, event.y);
    if (!object || !object->isSelectable()) {
        return false;
    }
    auto objectPtr = _game.getObjectById(object->id());
    if (!objectPtr) {
        return false;
    }
    // World-click handling publishes the selected target before
    // interaction. Repeat clicks must rebind it too; hover and programmatic
    // selection are not substitutes for this player-input producer.
    _game.setLastTarget(objectPtr->id());
    auto selectedObject = _area->selectedObject();
    if (objectPtr != selectedObject) {
        _area->selectObject(objectPtr);
        return true;
    }
    onObjectClick(objectPtr);

    return true;
}

void Module::onObjectClick(const std::shared_ptr<Object> &object) {
    const auto leader = _game.party().getLeader();
    if (!leader || leader->stateControlsActions()) return;
    switch (object->type()) {
    case ObjectType::Creature:
        onCreatureClick(std::static_pointer_cast<Creature>(object));
        break;
    case ObjectType::Door:
        onDoorClick(std::static_pointer_cast<Door>(object));
        break;
    case ObjectType::Placeable:
        onPlaceableClick(std::static_pointer_cast<Placeable>(object));
        break;
    default:
        break;
    }
}

bool Module::isHostileToPartyLeader(const Creature &creature) const {
    if (creature.isDead()) {
        return false;
    }
    return _services.game.reputes.getIsEnemy(creature, *_game.party().getLeader());
}

void Module::onCreatureClick(const std::shared_ptr<Creature> &creature) {
    debug(str(boost::format("Module: click: creature '%s', faction %d") % creature->tag() % static_cast<int>(creature->faction())));

    std::shared_ptr<Creature> partyLeader(_game.party().getLeader());

    if (creature->isDead()) {
        if (!creature->items().empty()) {
            partyLeader->clearAllActions();
            partyLeader->addAction(_game.newAction<OpenContainerAction>(creature));
        }
    } else {
        if (isHostileToPartyLeader(*creature)) {
            partyLeader->clearAllActions();
            auto action = _game.newAction<AttackObjectAction>(creature);
            action->setUserAction(true);
            partyLeader->addAction(std::move(action));
        } else if (!creature->conversation().empty()) {
            partyLeader->clearAllActions();
            partyLeader->addAction(_game.newAction<StartConversationAction>(creature, ""));
        }
    }
}

void Module::onDoorClick(const std::shared_ptr<Door> &door) {
    if (!door->linkedToModule().empty() && door->getOnOpen().empty()) {
        std::shared_ptr<Creature> partyLeader(_game.party().getLeader());
        if (door->isLocked()) {
            tryUnlockDoorWithKey(_game, *door, *partyLeader, _game.party());
        }
        if (door->isLocked()) {
            door->onFailToOpen(*partyLeader);
            return;
        }
        _game.scheduleModuleTransition(door->linkedToModule(), door->linkedTo());
        return;
    }
    if (!door->isOpen()) {
        std::shared_ptr<Creature> partyLeader(_game.party().getLeader());
        partyLeader->clearAllActions();
        partyLeader->addAction(_game.newAction<OpenDoorAction>(door));
    }
}

void Module::onPlaceableClick(const std::shared_ptr<Placeable> &placeable) {
    std::shared_ptr<Creature> partyLeader(_game.party().getLeader());

    if (placeable->hasInventory()) {
        partyLeader->clearAllActions();
        if (!placeable->isLocked()) {
            partyLeader->addAction(_game.newAction<OpenContainerAction>(placeable));
        }
    } else if (!placeable->conversation().empty()) {
        partyLeader->clearAllActions();
        partyLeader->addAction(_game.newAction<StartConversationAction>(placeable, ""));
    } else {
        placeable->runOnUsed(std::move(partyLeader));
    }
}

size_t Module::pendingSavedEventCount() const {
    return static_cast<size_t>(std::count(
        _savedEventLive.begin(), _savedEventLive.end(), true));
}

void Module::deserializeSavedEventQueue(
    const resource::Gff &ifo,
    const SerializedIdentityContext &identityContext) {
    _savedEventQueue = SavedEventQueue::fromGff(ifo, identityContext);
    _savedEventLive.clear();
    _savedEventReferencesBound.clear();
    _savedEventLive.reserve(_savedEventQueue.events.size());
    for (const auto &event : _savedEventQueue.events) {
        _savedEventLive.push_back(event.shouldRestore());
    }
    _publishedSavedEvents.clear();
    _savedEventsPublished = false;
}

std::vector<SavedEventRecord> Module::saveEventSnapshot() const {
    // Stable-frame semantic snapshot; byte encoding is deliberately later E3.
    std::vector<SavedEventRecord> result;
    for (size_t index = 0; index < _savedEventQueue.events.size(); ++index) {
        if (index < _savedEventLive.size() && _savedEventLive[index]) {
            result.push_back(_savedEventQueue.events[index]);
        }
    }
    return result;
}

size_t Module::enqueueSaveEvent(SavedEventRecord event) {
    // New events bind through the current B registry before becoming visible to
    // a save snapshot; raw IDs never gain cross-session authority.
    const bool referencesBound = event.bindObjectReferences(_game);
    _savedEventQueue.events.push_back(std::move(event));
    _savedEventLive.push_back(true);
    _savedEventReferencesBound.push_back(referencesBound);
    const auto index = _savedEventQueue.events.size() - 1;
    if (_savedEventsPublished) publishSavedEvent(index);
    return index;
}

size_t Module::enqueueBoundSaveEvent(
    SavedEventRecord event, bool referencesBound) {
    // Ordinary travel captures Party timers while the source registry still
    // owns their reference domain. The record already carries C4 exact-
    // incarnation handles; looking its numeric carriers up again after the
    // destination publishes could alias an unrelated object.
    _savedEventQueue.events.push_back(std::move(event));
    _savedEventLive.push_back(true);
    _savedEventReferencesBound.push_back(referencesBound);
    const auto index = _savedEventQueue.events.size() - 1;
    if (_savedEventsPublished) publishSavedEvent(index);
    return index;
}

bool Module::cancelSaveEvent(size_t index) {
    if (index >= _savedEventLive.size() || !_savedEventLive[index]) {
        return false;
    }
    _savedEventLive[index] = false;
    for (auto &published : _publishedSavedEvents) {
        if (published.savedIndex == index) {
            published.delivered = true;
        }
    }
    return true;
}

void Module::bindSavedEventQueue() {
    _savedEventReferencesBound.clear();
    _savedEventReferencesBound.reserve(_savedEventQueue.events.size());
    for (auto &event : _savedEventQueue.events) {
        _savedEventReferencesBound.push_back(
            !event.shouldRestore() || event.bindObjectReferences(_game));
    }
}

void Module::restoreProjectilePresentations() {
    if (!_projectilesAwaitingRestore) return;
    _projectilesAwaitingRestore = false;
    _services.game.projectiles.restorePresentations(std::move(_savedProjectiles), _game, _services);
    _savedProjectiles.clear();
}
void Module::publishSavedEventQueue() {
    if (_savedEventsPublished) return;
    _publishedSavedEvents.clear();
    for (size_t index = 0; index < _savedEventQueue.events.size(); ++index) publishSavedEvent(index);
    _savedEventsPublished = true;
}

void Module::publishSavedEvent(size_t index) {
    const auto &savedEvent = _savedEventQueue.events[index];
    if (!_savedEventLive[index] || !savedEvent.shouldRestore() ||
        index >= _savedEventReferencesBound.size() || !_savedEventReferencesBound[index] ||
        savedEvent.executionSupport() != SavedExecutionSupport::Executable) return;
    PublishedSavedEvent event;
    event.savedIndex = index;
    event.dueMilliseconds = static_cast<uint64_t>(savedEvent.day) * _game.millisecondsPerWorldDay() + savedEvent.time;
    if (savedEvent.eventId == static_cast<uint32_t>(SavedEventType::Timed)) {
        auto situation = std::get_if<SerializedScriptSituation>(&savedEvent.payload);
        if (!situation) return;
        SavedScriptSituationImporter importer(_game, _services.resource.scripts);
        auto imported = importer.import(*situation);
        if (!imported) {
            warn("Module: preserving unsupported timed event: " + imported.message);
            return;
        }
        event.continuation = std::move(imported.continuation);
    }
    _publishedSavedEvents.push_back(std::move(event));
}

void Module::dispatchDueSavedEvents() {
    publishSavedEventQueue();
    detail::dispatchDueSavedEvents(_publishedSavedEvents, _dispatchingSavedEvents,
        [&](size_t savedIndex) { return _savedEventLive[savedIndex]; },
        [&]() { return _game.worldTimeMilliseconds(); },
        [&](PublishedSavedEvent &event) { deliverSavedEvent(event); });
}

void Module::cancelObjectDestruction(const Object &object) {
    for (size_t index = 0; index < _savedEventQueue.events.size(); ++index) {
        const auto &event = _savedEventQueue.events[index];
        if (event.eventId == static_cast<uint32_t>(SavedEventType::DestroyObject) &&
            event.object.boundObject().get() == &object) _savedEventLive[index] = false;
    }
}

void Module::deliverSavedEvent(PublishedSavedEvent &published) {
    published.delivered = true;
    if (published.savedIndex < _savedEventLive.size()) {
        _savedEventLive[published.savedIndex] = false;
    }
    const auto savedEvent = _savedEventQueue.events[published.savedIndex];
    auto target = savedEvent.object.boundObject();
    if (!target) {
        return;
    }

    switch (static_cast<SavedEventType>(savedEvent.eventId)) {
    case SavedEventType::SpellImpact:
    case SavedEventType::ItemOnHitSpellImpact: {
        if (const auto *hit = std::get_if<SavedWeaponImpact>(&savedEvent.payload)) {
            auto source = hit->source.boundObject();
            auto applications = hit->applications;
            for (auto &application : applications) application.creator = source;
            auto damage = hit->damage;
            damage.restoring = false;
            if (!damage.hasStableId()) damage.id = _game.allocateEffectId();
            target->applyEffect(std::move(damage));
            addDeferredCombatFeedback(_game, _services, source, *target, hit->feedback);
            applyItemOnHitApplications(std::move(applications), *target, _game, _services);
            break;
        }
        const auto *impact = std::get_if<SavedSpellImpact>(&savedEvent.payload);
        if (!impact) break;
        auto caster = impact->caster.boundObject();
        if (!caster) break;
        const auto definition = _services.game.spells.get(static_cast<SpellType>(impact->spellId));
        if (!definition) break;
        Spell spell(*definition);
        spell.impactScript = impact->script;
        auto spellTarget = impact->target.boundObject();
        int casterLevel = impact->casterLevel;
        if (casterLevel < 0) {
            if (const auto *creature = dyn_cast<Creature>(caster.get()))
                casterLevel = creature->spellCasterLevel(spell, !impact->item.isInvalid());
            else casterLevel = std::max(10, 2 * static_cast<int>(spell.innateLevel) - 1);
        }
        SpellCastContext context {impact->spellId, std::max(0, casterLevel), impact->metaMagic, impact->finalForceCost};
        runSpellImpact(_game, spell, *caster, spellTarget.get(),
            std::make_shared<Location>(impact->targetPosition, impact->targetFacing), &context);
        break;
    }
    case SavedEventType::SignalEvent: {
        const auto &event = std::get<SavedScriptEvent>(savedEvent.payload);
        if (event.type == 11 && !event.integers.empty()) {
            _game.scriptRunner().run(target->getOnUserDefined(), {
                {script::ArgKind::Caller, script::Variable::ofObject(target->id())},
                {script::ArgKind::UserDefinedEventNumber, script::Variable::ofInt(event.integers[0])}});
        } else if (event.type == 2 && event.integers.size() >= 2) {
            auto caster = event.objects.empty() ? nullptr : event.objects[0].boundObject();
            _game.scriptRunner().run(target->getOnSpellCastAt(), {
                {script::ArgKind::Caller, script::Variable::ofObject(target->id())},
                {script::ArgKind::LastSpellCaster, script::Variable::ofObject(caster ? caster->id() : script::kObjectInvalid)},
                {script::ArgKind::LastSpell, script::Variable::ofInt(event.integers[0])},
                {script::ArgKind::LastSpellHarmful, script::Variable::ofInt(event.integers[1])}});
        }
        break;
    }
    case SavedEventType::OnMeleeAttacked: {
        if (auto creature = dyn_cast<Creature>(target)) {
            const auto *attack = std::get_if<SavedCombatAttack>(&savedEvent.payload);
            auto caller = savedEvent.caller.boundObject();
            creature->receiveAttackEvent(attack ? attack->history.get() : nullptr,
                caller ? caller->id() : script::kObjectInvalid,
                attack ? attack->fields.get() : nullptr);
        }
        break;
    }
    case SavedEventType::BroadcastSafeProjectile: {
        const auto *attack = std::get_if<SavedCombatAttack>(&savedEvent.payload);
        auto source = std::dynamic_pointer_cast<Creature>(savedEvent.caller.boundObject());
        auto receiver = attack ? attack->reactionObject.boundObject() : nullptr;
        auto weapon = attack ? std::dynamic_pointer_cast<Item>(attack->ammoItem.boundObject()) : nullptr;
        if (source && receiver && weapon && _area &&
            _area->isObjectResident(*source) && _area->isObjectResident(*receiver)) {
            if (attack->fields) _services.game.projectiles.launchSafeProjectile(
                *source, *receiver, *weapon, *attack->fields, _game, _services,
                attack->history ? attack->history->type : 0);
        }
        break;
    }
    case SavedEventType::DestroyObject: {
        if (auto item = dyn_cast<Item>(target)) {
            if (auto owner = _game.getObjectById(item->owner())) {
                if (item->isEquipped()) {
                    if (auto *creature = dyn_cast<Creature>(owner.get())) creature->takeEquippedItem(item);
                } else owner->removeItemStack(item);
            }
        }
        _area->destroyObject(*target);
        break;
    }
    case SavedEventType::Timed:
        if (published.continuation) {
            _game.scriptRunner().run(
                *published.continuation, _game, target->id());
        }
        break;
    case SavedEventType::ApplyEffect: {
        auto savedEffect = std::get_if<EffectInstance>(&savedEvent.payload);
        if (!savedEffect) {
            break;
        }
        EffectInstance effect(*savedEffect);
        effect.restoring = false;
        if (effect.hasStableId()) {
            _game.importEffectId(effect.id);
        } else {
            effect.id = _game.allocateEffectId();
        }
        target->applyEffect(std::move(effect));
        break;
    }
    case SavedEventType::RemoveEffect: {
        auto effect = std::get_if<EffectInstance>(&savedEvent.payload);
        if (effect) {
            target->removeEffectsById(effect->id);
        }
        break;
    }
    default:
        break;
    }
}

void Module::update(float dt) {
    // Process the module object's own action queue so delayed/assigned commands
    // scheduled by module scripts (e.g. Mod_OnModLoad) execute. Without this the
    // module is never ticked and its DelayCommand continuations never run.
    Object::update(dt);
    dispatchDueSavedEvents();

    if (_game.cameraType() == CameraType::ThirdPerson) {
        _player->update(dt);
    }
    _area->update(dt);
    dispatchDueSavedEvents();
}

std::vector<ContextAction> Module::getContextActions(const std::shared_ptr<Object> &object) const {
    std::vector<ContextAction> actions;

    switch (object->type()) {
    case ObjectType::Creature: {
        auto leader = _game.party().getLeader();
        auto creature = std::static_pointer_cast<Creature>(object);
        if (isHostileToPartyLeader(*creature)) {
            actions.push_back(ContextAction(ActionType::AttackObject));
            auto weapon = leader->getEquippedItem(InventorySlots::rightWeapon);
            if (weapon && weapon->isRanged()) {
                if (leader->hasEffectiveFeat(FeatType::MasterPowerBlast)) {
                    actions.push_back(ContextAction(FeatType::MasterPowerBlast));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedPowerBlast)) {
                    actions.push_back(ContextAction(FeatType::ImprovedPowerBlast));
                } else if (leader->hasEffectiveFeat(FeatType::PowerBlast)) {
                    actions.push_back(ContextAction(FeatType::PowerBlast));
                }
                if (leader->hasEffectiveFeat(FeatType::MasterSniperShot)) {
                    actions.push_back(ContextAction(FeatType::MasterSniperShot));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedSniperShot)) {
                    actions.push_back(ContextAction(FeatType::ImprovedSniperShot));
                } else if (leader->hasEffectiveFeat(FeatType::SniperShot)) {
                    actions.push_back(ContextAction(FeatType::SniperShot));
                }
                if (leader->hasEffectiveFeat(FeatType::MultiShot)) {
                    actions.push_back(ContextAction(FeatType::MultiShot));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedRapidShot)) {
                    actions.push_back(ContextAction(FeatType::ImprovedRapidShot));
                } else if (leader->hasEffectiveFeat(FeatType::RapidShot)) {
                    actions.push_back(ContextAction(FeatType::RapidShot));
                }
            } else {
                if (leader->hasEffectiveFeat(FeatType::MasterPowerAttack)) {
                    actions.push_back(ContextAction(FeatType::MasterPowerAttack));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedPowerAttack)) {
                    actions.push_back(ContextAction(FeatType::ImprovedPowerAttack));
                } else if (leader->hasEffectiveFeat(FeatType::PowerAttack)) {
                    actions.push_back(ContextAction(FeatType::PowerAttack));
                }
                if (leader->hasEffectiveFeat(FeatType::MasterCriticalStrike)) {
                    actions.push_back(ContextAction(FeatType::MasterCriticalStrike));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedCriticalStrike)) {
                    actions.push_back(ContextAction(FeatType::ImprovedCriticalStrike));
                } else if (leader->hasEffectiveFeat(FeatType::CriticalStrike)) {
                    actions.push_back(ContextAction(FeatType::CriticalStrike));
                }
                if (leader->hasEffectiveFeat(FeatType::WhirlwindAttack)) {
                    actions.push_back(ContextAction(FeatType::WhirlwindAttack));
                } else if (leader->hasEffectiveFeat(FeatType::ImprovedFlurry)) {
                    actions.push_back(ContextAction(FeatType::ImprovedFlurry));
                } else if (leader->hasEffectiveFeat(FeatType::Flurry)) {
                    actions.push_back(ContextAction(FeatType::Flurry));
                }
            }

            auto &itemAttrs = _game.party().getLeader()->itemAttributes();
            for (const auto &[item, spell] : itemAttrs.attackingSpells()) {
                actions.push_back(ContextAction(item, spell));
            }
        }
        break;
    }
    case ObjectType::Door: {
        auto door = std::static_pointer_cast<Door>(object);
        auto leader = _game.party().getLeader();
        if (canBashDoor(*door, *leader, _services.game.reputes)) {
            actions.push_back(ContextAction(ActionType::AttackObject));
        }
        if (door->isLocked() && !door->isKeyRequired() && leader->attributes().hasSkill(SkillType::Security)) {
            actions.push_back(ContextAction(SkillType::Security));
        }
        break;
    }
    case ObjectType::Placeable: {
        auto placeable = cast<Placeable>(object);
        auto leader = _game.party().getLeader();
        if (canBashPlaceable(*placeable, *leader, _services.game.reputes)) {
            actions.push_back(ContextAction(ActionType::AttackObject));
        }
        if (canUseSecurityOnPlaceable(*placeable, *leader)) {
            actions.push_back(ContextAction(SkillType::Security));
        }
        break;
    }
    default:
        break;
    }

    return actions;
}

bool Module::handleKeyDown(const input::KeyEvent &event) {
    switch (event.code) {
    case input::KeyCode::Space: {
        bool paused = !_game.isPaused();
        _game.setPaused(paused);
        return true;
    }
    default:
        return false;
    }
}

} // namespace game

} // namespace reone
