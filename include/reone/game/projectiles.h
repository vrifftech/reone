/*
 * Copyright (c) 2025 The reone project contributors
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

#include "reone/game/types.h"
#include "reone/game/savedruntime.h"

namespace reone {

namespace resource {

class TwoDAs;
class TwoDA;

} // namespace resource

namespace game {

class Creature;
struct Spell;
class Object;
class Item;
class Game;
struct ServicesView;
struct EffectInstance;

enum class ProjectileAttackType {
    Basic = 1,
    Rapid = 2,
    Sniper = 3,
    Power = 4,
};

struct ProjectileSpec {
    std::vector<std::pair<float, int>> projectiles;
    uint32_t misses;
};

class IProjectiles {
public:
    virtual ~IProjectiles() = default;
    virtual void clear() = 0;
    virtual void launchLightsaberThrow(Creature &, const EffectInstance &, Game &, ServicesView &) = 0;
    virtual void update(float, Game &, ServicesView &) = 0;
    virtual void retireAreaRuntime() = 0;
    // Presentation hooks remain optional for headless providers.
    virtual uint64_t beginSpell(Object &, Object *, const glm::vec3 &, const Spell &,
                               ProjectilePathType, Game &, ServicesView &) { return 0; }
    virtual void releaseSpell(uint64_t, float, Game &, ServicesView &) {}
    virtual void cancelSpell(uint64_t) {}
    virtual bool blocksRangedParry(const Creature &) const { return false; }
    virtual void launchReflected(Creature &, Creature &, const Item &, Game &, ServicesView &) {}
    virtual void launchSafeProjectile(Creature &, Object &, const Item &,
                                      const AttackEventFields &, Game &, ServicesView &, uint16_t = 0) {}
    virtual std::vector<SavedProjectile> savePresentations() const { return {}; }
    virtual void restorePresentations(std::vector<SavedProjectile>, Game &, ServicesView &) {}
    virtual ProjectileSpec *get(ProjectileAttackType attack, CreatureWieldType wield, int appearance) = 0;
};

class Projectiles : public IProjectiles {
public:
    Projectiles(resource::TwoDAs &twoDas) :
        _twoDas(twoDas) {}

    void init();
    void clear() override;
    void launchLightsaberThrow(Creature &, const EffectInstance &, Game &, ServicesView &) override;
    void update(float dt, Game &, ServicesView &) override;
    void retireAreaRuntime() override;
    uint64_t beginSpell(Object &, Object *, const glm::vec3 &, const Spell &,
                        ProjectilePathType, Game &, ServicesView &) override;
    void releaseSpell(uint64_t, float, Game &, ServicesView &) override;
    void cancelSpell(uint64_t) override;
    bool blocksRangedParry(const Creature &) const override;
    void launchReflected(Creature &, Creature &, const Item &, Game &, ServicesView &) override;
    void launchSafeProjectile(Creature &, Object &, const Item &,
                              const AttackEventFields &, Game &, ServicesView &, uint16_t = 0) override;
    std::vector<SavedProjectile> savePresentations() const override;
    void restorePresentations(std::vector<SavedProjectile>, Game &, ServicesView &) override;

    ProjectileSpec *get(ProjectileAttackType attack, CreatureWieldType wield, int appearance) override;

private:
    struct ActiveProjectile;
    void attachPresentation(ActiveProjectile &, Game &, ServicesView &, bool restoring);
    void startLeg(ActiveProjectile &, Game &, ServicesView &, bool restoring = false);
    void releaseHand(const ActiveProjectile &);
    std::vector<std::shared_ptr<ActiveProjectile>> _active;
    uint64_t _nextPresentationId {1};

    void parseHumanoidWeaponDischarge(resource::TwoDA &weaponDa);

    void parseDroidWeaponDischarge(resource::TwoDA &weaponDa,
                                   resource::TwoDA &droidDa,
                                   resource::TwoDA &animDa);

    resource::TwoDAs &_twoDas;

    std::map<std::pair<CreatureWieldType, ProjectileAttackType>,
             ProjectileSpec>
        _humanoids;

    std::map<std::pair<int, ProjectileAttackType>,
             ProjectileSpec>
        _droids;
};

} // namespace game

} // namespace reone
