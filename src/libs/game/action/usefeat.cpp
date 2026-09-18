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

#include "reone/game/action/usefeat.h"

#include "reone/game/animations.h"
#include "reone/game/attack.h"
#include "reone/game/combat.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object.h"
#include "reone/game/projectiles.h"
#include "reone/scene/graphs.h"

#include <cassert>

namespace reone {

namespace game {

static const char *getAnimFormat(FeatType feat) {
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::ImprovedCriticalStrike:
    case FeatType::MasterCriticalStrike:
        return "f%da1";

    case FeatType::Flurry:
    case FeatType::ImprovedFlurry:
    case FeatType::WhirlwindAttack:
        return "f%da2";

    case FeatType::PowerAttack:
    case FeatType::ImprovedPowerAttack:
    case FeatType::MasterPowerAttack:
        return "f%da3";

    case FeatType::RapidShot:
    case FeatType::ImprovedRapidShot:
    case FeatType::MultiShot:
        return "b%da2";

    case FeatType::SniperShot:
    case FeatType::ImprovedSniperShot:
    case FeatType::MasterSniperShot:
        return "b%da3";

    case FeatType::PowerBlast:
    case FeatType::ImprovedPowerBlast:
    case FeatType::MasterPowerBlast:
        return "b%da4";

    default:
        return nullptr;
    }
}

static std::optional<ProjectileAttackType> getProjectileType(FeatType feat) {
    switch (feat) {
    case FeatType::RapidShot:
    case FeatType::ImprovedRapidShot:
    case FeatType::MultiShot:
        return ProjectileAttackType::Rapid;

    case FeatType::SniperShot:
    case FeatType::ImprovedSniperShot:
    case FeatType::MasterSniperShot:
        return ProjectileAttackType::Sniper;

    case FeatType::PowerBlast:
    case FeatType::ImprovedPowerBlast:
    case FeatType::MasterPowerBlast:
        return ProjectileAttackType::Power;

    default:
        return std::nullopt;
    }
}

static std::string getAttackAnim(FeatType feat, CreatureWieldType attackerWield) {
    const char *format = getAnimFormat(feat);
    if (!format) {
        return std::string();
    }
    return str(boost::format(format) % static_cast<int>(attackerWield));
}

static int getMeleeFeatAnimationKind(FeatType feat) {
    switch (feat) {
    case FeatType::CriticalStrike:
    case FeatType::ImprovedCriticalStrike:
    case FeatType::MasterCriticalStrike:
        return 1;
    case FeatType::Flurry:
    case FeatType::ImprovedFlurry:
    case FeatType::WhirlwindAttack:
        return 2;
    case FeatType::PowerAttack:
    case FeatType::ImprovedPowerAttack:
    case FeatType::MasterPowerAttack:
        return 3;
    default:
        return 0;
    }
}

static std::string getMeleeFeatAttackAnimation(
    FeatType feat,
    Creature &attacker,
    const Object &target,
    CreatureWieldType wield) {

    int kind = getMeleeFeatAnimationKind(feat);
    if (kind == 0) {
        return std::string();
    }

    attacker.selectMeleeAttackVariant(
        !isCreatureCombat(attacker, target));

    if (wield == CreatureWieldType::StunBaton) {
        return kind == 2 ? "g1a2" : "g1a1";
    }

    assert(isMeleeWieldType(wield) && "Invalid melee feat wield type");
    return str(boost::format("f%da%d") % static_cast<int>(wield) % kind);
}

static std::vector<std::string> attack(
    FeatType feat,
    const CombatRound &round,
    Creature &attacker,
    Object &target,
    const IAnimations &anims,
    AttackBuffer &attacks) {

    attacks.addPhysicalAttacks(attacker, target, feat);

    scene::AnimationProperties animProp =
        scene::AnimationProperties::fromFlags(scene::AnimationFlags::blend);

    CreatureWieldType targetWield = CreatureWieldType::None;
    if (auto *targetCreature = dyn_cast<Creature>(&target)) {
        targetWield = targetCreature->getWieldType();
    }

    CreatureWieldType attackerWield = attacker.getWieldType();
    bool melee = !isRangedWieldType(attackerWield);
    size_t animationCount = melee ? attacks.attackCount() : 1;
    std::vector<std::string> attackAnimations;
    attackAnimations.reserve(animationCount);

    for (size_t index = 0; index < animationCount; ++index) {
        std::string attackAnim;
        if (melee && getMeleeFeatAnimationKind(feat) != 0) {
            attackAnim = attacker.modelType() == Creature::ModelType::Creature
                             ? selectPhysicalMeleeAttackAnimation(
                                   attacker,
                                   target,
                                   attackerWield)
                             : getMeleeFeatAttackAnimation(
                                   feat,
                                   attacker,
                                   target,
                                   attackerWield);
        } else {
            attackAnim = getAttackAnim(feat, attackerWield);
        }
        attackAnimations.push_back(std::move(attackAnim));
    }
    const std::string &primaryAttackAnimation = attackAnimations.front();
    attacker.playAnimation(primaryAttackAnimation, animProp);

    if (round.duel) {
        auto &opponent = static_cast<Creature &>(target);
        opponent.face(attacker);

        std::string resultAnim = anims.getAttackResult(
            primaryAttackAnimation,
            targetWield,
            attacks.result());
        opponent.playAnimation(resultAnim, animProp);
    }

    return attackAnimations;
}

void UseFeatAction::addProjectiles(const Creature &creature, FeatType feat) {
    auto projType = getProjectileType(feat);
    if (!projType) {
        return;
    }

    ProjectileSpec *spec = _services.game.projectiles.get(
        projType.value(), creature.getWieldType(), creature.appearance());

    if (!spec) {
        // no projectiles for this attack
        return;
    }

    addProjectilesFromSpec(_projectiles, *spec, &_attacks);
}

void UseFeatAction::execute(std::shared_ptr<Action> self, Object &actor, float dt) {
    Creature &attacker = static_cast<Creature &>(actor);
    if (!runtimeDependenciesLive()) {
        cancel(self, actor);
        markCancelled();
        return;
    }
    auto target = _target.resolve();
    if (!target || target.get() == &actor) {
        finish(attacker);
        return;
    }
    if (isPhysicalAttackFeat(_feat)) {
        attacker.setAttemptedAttackTarget(target->id());
    }

    if (target->isDead() && !_attacks.hasPendingMelee()) {
        finish(attacker);
        return;
    }

    if (_game.combat().isActionPaused(*self)) return;

    if (!navigateToAttackTarget(attacker, *target, dt, _reachedTarget, &_game, self.get())) {
        return;
    }

    attacker.face(*target);

    const CombatRound &round = _game.combat().addAction(self, actor);
    if (round.suspends(*self)) return;
    AttackSchedule::State state = _schedule.update(round, *self, dt);

    // Gameplay updates
    switch (state) {
    case AttackSchedule::Attack: {
        lock();
        attacker.setMovementType(Creature::MovementType::None);
        attacker.setMovementRestricted(true);

        std::vector<std::string> attackAnimations = attack(
            _feat,
            round,
            attacker,
            *target,
            _services.game.animations,
            _attacks);
        _attacks.resolveMeleeSpecialAttack(_feat, attacker, *target, _game);
        _attacks.resolve(attacker, *target);

        if (!isRangedWieldType(attacker.getWieldType())) {
            _attacks.prepareMeleeSequence(
                _services.game.animations,
                attackAnimations);
            _schedule.startMelee(
                _attacks.latestMeleeImpactMilliseconds());
            _attacks.signalReadyMelee(
                0,
                _game,
                _services,
                attacker,
                *target);
        }

        addProjectiles(attacker, _feat);
        return;
    }
    case AttackSchedule::WaitDamage: {
        if (_schedule.isMelee()) {
            _attacks.signalReadyMelee(
                _schedule.meleeElapsedMilliseconds(),
                _game,
                _services,
                attacker,
                *target);
        }
        break;
    }
    case AttackSchedule::Damage: {
        if (_schedule.isMelee()) {
            _attacks.signalReadyMelee(
                _schedule.meleeElapsedMilliseconds(),
                _game,
                _services,
                attacker,
                *target);
        } else {
            _attacks.signal(_game, _services, attacker, *target);
        }
        break;
    }
    case AttackSchedule::Finish: {
        finish(attacker);
        return;
    }
    default:
        break;
    }

    // Projectiles
    if (!_projectiles.empty()) {
        switch (state) {
        case AttackSchedule::Damage:
        case AttackSchedule::WaitDamage:
        case AttackSchedule::WaitFinish: {
            auto &sceneGraph = _services.scene.graphs.get(kSceneMain);
            _projectiles.update(dt, attacker, *target, sceneGraph);
            break;
        }
        default:
            break;
        }
    }
}

void UseFeatAction::onQueued(Object &actor) {
    if (!originalSavedAction() && isPhysicalAttackFeat(_feat)) {
        if (auto target = _target.resolve()) {
            if (auto creature = dyn_cast<Creature>(target)) {
                cast<Creature>(actor).recordQueuedAttack(*creature);
            }
        }
    }
}

bool UseFeatAction::cancel(std::shared_ptr<Action> self, Object &actor) {
    Creature &attacker = static_cast<Creature &>(actor);
    _game.combat().transferEquipment(static_cast<Creature &>(actor));
    _attacks.discardPendingMelee();
    _attacks.clearHistory();
    attacker.clearCurrentAttackTarget();
    finish(attacker);
    return true;
}

std::optional<SavedActionRecord> UseFeatAction::saveFacingState() const {
    auto target = _target.resolve();
    if (!target || !isPhysicalAttackFeat(_feat)) {
        return std::nullopt;
    }

    // K1 and K2 use the same ActionId 12 physical-attack command.
    // Slot 6 is nSpecialAttack: zero denotes a basic attack and a physical
    // feat identifier denotes the corresponding special attack. Live round,
    // roll, schedule, animation and projectile state restart after restore.
    SavedActionRecord result =
        originalSavedAction().value_or(SavedActionRecord {});
    result.actionId = 12;
    result.declaredParameterCount = 10;
    result.parameters = {
        {1, int32_t {_cutsceneAttack ? 1 : 0}},
        {3, SavedObjectReference::fromRuntimeId(target->id())},
        {1, int32_t {1}},
        {1, int32_t {10009}},
        {1, int32_t {1500}},
        {1, int32_t {1}},
        {1, static_cast<int32_t>(_feat)},
        {1, int32_t {0}},
        {1, int32_t {4}},
        {1, int32_t {0}},
    };
    SavedPhysicalAction progress;
    _attacks.saveContinuation(progress, _game);
    _schedule.saveContinuation(*progress.state);
    progress.state->fields().push_back(resource::Gff::Field::newByte("Reached", _reachedTarget));
    result.physical = std::move(progress);
    return result;
}

void UseFeatAction::restorePhysicalState(const SavedPhysicalAction &state) {
    if (!state.valid()) return;
    _attacks.restoreContinuation(state); _schedule.restoreContinuation(*state.state);
    _reachedTarget = state.state->getBool("Reached");
}

void UseFeatAction::finish(Creature &attacker) {
    attacker.setMovementRestricted(false);
    _projectiles.reset();
    complete();
}

} // namespace game

} // namespace reone
