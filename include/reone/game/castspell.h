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

#pragma once

#include "types.h"
#include "savedruntime.h"

namespace reone {

namespace scene {
class ModelSceneNode;
class ISceneGraph;
} // namespace scene

namespace graphics {
class Model;
}

namespace game {

class Action;
class CombatRound;
class Creature;
class Object;
class Game;
struct Spell;
class Location;

struct SpellCastContext {
    int spellId {-1};
    int casterLevel {0};
    int metaMagic {255};
    int forcePointCost {0};
};

bool admitSpellCast(const Object &actor, const Spell &spell, bool freeCast, bool itemCast = false);
float spellRange(const Object &actor, const Spell &spell, const Object *target = nullptr);
bool withinSpellRange(const Object &actor, const Spell &spell, const glm::vec3 &position,
                      const Object *target = nullptr);
bool commitSpellCast(Object &actor, const Spell &spell, bool freeCast,
                     SpellCastContext &context, bool itemCast = false);
ProjectilePathType normalizeProjectilePath(ProjectilePathType path);
uint32_t spellProjectileTimeMilliseconds(
    const Spell &spell, const glm::vec3 &origin, const glm::vec3 &destination,
    ProjectilePathType path, bool tsl);
float spellProjectileTime(const Spell &spell, const glm::vec3 &origin,
                          const glm::vec3 &destination, ProjectilePathType path, bool tsl = true);
ProjectilePathType effectiveProjectilePath(const Spell &spell, ProjectilePathType overridePath);

bool queueSpellImpact(Game &game, const Spell &spell, Object &caster,
                      Object *target, const Location &location,
                      const SpellCastContext &context, Object *item = nullptr, uint32_t delayMilliseconds = 0);

void runSpellImpact(Game &game, const Spell &spell, Object &caster,
                    Object *target, const std::shared_ptr<Location> &location,
                    const SpellCastContext *context = nullptr);

class SpellSchedule {
public:
    explicit SpellSchedule(float conjTime, float castTime, float catchTime = 0.0f) :
        _conjTime(conjTime), _castTime(castTime), _catchTime(catchTime) {}

    enum State {
        WaitConjure,
        Conjure,
        WaitCast,
        Cast,
        WaitEffect,
        Effect,
        WaitFinish,
        Finish,
    };

    State update(const CombatRound &round, Action &action, float dt);
    State update(bool canStart, bool roundFinished, float dt);
    void save(SavedCastAction &record) const;
    void restore(const SavedCastAction &record);
    bool awaitingRelease() const { return _state < WaitFinish; }
    bool holdsRound() const { return _state < WaitFinish || remaining() > 0.0f; }
    float remaining() const { return std::max(0.0f, _conjTime + _castTime + _catchTime - _time); }

private:
    State _state {WaitConjure};
    float _time {0.0f};
    float _conjTime {0.0f};
    float _castTime {0.0f};
    float _catchTime {0.0f};
};

class Grenade {
public:
    ~Grenade();

    enum State {
        Swing,
        Throw,
        Wait,
        Explode,
        Finish,
    };

    void fire(Creature &caster, graphics::Model &projModel, float swingTime, float throwTime);
    void update(SpellSchedule::State spellState, Object &target, float dt);

private:
    // Compute parameters for a parabolic trajectory from origin to target.
    void computeTrajectory(glm::vec3 origin, glm::vec3 target, float throwTime);

private:
    State _state {Swing};
    std::shared_ptr<scene::ModelSceneNode> _projNode;
    float _time {0.0f};
    float _swingTime {0.0f};
    float _throwTime {0.0f};
    glm::vec3 _throwOrigin;
    glm::vec3 _throwTarget;
    glm::vec3 _throwVelocity;
    glm::vec3 _throwAccel;
};

} // namespace game
} // namespace reone
