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

#include "reone/game/location.h"
#include "reone/game/d20/spell.h"
#include "reone/game/castspell.h"
#include "reone/game/game.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/placeable.h"
#include "reone/game/object/module.h"
#include "reone/game/forcerules.h"

namespace reone {
namespace game {

SpellSchedule::State SpellSchedule::update(
    const CombatRound &round, Action &action, float dt) {
    if (round.suspends(action)) return _state;
    return update(round.canExecute(action), round.state == CombatRound::Finished, dt);
}

SpellSchedule::State SpellSchedule::update(bool canStart, bool roundFinished, float dt) {
    _time += std::max(0.0f, dt);

    switch (_state) {
    case SpellSchedule::WaitConjure: {
        if (canStart) {
            _time = 0.0f;
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
        if (roundFinished && _time >= _conjTime + _castTime + _catchTime) {
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
    _state = static_cast<State>(record.phase); _time = record.elapsed;
    _conjTime = record.conjureTime; _castTime = record.castTime; _catchTime = record.catchTime;
    // Entry phases have already run before a snapshot is taken.
    if (_state == Conjure) _state = WaitCast;
    else if (_state == Cast) _state = WaitEffect;
    else if (_state == Effect) _state = WaitFinish;
}

static scene::SceneNode &determineGrenadeAttachment(scene::ModelSceneNode &model) {
    if (scene::ModelNodeSceneNode *handHook = model.getNodeByName("rhand")) {
        return *handHook;
    }

    return model;
}

void Grenade::fire(Creature &caster, graphics::Model &projModel, float swingTime, float throwTime) {
    auto casterNode = std::static_pointer_cast<scene::ModelSceneNode>(caster.sceneNode());
    scene::SceneNode &attachmentNode = determineGrenadeAttachment(*casterNode);

    // Create a grenade node and attach it to caster's hand for the duration of
    // the swing.
    scene::ISceneGraph &graph = casterNode->graph();
    _projNode = graph.newModel(projModel, scene::ModelUsage::Projectile);
    attachmentNode.addChild(*_projNode);

    // Keep the grenade attached for swingTime seconds.
    _swingTime = swingTime;
    _throwTime = throwTime;
}

void Grenade::computeTrajectory(glm::vec3 origin, glm::vec3 target, float time) {
    // Position at any time point t, with initial velocity V, origin X', and
    // acceleration g: X = X' + Vt + gt^2/2

    // TODO: instead of solving this for time, we can complicate things more
    // with variable initial velocity and angle.

    _throwAccel = glm::vec3(0.0f, 0.0f, -9.81f);
    _throwVelocity = ((target - origin) / time) - (_throwAccel * time * 0.5f);
    _throwOrigin = origin;
    _throwTarget = target;
    _throwTime = time;
}

void Grenade::update(SpellSchedule::State spellState, Object &target, float dt) {
    _time += dt;

    switch (_state) {
    case Swing: {
        if (_time < _swingTime) {
            // Keep the grenade attached.
            return;
        }

        glm::vec3 origin = _projNode->origin();

        computeTrajectory(origin, target.position(), _throwTime);

        // Detach the grenade and transition to Throw state.
        _projNode->parent()->removeChild(*_projNode);
        _projNode->setLocalTransform(glm::translate(origin));
        _projNode->graph().addRoot(_projNode);
        _state = Throw;
        return;
    }
    case Throw: {
        if (_time < (_swingTime + _throwTime)) {
            float t = _time - _swingTime;
            glm::vec3 pos = _throwOrigin + (_throwVelocity * t) + (_throwAccel * t * t * 0.5f);
            _projNode->setLocalTransform(glm::translate(pos));
            return;
        }
        _projNode->setLocalTransform(glm::translate(_throwTarget));
        _state = Wait;
        return;
    }
    case Wait: {
        if ((int)spellState < (int)SpellSchedule::Effect) {
            return;
        }
        _state = Explode;
        return;
    }
    case Explode: {
        _projNode->graph().removeRoot(*_projNode);
        _projNode.reset();
        _state = Finish;
    }
    default:
        return;
    }
}

Grenade::~Grenade() {
    if (_projNode) {
        _projNode->graph().removeRoot(*_projNode);
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
ProjectilePathType effectiveProjectilePath(const Spell &spell, ProjectilePathType path) {
    return path == ProjectilePathType::Default ? spell.projectilePath : normalizeProjectilePath(path);
}
uint32_t spellProjectileTimeMilliseconds(
    const Spell &spell, const glm::vec3 &origin, const glm::vec3 &destination,
    ProjectilePathType overridePath, bool tsl) {
    if (!spell.projectile) return 0;
    const float distance = glm::distance(origin, destination);
    // K1 calls logf; K2 calls log and narrows before the multiply/add.
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
        // K2 truncates to a signed 64-bit integer and stores its low word.
        // Invalid conversions yield INT64_MIN, whose low word is zero.
        if (std::isfinite(travel) && travel >= 0.0f &&
            static_cast<double>(travel) < 9223372036854775808.0)
            milliseconds = static_cast<uint32_t>(static_cast<uint64_t>(travel));
    } else if (travel > 0.0f) {
        // K1 uses a saturating unsigned conversion (NaN becomes zero).
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

bool admitSpellCast(const Object &actor, const Spell &spell, bool freeCast, bool itemCast) {
    if (actor.isDead()) return false;
    const auto *creature = dyn_cast<Creature>(&actor);
    if (!creature) return isa<Placeable>(&actor);
    if (!creature->canCastSpells()) return false;
    if (!itemCast) {
        const auto mask = creature->forceItemMask();
        if ((mask & spell.forbidItemMask) != 0 ||
            (mask & spell.requireItemMask) != spell.requireItemMask) return false;
    }
    if (freeCast) return true;
    if (creature->spellCasterLevel(spell) >= 0 && creature->attributes().hasSpell(spell.type))
        return creature->canPaySpellForcePointCost(spell);
    int level = 0;
    return creature->readySpellLikeAbility(spell.type, level);
}

float spellRange(const Object &actor, const Spell &spell, const Object *target) {
    float range = spell.range;
    if (const auto *creature = dyn_cast<Creature>(&actor)) range += creature->creaturePersonalSpace() - 0.1f;
    if (const auto *creature = dyn_cast<Creature>(target)) range += creature->creaturePersonalSpace() - 0.1f;
    return std::max(0.0f, range);
}

bool withinSpellRange(const Object &actor, const Spell &spell, const glm::vec3 &position, const Object *target) {
    return &actor == target || glm::distance(glm::vec2(actor.position()), glm::vec2(position)) <= spellRange(actor, spell, target);
}

bool commitSpellCast(Object &actor, const Spell &spell, bool freeCast,
                     SpellCastContext &context, bool itemCast) {
    if (!admitSpellCast(actor, spell, freeCast, itemCast)) return false;
    context.spellId = static_cast<int>(spell.type);
    // These titles do not offer NWN metamagic or domain spell slots.
    context.metaMagic = 0;
    context.forcePointCost = 0;
    if (auto *creature = dyn_cast<Creature>(&actor)) {
        context.casterLevel = creature->spellCasterLevel(spell, freeCast);
        if (!freeCast) {
            const bool knownForcePower = context.casterLevel >= 0 &&
                creature->attributes().hasSpell(spell.type);
            if (knownForcePower) {
                if (!creature->commitSpellForcePointCost(spell, context.forcePointCost)) return false;
            } else if (!creature->consumeSpellLikeAbility(spell.type, context.casterLevel)) {
                return false;
            }
        }
    } else if (isa<Placeable>(&actor)) {
        context.casterLevel = std::max(10, 2 * static_cast<int>(spell.innateLevel) - 1);
    } else {
        return false;
    }
    actor.setSpellCastContext(context);
    return true;
}

bool queueSpellImpact(Game &game, const Spell &spell, Object &caster,
                      Object *target, const Location &location,
                      const SpellCastContext &context, Object *item, uint32_t delayMilliseconds) {
    const auto module = game.module();
    if (!module || !caster.isRuntimeLive()) return false;
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
    impact.targetFacing = location.facing();
    impact.casterLevel = context.casterLevel;
    impact.metaMagic = context.metaMagic;
    impact.finalForceCost = context.forcePointCost;
    event.payload = std::move(impact);
    const bool bound = event.bindObjectReferences(game);
    if (!bound) return false;
    module->enqueueBoundSaveEvent(std::move(event), true);
    return true;
}

void runSpellImpact(Game &game, const Spell &spell, Object &caster,
                    Object *target, const std::shared_ptr<Location> &location,
                    const SpellCastContext *context) {
    if (spell.impactScript.empty()) return;
    const SpellCastContext cast = context ? *context : caster.spellCastContext();
    std::vector<script::Argument> args {
        {script::ArgKind::Caller, script::Variable::ofObject(caster.id())},
        {script::ArgKind::SpellId, script::Variable::ofInt(static_cast<int>(spell.type))},
        {script::ArgKind::SpellTargetObject, script::Variable::ofObject(
            target ? target->id() : script::kObjectInvalid)},
        {script::ArgKind::SpellLocation, script::Variable::ofLocation(location)},
        {script::ArgKind::SpellCasterLevel, script::Variable::ofInt(cast.casterLevel)},
        {script::ArgKind::SpellMetaMagic, script::Variable::ofInt(cast.metaMagic)},
        {script::ArgKind::SpellForcePointCost, script::Variable::ofInt(cast.forcePointCost)},
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
    game.scriptRunner().run(spell.impactScript, args);
}

} // namespace game
} // namespace reone
