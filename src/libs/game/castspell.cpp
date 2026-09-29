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

#include <algorithm>

#include <glm/geometric.hpp>

#include "reone/audio/mixer.h"
#include "reone/game/location.h"
#include "reone/game/d20/class.h"
#include "reone/graphics/model.h"
#include "reone/resource/provider/models.h"
#include "reone/scene/graph.h"
#include "reone/game/d20/spell.h"
#include "reone/game/castspell.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/item.h"
#include "reone/game/object/placeable.h"
#include "reone/game/party.h"
#include "reone/scene/node/model.h"
#include "reone/game/object/module.h"
#include "reone/game/forcerules.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/system/exception/validation.h"

namespace reone {
namespace game {

static constexpr int kCatchAnimationId = 10161;

SpellType itemSpellForActor(const Object &actor, SpellType propertySpell) {
    const auto *creature = dyn_cast<Creature>(&actor);
    if (!actor.game().isTSL() || !creature) return propertySpell;
    bool launcher = false;
    for (const int slot : {InventorySlots::rightArm, InventorySlots::leftArm}) {
        const auto it = creature->equipment().find(slot);
        if (it != creature->equipment().end() && it->second->itemType() == 48) {
            launcher = true;
            break;
        }
    }
    if (!launcher) return propertySpell;
    switch (static_cast<int>(propertySpell)) {
    case 87: return static_cast<SpellType>(246);
    case 88: return static_cast<SpellType>(247);
    case 89: return static_cast<SpellType>(248);
    case 90: return static_cast<SpellType>(249);
    case 91: return static_cast<SpellType>(250);
    case 92: return static_cast<SpellType>(251);
    case 93: return static_cast<SpellType>(252);
    case 94: return static_cast<SpellType>(253);
    case 95: return static_cast<SpellType>(254);
    case 186: return static_cast<SpellType>(255);
    case 187: return static_cast<SpellType>(256);
    default: return propertySpell;
    }
}

void publishItemCastLevel(Creature &caster) {
    // Item execution publishes the undrained last-class byte, independently
    // of both the property's caster level and the ordinary spell selector.
    const auto &classes = caster.attributes().classLevels();
    const uint8_t count = static_cast<uint8_t>(classes.size());
    const uint8_t index = static_cast<uint8_t>(count - 1);
    caster.spellScriptContext().levelOverride = index < count
        ? static_cast<uint8_t>(classes[index].second) : 0;
}

int queryCasterLevel(const Object &object) {
    const Object *owner = &object;
    std::shared_ptr<Object> areaEffectCreator;
    int spellId = object.spellCastContext().spellId;
    if (object.type() == ObjectType::AreaOfEffect) {
        areaEffectCreator = object.effectSource();
        if (!areaEffectCreator) return 0;
        owner = areaEffectCreator.get();
        spellId = static_cast<int>(object.effectSpellId());
    }
    if (const auto creature = dyn_cast<Creature>(owner)) {
        if (owner->spellScriptContext().levelOverride)
            return *owner->spellScriptContext().levelOverride;
        const int clazz = owner->spellCastContext().castingClass;
        switch (castingSourceKind(clazz)) {
        case CastingSourceKind::SpellLikeAbility:
            return creature->spellLikeAbilityCasterLevel(spellId);
        case CastingSourceKind::Class:
            return creature->adjustedClassLevel(clazz);
        case CastingSourceKind::UnselectedClass:
            break;
        }
    } else if (!isa<Placeable>(owner)) {
        return 0;
    }
    // The unselected-class/placeable branch reads the owner's current spell,
    // including when the query was made for a created area effect.
    const auto spell = owner->services().game.spells.get(
        static_cast<SpellType>(owner->spellCastContext().spellId));
    return spell ? std::max(10, 2 * static_cast<int>(spell->innateLevel) - 1) : 0;
}

void SpellScriptContext::setImpact(
    const SpellCastContext &context, const std::shared_ptr<Object> &target,
    const std::shared_ptr<Object> &item, const Location *location) {
    cast = context;
    _target = target;
    _item = item;
    _location = location ? std::make_shared<Location>(location->position(), location->facing()) : nullptr;
}

std::shared_ptr<Object> SpellScriptContext::target() const { return _target.resolve(); }
std::shared_ptr<Object> SpellScriptContext::item() const { return _item.resolve(); }
std::shared_ptr<Location> SpellScriptContext::location() const {
    return _location ? std::make_shared<Location>(_location->position(), _location->facing()) : nullptr;
}
std::shared_ptr<Object> SpellScriptContext::activeTarget() const { return _activeTarget.resolve(); }
void SpellScriptContext::setActiveTarget(const std::shared_ptr<Object> &target) { _activeTarget = target; }
void SpellScriptContext::clearActiveTarget() { _activeTarget.reset(); }

void SpellScriptContext::restore(const resource::Gff *state) {
    *this = SpellScriptContext {};
    if (!state) return;
    cast.spellId = state->getInt("SpellId", -1);
    cast.casterLevel = state->getInt("CasterLevel");
    cast.metaMagic = state->getInt("MetaMagic", 255);
    cast.forcePointCost = state->getInt("ForceCost");
    uint8_t encodedClass;
    if (!state->readByte(encodedClass, "CastingClass"))
        throw ValidationException("SpellContext has no CastingClass");
    cast.castingClass = encodedClass;
    if (state->getBool("OverrideActive")) levelOverride = state->getInt("LevelOverride");
    _savedIds = {{state->getUint("TargetId", kSavedRuntimeInvalidObjectId),
        state->getUint("ItemId", kSavedRuntimeInvalidObjectId),
        state->getUint("ActiveTarget", kSavedRuntimeInvalidObjectId)}};
    if (state->getBool("HasLocation")) {
        const glm::vec3 position(state->getFloat("X"), state->getFloat("Y"), state->getFloat("Z"));
        const float facing = state->getFloat("Facing");
        if (std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) && std::isfinite(facing))
            _location = std::make_shared<Location>(position, facing);
    }
    _needsBind = true;
    _restored = true;
}

void SpellScriptContext::bind(
    const std::function<std::shared_ptr<Object>(uint32_t)> &resolver) {
    if (!_needsBind) return;
    RuntimeObjectRef<Object> *references[] = {&_target, &_item, &_activeTarget};
    for (size_t index = 0; index < _savedIds.size(); ++index) {
        *references[index] = _savedIds[index] == kSavedRuntimeInvalidObjectId
            ? nullptr : resolver(_savedIds[index]);
    }
    _needsBind = false;
}

std::shared_ptr<resource::Gff> SpellScriptContext::save(
    const std::function<uint32_t(const Object &)> &objectId) const {
    if (cast.spellId < 0 && !_location && _target.empty() && _item.empty() &&
        _activeTarget.empty() && !_restored) return nullptr;
    const auto id = [&](const RuntimeObjectRef<Object> &reference) {
        const auto object = reference.resolve();
        return object ? objectId(*object) : kSavedRuntimeInvalidObjectId;
    };
    using resource::Gff;
    auto result = Gff::Builder()
        .field(Gff::Field::newInt("SpellId", cast.spellId))
        .field(Gff::Field::newInt("CasterLevel", cast.casterLevel))
        .field(Gff::Field::newInt("MetaMagic", cast.metaMagic))
        .field(Gff::Field::newInt("ForceCost", cast.forcePointCost))
        .field(Gff::Field::newByte("CastingClass", static_cast<uint8_t>(cast.castingClass)))
        .field(Gff::Field::newByte("OverrideActive", levelOverride.has_value()))
        .field(Gff::Field::newInt("LevelOverride", levelOverride.value_or(0)))
        .field(Gff::Field::newDword("TargetId", id(_target)))
        .field(Gff::Field::newDword("ItemId", id(_item)))
        .field(Gff::Field::newDword("ActiveTarget", id(_activeTarget)))
        .field(Gff::Field::newByte("HasLocation", _location != nullptr));
    if (_location) {
        result.field(Gff::Field::newFloat("X", _location->position().x))
            .field(Gff::Field::newFloat("Y", _location->position().y))
            .field(Gff::Field::newFloat("Z", _location->position().z))
            .field(Gff::Field::newFloat("Facing", _location->facing()));
    }
    return result.build();
}

void SpellScriptContext::retireAreaRuntime(const std::set<const Object *> &retainedObjects) {
    _needsBind = false;
    _savedIds.fill(kSavedRuntimeInvalidObjectId);
    for (auto *reference : {&_target, &_item, &_activeTarget}) {
        const auto object = reference->resolve();
        if (!object || retainedObjects.count(object.get()) == 0) reference->reset();
    }
}

SpellSchedule::State SpellSchedule::update(
    const CombatRound &round, Action &action, float dt) {
    _previousTime = _time;
    // Once started, a cast keeps its own time through any round pause.
    if (_state == SpellSchedule::WaitConjure && round.suspends(action)) return _state;
    return update(round.canExecute(action), dt);
}

SpellSchedule::State SpellSchedule::update(bool canStart, float dt) {
    _previousTime = _time;
    _time += std::max(0.0f, dt);

    switch (_state) {
    case SpellSchedule::WaitConjure: {
        if (canStart) {
            _time = _previousTime = 0.0f;
            _state = SpellSchedule::Conjure;
        }
        break;
    }
    case SpellSchedule::Conjure: {
        _state = SpellSchedule::WaitCast;
        break;
    }

    case SpellSchedule::WaitCast: {
        if (_time >= _conjTime) {
            _state = SpellSchedule::Cast;
        }
        break;
    }
    case SpellSchedule::Cast: {
        _state = SpellSchedule::WaitEffect;
        break;
    }
    case SpellSchedule::WaitEffect: {
        if (_time >= (_castTime + _conjTime)) {
            _state = SpellSchedule::Effect;
        }
        break;
    }
    case SpellSchedule::Effect: {
        _state = SpellSchedule::WaitFinish;
        break;
    }
    case SpellSchedule::WaitFinish: {
        // The cast ends as its cast time runs out, caught or not; the catch
        // only lengthens the round pause.
        if (_time >= _conjTime + _castTime) {
            _state = SpellSchedule::Finish;
        }
        break;
    }
    case SpellSchedule::Finish: {
        break;
    }
    }

    return _state;
}

void SpellSchedule::save(SavedCastAction &record) const {
    record.phase = _state; record.elapsed = _time;
    record.conjureTime = _conjTime; record.castTime = _castTime; record.catchTime = _catchTime;
}
void SpellSchedule::restore(const SavedCastAction &record) {
    _state = static_cast<State>(record.phase); _time = _previousTime = record.elapsed;
    _conjTime = record.conjureTime; _castTime = record.castTime; _catchTime = record.catchTime;
    // Entry phases have already run before a snapshot is taken.
    if (_state == Conjure) _state = WaitCast;
    else if (_state == Cast) _state = WaitEffect;
    else if (_state == Effect) _state = WaitFinish;
}

static CastPresentation castClips(SpellCastAnimation code, bool creatureModel) {
    if (creatureModel) {
        switch (code) {
        case SpellCastAnimation::Throw: return {};
        case SpellCastAnimation::Up: return {"b0a1", "", false};
        case SpellCastAnimation::MonsterFury: return {"g0a1", "creadyrtw", true};
        default: return {"g0a1", "", false};
        }
    }
    switch (code) {
    case SpellCastAnimation::Self: return {"castout1", "castoutlp1", true};
    case SpellCastAnimation::Dark: return {"castout2", "castoutlp2", true};
    case SpellCastAnimation::Up: return {"castout3", "castoutlp3", true};
    case SpellCastAnimation::Throw: return {"throwsab", "throwsablp", true};
    case SpellCastAnimation::Area:
    case SpellCastAnimation::Jump:
    case SpellCastAnimation::Monster: return {"castout1", "", false};
    case SpellCastAnimation::Fury: return {"castout1", "forcerage", false};
    case SpellCastAnimation::Crush: return {"castout1", "forcecrush", false};
    case SpellCastAnimation::MonsterFury: return {"castout1", "creadyrtw", true};
    default: return {"castout1", "throwsablp", true};
    }
}

// The conjure's and the cast's animation IDs follow the code alone.
CastPresentation castPresentation(SpellCastAnimation code, bool creatureModel) {
    CastPresentation look = castClips(code, creatureModel);
    switch (code) {
    case SpellCastAnimation::Dark: look.conjureId = 10016; break;
    case SpellCastAnimation::Up: look.conjureId = 11000; break;
    case SpellCastAnimation::Throw: look.conjureId = 10162; break;
    default: look.conjureId = 10015; break;
    }
    switch (code) {
    case SpellCastAnimation::Self: look.castId = 10017; break;
    case SpellCastAnimation::Dark: look.castId = 10018; break;
    case SpellCastAnimation::Up: look.castId = 10019; break;
    case SpellCastAnimation::Area: look.castId = 10020; break;
    case SpellCastAnimation::Jump:
    case SpellCastAnimation::Monster: look.castId = -1; break;
    case SpellCastAnimation::Fury: look.castId = 10414; break;
    case SpellCastAnimation::Crush: look.castId = 10415; break;
    case SpellCastAnimation::MonsterFury: look.castId = 279; break;
    default: look.castId = 10061; break;
    }
    return look;
}

// A wrist launcher worn on either arm.
static bool wearsWristLauncher(const Creature &user) {
    for (const int slot : {InventorySlots::rightArm, InventorySlots::leftArm}) {
        const auto it = user.equipment().find(slot);
        if (it != user.equipment().end() && it->second && it->second->itemType() == 48) return true;
    }
    return false;
}

ItemUsePresentation itemUsePresentation(const Creature &user, int itemType, const Spell &spell,
                                        const Creature *targetCreature, const glm::vec3 &targetPosition) {
    const bool tsl = user.game().isTSL();
    // Only character and droid models use the item clips; others stand in their pose.
    const bool bodied = user.modelType() != Creature::ModelType::Creature;
    ItemUsePresentation look;
    switch (itemType) {
    case 6:   // grenades
    case 49:  // rockets
        look.impact = 0.8f;
        if (!bodied) { look.hold = false; break; }
        if (tsl && wearsWristLauncher(user)) {
            look.clip = "b11a3";
            look.clipId = 10417;
            look.impact = 0.95f;
        } else if (glm::dot(targetPosition - user.position(), targetPosition - user.position()) < 10.0f * 10.0f) {
            look.clip = "throwgren1";
            look.clipId = 10130;
            look.impact = 0.7f;
        } else {
            look.clip = "throwgren";
            look.clipId = 10129;
        }
        break;
    case 25:  // stims
    case 26:  // droid repair
    case 45:  // medical
        look.onTarget = itemType != 26;
        look.impact = 0.75f;
        if (!bodied) { look.hold = false; break; }
        if (!tsl || targetCreature == &user) {
            look.clip = "inject";
            look.clipId = 10070;
        } else {
            look.clip = "throwsablp";
            look.clipId = 10061;
            look.loops = true;
        }
        break;
    case 47:  // squad recovery kit
        look.onTarget = true;
        look.impact = 0.75f;
        if (!bodied) { look.hold = false; break; }
        look.clip = "activate";
        look.clipId = 10136;
        break;
    case 20:  // forearm bands
        look.impact = 0.6f;
        look.end = 1.0f;
        if (!bodied) { look.hold = false; break; }
        look.clip = "activate";
        look.clipId = 10136;
        break;
    case 12: { // droid utility: its conjure, then its loop; it takes effect as the loop starts
        look.clip = "castout1";
        look.clipId = 11001;
        look.loopAt = 0.3f;
        auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(user.sceneNode());
        if (model) {
            if (auto conjure = model->model().getAnimation("castout1")) look.loopAt = conjure->length() - 0.05f;
        }
        look.impact = look.loopAt;
        break;
    }
    default:
        look.clip = "castoutlp1";
        look.clipId = 10017;
        look.loops = true;
        look.end = spell.castTime + 0.001f;
        break;
    }
    return look;
}

// The dead show their own pose.
static bool isPoseable(const Creature &creature) {
    return !creature.isDead() && !creature.isTemporarilyDead();
}

const ItemUsePresentation &CastPresenter::itemUse(const Creature &user, const Spell &spell, int itemType,
                                                  const Creature *targetCreature, const glm::vec3 &targetPosition) {
    _item = itemUsePresentation(user, itemType, spell, targetCreature, targetPosition);
    return *_item;
}

bool interruptEntangledCast(Creature &caster) {
    if (!caster.hasEffect(EffectType::Entangle)) return false;
    caster.showPauseReadyAnimation(false);
    caster.setOrientationLock(script::kObjectInvalid);
    return true;
}

// The visuals hang from the head, the hand and the feet of the caster's body.
static constexpr std::array<const char *, 3> kCastVisualHooks {"headconjure", "handconjure", "root"};
// After the release, each set lasts this long in turn: the conjure visuals, then the cast visuals.
static constexpr float kCastVisualSeconds = 5.0f;

SpellCastVisuals::~SpellCastVisuals() {
    detach(_conjure);
    detach(_cast);
}

void SpellCastVisuals::attach(Set &set, Creature &caster, const std::array<std::string, 3> &models,
                              const char *animation) {
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(caster.sceneNode());
    if (!body) return;
    for (size_t index = 0; index < set.size(); ++index) {
        if (models[index].empty()) continue;
        auto model = caster.services().resource.models.get(models[index]);
        auto *hook = body->getNodeByName(kCastVisualHooks[index]);
        if (!model || !hook) continue;
        auto &attached = set[index];
        attached.body = body;
        attached.hook = hook;
        attached.model = body->graph().newModel(*model, scene::ModelUsage::Projectile);
        hook->addChild(*attached.model);
        attached.model->playAnimation(animation);
    }
}

void SpellCastVisuals::detach(Attached &attached) {
    if (attached.model && attached.hook && !attached.body.expired()) attached.hook->removeChild(*attached.model);
    attached = Attached {};
}

void SpellCastVisuals::detach(Set &set) {
    for (auto &attached : set) detach(attached);
}

void SpellCastVisuals::showConjure(Creature &caster, const Spell &spell) {
    detach(_conjure);
    detach(_cast);
    attach(_conjure, caster, spell.conjureVisuals, "conjure01");
    _released = false;
    _conjureRemaining = _castRemaining = kCastVisualSeconds;
}

void SpellCastVisuals::showCast(Creature &caster, const Spell &spell) {
    detach(_cast);
    attach(_cast, caster, spell.castVisuals, "cast01");
    // The conjure visuals fade out; one without a fade stays until its time runs out.
    for (auto &attached : _conjure) {
        if (!attached.model || !attached.model->model().getAnimation("fade")) continue;
        attached.model->playAnimation("fade");
        attached.fading = true;
    }
    _released = true;
}

void SpellCastVisuals::clear() {
    detach(_conjure);
    detach(_cast);
    _released = false;
}

void SpellCastVisuals::update(float dt) {
    // A faded conjure visual is gone once its fade has played.
    for (auto &attached : _conjure)
        if (attached.fading && attached.model->isAnimationFinished()) detach(attached);
    if (!_released) return;
    if (_conjureRemaining > 0.0f) {
        _conjureRemaining -= dt;
        if (_conjureRemaining <= 0.0f) {
            detach(_conjure);
            _conjureRemaining = 0.0f;
        }
    } else if (_castRemaining > 0.0f) {
        _castRemaining -= dt;
        if (_castRemaining <= 0.0f) {
            detach(_cast);
            _castRemaining = 0.0f;
            _released = false;
        }
    } else {
        _released = false;
    }
}

void CastPresenter::update(Creature &caster, const Spell &spell, std::optional<int> itemType, Creature *targetCreature,
                           const glm::vec3 &targetPosition, const SpellSchedule &schedule, SpellSchedule::State state) {
    if (itemType) {
        if (!_item) _item = itemUsePresentation(caster, *itemType, spell, targetCreature, targetPosition);
        updateItem(caster, targetCreature, schedule, state);
        return;
    }
    // Nothing shows before the cast starts.
    if (state == SpellSchedule::WaitConjure) return;
    const auto look = castPresentation(spell.castAnimation, caster.modelType() == Creature::ModelType::Creature);
    if (state == SpellSchedule::Conjure || state == SpellSchedule::WaitCast) {
        caster.holdCastAnimation();
    } else if (state == SpellSchedule::Cast || state == SpellSchedule::WaitEffect) {
        caster.holdCastAnimation(look.castLoops ? look.cast : std::string());
    } else if (state == SpellSchedule::Effect && spell.catchTime > 0.0f) {
        // A caught cast plays its catch, and its cast clip stays until the
        // round pause end poses the caster.
        caster.holdCastAnimation(look.castLoops ? look.cast : std::string());
        caster.playFireForgetAnimation("catchsab", AnimationSource {kCatchAnimationId});
    }
    // The conjure and a one-shot cast clip wait in the caster's queue, over
    // the idle the hold gives; a cast loop arriving meanwhile waits for them.
    if (state == SpellSchedule::Conjure && spell.conjTime > 0.0f && !look.conjure.empty())
        caster.playFireForgetAnimation(look.conjure, AnimationSource {look.conjureId});
    if (state == SpellSchedule::Cast && !look.castLoops && !look.cast.empty())
        caster.playFireForgetAnimation(look.cast, AnimationSource {look.castId});
    // Without a catch the cast ends in the pose as its time runs out; with one,
    // the round pause end poses the caster.
    if (spell.catchTime <= 0.0f && state == SpellSchedule::Effect && isPoseable(caster))
        caster.showPauseReadyAnimation(true);
}

void CastPresenter::updateItem(Creature &user, Creature *targetCreature,
                               const SpellSchedule &schedule, SpellSchedule::State state) {
    if (state == SpellSchedule::WaitConjure) return;
    const auto &look = *_item;
    const float time = schedule.time();
    if (time < look.end) {
        if (look.onTarget && targetCreature && targetCreature != &user) {
            // The clip, or the pose, is drawn on the creature it is used on;
            // the user keeps its own.
            if (state == SpellSchedule::Conjure) {
                if (!look.hold) {
                    if (isPoseable(*targetCreature)) targetCreature->showPauseReadyAnimation(false);
                } else if (look.loops) {
                    targetCreature->playAnimation(look.clip, scene::AnimationProperties::fromFlags(
                        scene::AnimationFlags::loop));
                } else {
                    targetCreature->markAnimationChosen();
                    targetCreature->playFireForgetAnimation(look.clip, AnimationSource {look.clipId});
                }
            }
        } else if (!look.hold) {
            if (state == SpellSchedule::Conjure && isPoseable(user)) user.showPauseReadyAnimation(false);
        } else {
            if (look.loopAt >= 0.0f && time >= look.loopAt) user.holdCastAnimation("castoutlp1");
            else user.holdCastAnimation(look.loops ? look.clip : std::string());
            if (state == SpellSchedule::Conjure && !look.loops)
                user.playFireForgetAnimation(look.clip, AnimationSource {look.clipId});
        }
    }
    if (schedule.reached(look.end) && isPoseable(user)) user.showPauseReadyAnimation(true);
}

// A restored cast shows its loop again at once. Its one-shot shows again only
// for a party member, which comes back without a clip of its own, so the
// one-shot the cast is still writing arrives anew and plays from its start;
// any other creature comes back already showing it and returns to its loop at
// once.
void CastPresenter::restore(Creature &caster, const Spell &spell, std::optional<int> itemType, Creature *targetCreature,
                            const glm::vec3 &targetPosition, const SpellSchedule &schedule) {
    const float time = schedule.time();
    const bool replays = caster.game().party().isMember(caster);
    if (itemType) {
        _item = itemUsePresentation(caster, *itemType, spell, targetCreature, targetPosition);
        const auto &look = *_item;
        if (time >= look.end || !look.hold || (look.onTarget && targetCreature && targetCreature != &caster)) return;
        if (look.loopAt >= 0.0f && time >= look.loopAt) {
            caster.holdCastAnimation("castoutlp1");
            return;
        }
        caster.holdCastAnimation(look.loops ? look.clip : std::string());
        // The clip is written until the item takes effect.
        if (!look.loops && replays && time < look.impact)
            caster.playFireForgetAnimation(look.clip, AnimationSource {look.clipId});
        return;
    }
    const auto look = castPresentation(spell.castAnimation, caster.modelType() == Creature::ModelType::Creature);
    const float castEnd = spell.conjTime + spell.castTime;
    if (time < spell.conjTime) {
        caster.holdCastAnimation();
        if (!look.conjure.empty() && replays) caster.playFireForgetAnimation(look.conjure, AnimationSource {look.conjureId});
    } else if (time < castEnd) {
        caster.holdCastAnimation(look.castLoops ? look.cast : std::string());
        if (!look.castLoops && !look.cast.empty() && replays)
            caster.playFireForgetAnimation(look.cast, AnimationSource {look.castId});
    } else if (spell.catchTime > 0.0f) {
        // Caught: the cast clip stays until the round pause end poses the caster.
        caster.holdCastAnimation(look.castLoops ? look.cast : std::string());
        if (time < castEnd + spell.catchTime && replays)
            caster.playFireForgetAnimation("catchsab", AnimationSource {kCatchAnimationId});
    }
}

ProjectilePathType normalizeProjectilePath(ProjectilePathType path) {
    switch (path) {
    case ProjectilePathType::Default: case ProjectilePathType::Homing:
    case ProjectilePathType::Ballistic: case ProjectilePathType::HighBallistic:
    case ProjectilePathType::Accelerating: case ProjectilePathType::Spiral:
    case ProjectilePathType::Linked: case ProjectilePathType::Bounce:
    case ProjectilePathType::Burst: case ProjectilePathType::Grenade: return path;
    default: return ProjectilePathType::Default;
    }
}
std::optional<ProjectilePathType> projectilePathFromScript(int value) {
    switch (value) {
    case 0: return ProjectilePathType::Default;
    case 1: return ProjectilePathType::Homing;
    case 2: return ProjectilePathType::Ballistic;
    case 3: return ProjectilePathType::HighBallistic;
    case 4: return ProjectilePathType::Accelerating;
    default: return std::nullopt;
    }
}

std::optional<SpellSelection> scriptCastingSource(const Creature &caster, const Spell &spell) {
    const auto &classes = caster.attributes().classLevels();
    // A third class never casts for a script command; the spell-like ability is tried instead.
    for (size_t index = 0; index < classes.size() && index < 2; ++index) {
        const ClassType clazz = classes[index].first->type();
        if (!isForceUsingClass(clazz, caster.game().isTSL()) || !caster.hasSpellUsesLeft(spell, index)) continue;
        const auto required = spell.getClassLevelRequirement(clazz);
        if (required && *required >= 0 && *required != 0xff)
            return SpellSelection {static_cast<int>(index), kUnspecifiedCasterLevel};
    }
    if (!caster.knowsSpellLikeAbility(spell.type)) return std::nullopt;
    return SpellSelection {kSpellLikeAbilityClass, caster.spellLikeAbilityCasterLevel(static_cast<int>(spell.type))};
}

void presentSpellRelease(Object &caster, const Spell &spell) {
    if (spell.castSound)
        caster.services().audio.mixer.play(spell.castSound, audio::AudioType::Sound, 1.0f, false, caster.position());
    if (auto *creature = dyn_cast<Creature>(&caster)) creature->spellCastVisuals().showCast(*creature, spell);
}

ProjectilePathType effectiveProjectilePath(const Spell &spell, ProjectilePathType path) {
    return path == ProjectilePathType::Default ? spell.projectilePath : normalizeProjectilePath(path);
}
uint32_t spellProjectileTimeMilliseconds(
    const Spell &spell, const glm::vec3 &origin, const glm::vec3 &destination,
    ProjectilePathType overridePath, bool tsl) {
    if (!spell.projectile) return 0;
    const float distance = glm::distance(origin, destination);
    // Use single-precision logarithms in KotOR. TSL uses a double-precision
    // logarithm narrowed before the multiply/add.
    const float logarithm = tsl ? static_cast<float>(std::log(static_cast<double>(distance)))
                                : std::log(distance);
    float speed = logarithm * 3.0f + 2.0f;
    const auto path = effectiveProjectilePath(spell, overridePath);
    if (path == ProjectilePathType::HighBallistic) return 2000;
    if (path == ProjectilePathType::Homing) speed *= 2.0f;
    else if (path == ProjectilePathType::Accelerating) speed *= 1.5f;
    else if (path == ProjectilePathType::Linked) speed = distance * 0.5f;
    else if (path == ProjectilePathType::Bounce) speed *= 0.4f;
    if (speed <= 0.0f) return 1;

    const float travel = distance / speed * 1000.0f;
    uint32_t milliseconds = 0;
    if (tsl) {
        // TSL truncates to a signed 64-bit integer and stores its low word.
        // Invalid conversions yield INT64_MIN, whose low word is zero.
        if (std::isfinite(travel) && travel >= 0.0f &&
            static_cast<double>(travel) < 9223372036854775808.0)
            milliseconds = static_cast<uint32_t>(static_cast<uint64_t>(travel));
    } else if (travel > 0.0f) {
        // KotOR uses a saturating unsigned conversion; NaN becomes zero.
        milliseconds = static_cast<double>(travel) >= 4294967296.0
            ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(travel);
    }
    // This uses the authored spell path, not the override. Neither early
    // return above passes through the Spiral adjustment.
    if (spell.projectilePath == ProjectilePathType::Spiral) milliseconds += 2500;
    return milliseconds;
}

float spellProjectileTime(const Spell &spell, const glm::vec3 &origin,
                          const glm::vec3 &destination, ProjectilePathType overridePath,
                          bool tsl) {
    return spellProjectileTimeMilliseconds(spell, origin, destination, overridePath, tsl) / 1000.0f;
}

bool equipmentAllowsSpell(const Creature &caster, const Spell &spell) {
    const auto mask = caster.forceItemMask();
    return !spell.isForbiddenByEquipment(mask) && spell.hasRequiredEquipment(mask);
}

bool admitSpellCast(const Object &actor, const Spell &spell, bool freeCast, bool itemCast,
                    std::optional<SpellSelection> selection) {
    const auto reject = [&](const char *reason) {
        debug(str(boost::format("Spell rejected: actor=%u spell=%d reason=%s")
            % actor.id() % static_cast<int>(spell.type) % reason), LogChannel::Combat);
        return false;
    };
    if (actor.isDead()) return reject("dead caster");
    const auto *creature = dyn_cast<Creature>(&actor);
    if (!creature) return isa<Placeable>(&actor) || reject("unsupported caster");
    if (!creature->canCastSpells()) return reject("caster cannot act");
    if (!itemCast && !equipmentAllowsSpell(*creature, spell)) return reject("equipment disallows power");
    if (freeCast) return true;
    if (selection) {
        switch (selection->sourceKind()) {
        case CastingSourceKind::UnselectedClass:
            return true;
        case CastingSourceKind::Class:
            return creature->hasSpellUsesLeft(spell, selection->classIndex) || reject("insufficient spell uses");
        case CastingSourceKind::SpellLikeAbility: {
            int level = 0;
            return creature->readySpellLikeAbility(spell.type, level) || reject("ability not ready");
        }
        }
    }
    if (creature->spellCasterLevel(spell) >= 0 && creature->attributes().hasSpell(spell.type))
        return creature->canPaySpellForcePointCost(spell) || reject("insufficient resources");
    int level = 0;
    return creature->readySpellLikeAbility(spell.type, level) || reject("power not available to caster");
}

float spellRange(const Object &actor, const Spell &spell, const Object *target) {
    float range = spell.range;
    if (const auto *creature = dyn_cast<Creature>(&actor)) range += creature->creaturePersonalSpace() - 0.1f;
    if (const auto *creature = target ? dyn_cast<Creature>(target) : nullptr) range += creature->creaturePersonalSpace() - 0.1f;
    return std::max(0.0f, range);
}

bool withinSpellRange(const Object &actor, const Spell &spell, const glm::vec3 &position, const Object *target) {
    return &actor == target || glm::distance(glm::vec2(actor.position()), glm::vec2(position)) <= spellRange(actor, spell, target);
}

// A cast starts from its spell with no metamagic (these titles offer no NWN
// metamagic or domain spell slots), no cost and no selected class.
static void beginSpellCastContext(SpellCastContext &context, const Spell &spell) {
    context.spellId = static_cast<int>(spell.type);
    context.metaMagic = 0;
    context.forcePointCost = 0;
    context.castingClass = kUnselectedCastingClass;
}

bool commitSpellCast(Object &actor, const Spell &spell, bool freeCast,
                     SpellCastContext &context, bool itemCast, std::optional<SpellSelection> selection) {
    if (!admitSpellCast(actor, spell, freeCast, itemCast, selection)) return false;
    beginSpellCastContext(context, spell);
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        context.casterLevel = creature->spellCasterLevel(spell, freeCast);
        if (!freeCast && selection) {
            context.castingClass = selection->classIndex;
            switch (selection->sourceKind()) {
            case CastingSourceKind::SpellLikeAbility:
                creature->consumeSpellLikeAbility(spell.type, selection->casterLevel);
                context.casterLevel = creature->spellLikeAbilityCasterLevel(static_cast<int>(spell.type));
                break;
            case CastingSourceKind::UnselectedClass:
                // An explicitly unselected-class source skips readiness/resource debit.
                context.casterLevel = std::max(10, 2 * static_cast<int>(spell.innateLevel) - 1);
                break;
            case CastingSourceKind::Class:
                if (!creature->commitSpellForcePointCost(spell, context.forcePointCost)) return false;
                context.casterLevel = creature->adjustedClassLevel(selection->classIndex);
                break;
            }
            if (selection->hasExplicitLevel()) context.casterLevel = selection->casterLevel;
        } else if (!freeCast) {
            const bool knownForcePower = context.casterLevel >= 0 &&
                creature->attributes().hasSpell(spell.type);
            if (knownForcePower) {
                if (!creature->commitSpellForcePointCost(spell, context.forcePointCost)) return false;
                context.castingClass = creature->spellCastingClass(spell);
            } else {
                if (!creature->readySpellLikeAbility(spell.type, context.casterLevel)) return false;
                creature->consumeSpellLikeAbility(spell.type, context.casterLevel);
                context.castingClass = kSpellLikeAbilityClass;
            }
        }
    } else if (isa<Placeable>(&actor)) {
        context.casterLevel = std::max(10, 2 * static_cast<int>(spell.innateLevel) - 1);
    } else {
        return false;
    }
    actor.setSpellCastContext(context);
    debug(str(boost::format("Spell committed: actor=%u spell=%d level=%d cost=%d")
        % actor.id() % context.spellId % context.casterLevel % context.forcePointCost), LogChannel::Combat);
    return true;
}

void useItemAtOnce(Game &game, Creature &user, const std::shared_ptr<Item> &item) {
    // The user becomes its own spell target.
    user.spellScriptContext().setActiveTarget(game.getObjectById(user.id()));
    const auto &properties = item->properties();
    // Immediate use takes the first cast-spell property, whether or not an
    // upgrade gates it.
    const auto property = std::find_if(properties.begin(), properties.end(), [](const Item::PropertyEntry &entry) {
        return entry.propertyName == static_cast<uint16_t>(ItemProperty::ActivateItem);
    });
    if (property == properties.end()) return;
    const size_t index = static_cast<size_t>(property - properties.begin());
    // The spell is aimed at the user and the origin, facing as the user does,
    // and takes effect after the user's last measured spell flight.
    if (auto spell = user.services().game.spells.get(static_cast<SpellType>(property->subtype))) {
        SpellCastContext context;
        beginSpellCastContext(context, *spell);
        context.casterLevel = user.spellCasterLevel(*spell, true);
        user.setSpellCastContext(context);
        publishItemCastLevel(user);
        queueSpellImpact(game, *spell, user, &user, Location(glm::vec3(0.0f), scriptFacingFromObject(user.getFacing())),
            context, item.get(), user.lastSpellProjectileMilliseconds());
        // The impact goes out with the same release presentation as a cast.
        presentSpellRelease(user, *spell);
    }
    if (item->consumeSpellUse(index)) game.queueObjectDestruction(*item, 0.0f);
    if (user.isInCombat()) user.startItemUseCooldown();
    game.breakForfeitCondition(user, Party::kForfeitNoItems);
}

glm::vec3 menuItemLocation(const Object &user, const Object &target) {
    return &target == &user ? glm::vec3(0.0f) : target.position();
}

bool spellTargetLost(const Object &caster, const Object &target) {
    const auto *creature = isa<Creature>(&caster) ? dyn_cast<Creature>(&target) : nullptr;
    return creature && (creature->isDead() || creature->isTemporarilyDead());
}

// The droid utility device's base item type.
static constexpr int kDroidUtilityItemType = 12;

bool itemUseEndsWithItem(int itemType) {
    return itemType != kDroidUtilityItemType;
}

// Reported when an item has no use left as its use would take effect.
static constexpr int kItemNoUseLeftStrRef = 1434;

void presentFailedItemUse(Game &game, Object &user, bool noUseLeft) {
    auto *creature = dyn_cast<Creature>(&user);
    if (!creature) return;
    if (!creature->isDead() && !creature->isTemporarilyDead()) creature->showPauseReadyAnimation(false);
    if (noUseLeft && game.party().getLeader().get() == creature) game.addFeedbackMessage(kItemNoUseLeftStrRef);
}

bool queueSpellImpact(Game &game, const Spell &spell, Object &caster,
                      Object *target, const Location &location,
                      const SpellCastContext &context, Object *item, uint32_t delayMilliseconds) {
    const auto module = game.module();
    if (!module || !caster.isRuntimeLive()) {
        warn("Spell impact rejected: caster or module no longer active");
        return false;
    }
    SavedEventRecord event;
    const uint64_t when = game.worldTimeMilliseconds() + delayMilliseconds;
    event.day = static_cast<uint32_t>(when / game.millisecondsPerWorldDay());
    event.time = static_cast<uint32_t>(when % game.millisecondsPerWorldDay());
    event.object = SavedObjectReference::fromRuntimeId(caster.id());
    event.caller = event.object;
    event.eventId = static_cast<uint32_t>(SavedEventType::SpellImpact);
    SavedSpellImpact impact;
    impact.spellId = static_cast<int>(spell.type);
    impact.caster = event.object;
    impact.target = SavedObjectReference::fromRuntimeId(target ? target->id() : script::kObjectInvalid);
    impact.area = SavedObjectReference::fromRuntimeId(module->area() ? module->area()->id() : script::kObjectInvalid);
    impact.item = SavedObjectReference::fromRuntimeId(item ? item->id() : script::kObjectInvalid);
    impact.script = spell.impactScript;
    impact.targetPosition = location.position();
    impact.capturedContext = SavedSpellImpact::CapturedContext {
        context.casterLevel, context.metaMagic, location.facing(), context.castingClass, 0,
        caster.spellScriptContext().levelOverride};
    impact.finalForceCost = context.forcePointCost;
    event.payload = std::move(impact);
    const bool bound = event.bindObjectReferences(game);
    if (!bound) {
        warn("Spell impact rejected: object reference could not be resolved");
        return false;
    }
    debug(str(boost::format("Spell queued: actor=%u spell=%d delay=%u script=%s")
        % caster.id() % context.spellId % delayMilliseconds % spell.impactScript), LogChannel::Combat);
    module->enqueueBoundSaveEvent(std::move(event), true);
    return true;
}

void runSpellImpact(Game &game, const Spell &spell, Object &caster,
                    Object *target, const std::shared_ptr<Location> &location,
                    const SpellCastContext *context, Object *item, std::optional<int> levelOverride) {
    SpellCastContext cast = context ? *context : caster.spellCastContext();
    cast.spellId = static_cast<int>(spell.type);
    caster.spellScriptContext().setImpact(cast,
        target ? game.getObjectById(target->id()) : nullptr,
        item ? game.getObjectById(item->id()) : nullptr, location.get());
    // The event handler clears this member after either spell-script route,
    // and a spell with no impact script clears it the same way.
    if (levelOverride && isa<Creature>(&caster)) caster.spellScriptContext().levelOverride = *levelOverride;
    struct ClearLevelOverride {
        Object &caster;
        ~ClearLevelOverride() { caster.spellScriptContext().levelOverride.reset(); }
    } clearLevelOverride {caster};
    if (spell.impactScript.empty()) {
        warn("Spell impact has no script: " + std::to_string(static_cast<int>(spell.type)));
        return;
    }
    std::vector<script::Argument> args {
        {script::ArgKind::Caller, script::Variable::ofObject(caster.id())},
    };
    struct RestoreSpell {
        Object *object;
        SpellType previous;
        ~RestoreSpell() { if (object) object->setSpellCast(previous); }
    } restore {target, target ? target->spellCast() : SpellType::All};
    if (target) target->setSpellCast(spell.type);
    // Spell-impact events set this on the caster and clear it after
    // RunScript; the committed cast context must not leak into later effects.
    struct ClearEffectSpell {
        Object &caster;
        ~ClearEffectSpell() { caster.setEffectSpellId(0xffffffffu); }
    } clearEffectSpell {caster};
    caster.setEffectSpellId(static_cast<uint32_t>(spell.type));
    debug(str(boost::format("Spell impact: actor=%u spell=%d target=%u script=%s")
        % caster.id() % static_cast<int>(spell.type)
        % (target ? target->id() : script::kObjectInvalid) % spell.impactScript), LogChannel::Combat);
    const int result = game.scriptRunner().run(spell.impactScript, args);
    debug(str(boost::format("Spell script returned: actor=%u spell=%d result=%d")
        % caster.id() % static_cast<int>(spell.type) % result), LogChannel::Combat);
}

} // namespace game
} // namespace reone
