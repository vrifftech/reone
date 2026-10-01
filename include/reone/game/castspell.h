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

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>

#include <glm/vec3.hpp>

#include "types.h"
#include "savedruntime.h"

namespace reone {

namespace scene {
class ModelSceneNode;
class ModelNodeSceneNode;
class ISceneGraph;
} // namespace scene

namespace game {

class Action;
struct CombatRound;
class Creature;
class Item;
class Object;
class Game;
struct Spell;
class Location;
enum class SpellCastAnimation;

struct SpellCastContext {
    int spellId {-1};
    int casterLevel {0};
    int metaMagic {255};
    int forcePointCost {0};
    int castingClass {kUnselectedCastingClass};
};

class SpellScriptContext {
public:
    SpellCastContext cast;
    std::optional<int> levelOverride;

    void setImpact(const SpellCastContext &context,
                   const std::shared_ptr<Object> &target,
                   const std::shared_ptr<Object> &item,
                   const Location *location);
    std::shared_ptr<Object> target() const;
    std::shared_ptr<Object> item() const;
    std::shared_ptr<Location> location() const;
    std::shared_ptr<Object> activeTarget() const;
    void setActiveTarget(const std::shared_ptr<Object> &target);
    void clearActiveTarget();

    bool restored() const { return _restored; }
    void restore(const resource::Gff *state);
    void bind(const std::function<std::shared_ptr<Object>(uint32_t)> &resolver);
    std::shared_ptr<resource::Gff> save(
        const std::function<uint32_t(const Object &)> &objectId) const;
    void retireAreaRuntime(const std::set<const Object *> &retainedObjects);

private:
    RuntimeObjectRef<Object> _target;
    RuntimeObjectRef<Object> _item;
    RuntimeObjectRef<Object> _activeTarget;
    std::shared_ptr<Location> _location;
    std::array<uint32_t, 3> _savedIds {{kSavedRuntimeInvalidObjectId,
        kSavedRuntimeInvalidObjectId, kSavedRuntimeInvalidObjectId}};
    bool _needsBind {false};
    bool _restored {false};
};

int queryCasterLevel(const Object &object);
SpellType itemSpellForActor(const Object &actor, SpellType propertySpell);
void publishItemCastLevel(Creature &caster);

/** The caster's equipment neither forbids the power nor lacks what it requires. */
bool equipmentAllowsSpell(const Creature &caster, const Spell &spell);
bool admitSpellCast(const Object &actor, const Spell &spell, bool freeCast, bool itemCast = false,
                    std::optional<SpellSelection> selection = std::nullopt);
float spellRange(const Object &actor, const Spell &spell, const Object *target = nullptr);
bool withinSpellRange(const Object &actor, const Spell &spell, const glm::vec3 &position,
                      const Object *target = nullptr);
bool commitSpellCast(Object &actor, const Spell &spell, bool freeCast,
                     SpellCastContext &context, bool itemCast = false,
                     std::optional<SpellSelection> selection = std::nullopt);
ProjectilePathType normalizeProjectilePath(ProjectilePathType path);
/**
 * The path a script command names: the default, homing, ballistic, high
 * ballistic or accelerating path. Any other value names none, and the command
 * then casts nothing.
 */
std::optional<ProjectilePathType> projectilePathFromScript(int value);
/**
 * The casting source a script command picks for a creature: the first of its
 * first two classes that uses the Force, has uses of the spell left and can
 * cast it, else its spell-like ability for the spell. Without either the
 * command casts nothing.
 */
std::optional<SpellSelection> scriptCastingSource(const Creature &caster, const Spell &spell);
/**
 * What a spell's release shows on its caster: the cast sound and, on a
 * creature, the cast visuals. Ordinary, fake and instant casts alike, and an
 * item used at once.
 */
void presentSpellRelease(Object &caster, const Spell &spell);
uint32_t spellProjectileTimeMilliseconds(
    const Spell &spell, const glm::vec3 &origin, const glm::vec3 &destination,
    ProjectilePathType path, bool tsl);
ProjectilePathType effectiveProjectilePath(const Spell &spell, ProjectilePathType overridePath);

bool queueSpellImpact(Game &game, const Spell &spell, Object &caster,
                      Object *target, const Location &location,
                      const SpellCastContext &context, Object *item = nullptr, uint32_t delayMilliseconds = 0);

/**
 * Use an item at once, as the inventory does: its first cast-spell property,
 * whether or not an upgrade gates it, takes effect on the user after the
 * user's last measured spell flight, with no action or animation (the release
 * still shows its cast sound and cast visuals), spends one
 * use (an exhausted item is destroyed), and, in combat, holds back the next
 * item use. A party member's use breaks the forfeit condition that forbids
 * items.
 */
void useItemAtOnce(Game &game, Creature &user, const std::shared_ptr<Item> &item);

/**
 * The location an item use from a menu carries: its target's position, or
 * the origin when the user uses the item on itself.
 */
glm::vec3 menuItemLocation(const Object &user, const Object &target);

/**
 * A creature's cast or item use at a creature fails once that creature has
 * died or is a party member at zero vitality.
 */
bool spellTargetLost(const Object &caster, const Object &target);

/**
 * After its release an item use ends as soon as its item is gone, except a
 * droid utility's (base item type 12), which plays on to its end.
 */
bool itemUseEndsWithItem(int itemType);

/**
 * A failed item use leaves its user in its pause or ready pose. One that
 * fails before taking effect because its item has no use left tells the
 * controlled creature so.
 */
void presentFailedItemUse(Game &game, Object &user, bool noUseLeft = false);

void runSpellImpact(Game &game, const Spell &spell, Object &caster,
                    Object *target, const std::shared_ptr<Location> &location,
                    const SpellCastContext *context = nullptr, Object *item = nullptr,
                    std::optional<int> levelOverride = std::nullopt);

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
    State update(bool canStart, float dt);
    void setDurations(float conjTime, float castTime, float catchTime) {
        _conjTime = conjTime; _castTime = castTime; _catchTime = catchTime;
    }
    void save(SavedCastAction &record) const;
    void restore(const SavedCastAction &record);
    bool awaitingRelease() const { return _state < WaitFinish; }
    float time() const { return _time; }
    /** The last update carried the cast's time across \p seconds. */
    bool reached(float seconds) const { return _previousTime < seconds && _time >= seconds; }
    // The cast holds its round until its completion (including the
    // associated-feat expenditure) has run.
    bool holdsRound() const { return _state != Finish; }
    float remaining() const { return std::max(0.0f, _conjTime + _castTime + _catchTime - _time); }

private:
    State _state {WaitConjure};
    float _time {0.0f};
    float _previousTime {0.0f};
    float _conjTime {0.0f};
    float _castTime {0.0f};
    float _catchTime {0.0f};
};

// What using an item shows, by its base item type: the clip and its animation ID, whether it loops,
// whether the user is posed instead of holding it, whether it is drawn on the
// creature it is used on, when the droid utility turns to its loop, when the
// item takes effect, and when the use ends in the user's pose.
struct ItemUsePresentation {
    std::string clip;
    int clipId {-1}; // the clip's animation ID
    bool loops {false};
    bool hold {true};
    bool onTarget {false};
    float loopAt {-1.0f};
    float impact {0.001f};
    float end {1.5f};
};

/**
 * The models a spell shows on its caster's head, hand and feet. The conjure
 * visuals come with the conjure; the release brings the cast visuals and fades
 * the conjure visuals out. After the release the conjure visuals go within five
 * seconds and the cast visuals five seconds later; the next conjure or release
 * replaces them, and a cast that ends before its release takes both away.
 */
class SpellCastVisuals {
public:
    ~SpellCastVisuals();

    void showConjure(Creature &caster, const Spell &spell);
    void showCast(Creature &caster, const Spell &spell);
    void clear();
    void update(float dt);

private:
    struct Attached {
        std::weak_ptr<scene::ModelSceneNode> body;
        scene::ModelNodeSceneNode *hook {nullptr};
        std::shared_ptr<scene::ModelSceneNode> model;
        bool fading {false};
    };
    using Set = std::array<Attached, 3>;

    static void attach(Set &set, Creature &caster, const std::array<std::string, 3> &models,
                       const char *animation);
    static void detach(Attached &attached);
    static void detach(Set &set);

    Set _conjure;
    Set _cast;
    bool _released {false};
    float _conjureRemaining {0.0f};
    float _castRemaining {0.0f};
};

/**
 * An entangled caster's paid cast fails: the caster is posed and its
 * orientation released. Returns whether the cast was interrupted.
 */
bool interruptEntangledCast(Creature &caster);

// Shows each frame what the cast and item executors write: the conjure, the
// cast clip held for the cast time, and the pose at the end. An item use is
// shown by its item's base item type.
class CastPresenter {
public:
    /** The item use's presentation and times, computed when it starts. */
    const ItemUsePresentation &itemUse(const Creature &user, const Spell &spell, int itemType,
                                       const Creature *targetCreature, const glm::vec3 &targetPosition);
    void update(Creature &caster, const Spell &spell, std::optional<int> itemType, Creature *targetCreature,
                const glm::vec3 &targetPosition, const SpellSchedule &schedule, SpellSchedule::State state);
    /** Shows again what a restored cast was showing. */
    void restore(Creature &caster, const Spell &spell, const SpellSchedule &schedule);

private:
    std::optional<ItemUsePresentation> _item;

    void updateItem(Creature &user, Creature *targetCreature,
                    const SpellSchedule &schedule, SpellSchedule::State state);
};

} // namespace game
} // namespace reone
