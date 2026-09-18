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

#include "reone/system/exception/validation.h"
#include "reone/system/randomutil.h"
#include "reone/scene/di/services.h"
#include "reone/scene/node/model.h"
#include "reone/scene/graph.h"
#include "reone/graphics/model.h"
#include "reone/game/effect/creaturestate.h"
#include "reone/game/effect/damage.h"
#include "reone/game/d20/spells.h"
#include "reone/game/game.h"
#include "reone/game/di/services.h"
#include "reone/game/projectiles.h"
#include "reone/game/castspell.h"
#include "reone/resource/provider/models.h"
#include "reone/game/spellrules.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/area.h"
#include "reone/game/object/module.h"
#include "reone/scene/collision.h"
#include <cmath>
#include "reone/resource/2da.h"
#include "reone/resource/provider/2das.h"
#include "reone/system/arrayref.h"

#include <algorithm>
#include <array>
#include <set>
#include <glm/gtx/quaternion.hpp>

using namespace reone::resource;

namespace reone {

namespace game {

static constexpr char kModelEventDetonate[] = "detonate";

struct Projectiles::ActiveProjectile {
    SavedProjectile saved;
    std::shared_ptr<scene::ModelSceneNode> model;
    std::shared_ptr<scene::ModelSceneNode> flash;
    ~ActiveProjectile() {
        if (model) model->graph().removeRoot(*model);
        if (flash) flash->graph().removeRoot(*flash);
    }
};

static SavedObjectReference referenceTo(const Object *object, const Game &game) {
    auto result = SavedObjectReference::fromRuntimeId(object ? object->id() : script::kObjectInvalid);
    game.bindSavedObjectReference(result);
    return result;
}
static glm::vec3 attachmentPosition(const SavedObjectReference &reference, const std::string &hook,
                                     const glm::vec3 &previous) {
    auto object = reference.boundObject();
    if (!object) return previous;
    auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(object->sceneNode());
    if (model) if (auto *node = model->getNodeByName(hook)) return node->origin();
    return object->position() + (hook == "impact" ? glm::vec3(0, 0, 1.25f) : glm::vec3(0));
}
static glm::vec3 throwTargetPosition(const SavedObjectReference &target,
                                    const SavedObjectReference &caster,
                                    const glm::vec3 &previous) {
    auto object = target.boundObject();
    if (!object) return previous;
    if (object == caster.boundObject()) {
        auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(object->sceneNode());
        if (model) if (auto *hand = model->getNodeByName("rhand")) return hand->origin();
    }
    return object->position() + glm::vec3(0.0f, 0.0f, 1.25f);
}

static glm::vec3 safeProjectileTargetPosition(const SavedObjectReference &reference,
                                               const std::string &hook, const glm::vec3 &previous) {
    auto object = reference.boundObject();
    if (!object) return previous;
    auto model = std::dynamic_pointer_cast<scene::ModelSceneNode>(object->sceneNode());
    if (model && !hook.empty()) if (auto *node = model->getNodeByName(hook)) return node->origin();
    return object->position() + glm::vec3(0.0f, 0.0f, 1.25f);
}

static glm::vec3 safeLegDestination(const SavedProjectile &, const SavedProjectile::Leg &);

static glm::quat throwOrientation(const Object &source) {
    const float facing = source.getFacing();
    const glm::vec3 forward(-glm::sin(facing), glm::cos(facing), 0.0f);
    const glm::vec3 axis = glm::normalize(glm::cross(forward, glm::vec3(0, 0, 1)));
    const glm::vec3 thrown = glm::angleAxis(1.22173f, axis) * forward;
    return glm::rotation(glm::vec3(0, 1, 0), glm::normalize(thrown));
}

/**
 * Droid animation indices into animations.2da for each attack type. These
 * indices are also column names of droiddischarge.2da.
 */
using DroidAnimIDs = SmallVector<std::pair<ProjectileAttackType, int>, 4>;

struct DroidSpec {
    int appearanceType;
    std::vector<ProjectileAttackType> validAttacks;
};

static int filterRowByDroid(TwoDA &twoDa, int start, int end, bool isDroid) {
    std::string droid("droid");
    while (start != end) {
        // Some rows are completely empty, skip them.
        bool isValid = twoDa.getInt(start, "shots") > 0;

        if (isValid && twoDa.getBool(start, "droid", true) == isDroid) {
            return start;
        }
        ++start;
    }
    return end;
}

static void findDroidAnimations(resource::TwoDA &animDA, DroidAnimIDs &droidAnims) {
    SmallVector<std::pair<ProjectileAttackType, std::string>, 4> anims;
    anims.push_back({ProjectileAttackType::Basic, "b0a1"});
    anims.push_back({ProjectileAttackType::Rapid, "b0a2"});
    anims.push_back({ProjectileAttackType::Sniper, "b0a3"});
    anims.push_back({ProjectileAttackType::Power, "b0a4"});

    for (int i = 0; i < animDA.getRowCount(); ++i) {
        for (const auto &kv : anims) {
            ProjectileAttackType kind = kv.first;
            const std::string &animName = kv.second;
            if (animName == animDA.getString(i, "name")) {
                droidAnims.push_back({kind, i});
            }
        }
    }
}

static bool parseDroidDischargeRow(resource::TwoDA &droidDa, int row,
                                   const DroidAnimIDs &droidAnims,
                                   DroidSpec &droidSpec) {
    int commonAppearanceType = -1;

    for (const auto &animId : droidAnims) {
        int animAppearanceType = droidDa.getInt(row, std::to_string(animId.second), -1);
        if (animAppearanceType < 0) {
            continue;
        }
        if (commonAppearanceType == -1) {
            commonAppearanceType = animAppearanceType;
        }
        if (commonAppearanceType != animAppearanceType) {
            // All columns must have the same appearance type, otherwise the row
            // is malformed.
            return false;
        }

        droidSpec.validAttacks.push_back(animId.first);
    }

    if (commonAppearanceType == -1) {
        return false;
    }

    droidSpec.appearanceType = commonAppearanceType;
    return true;
}

static bool parseWeaponDischargeRow(resource::TwoDA &twoDa, int row,
                                    ProjectileSpec &spec) {
    int shots = twoDa.getInt(row, "shots");
    int hits = twoDa.getInt(row, "hits");
    if (shots <= 0 || hits > shots) {
        return false;
    }

    spec.misses = shots - hits;

    std::string switchmask = twoDa.getString(row, "switchmask");
    if (!switchmask.empty() && switchmask.length() != shots) {
        return false;
    }

    for (int i = 0; i < shots; ++i) {
        std::string columnShot = str(boost::format("shot%d") % (i + 1));
        int time_ms = twoDa.getInt(row, columnShot);
        int kind = switchmask.empty() ? 0 : (switchmask[i] - '0');
        spec.projectiles.emplace_back(time_ms / 1000.0f, kind);
    }

    return true;
}

void Projectiles::parseHumanoidWeaponDischarge(resource::TwoDA &weaponDa) {
    CreatureWieldType wields[] = {
        CreatureWieldType::BlasterPistol,
        CreatureWieldType::DualPistols,
        CreatureWieldType::BlasterRifle,
        CreatureWieldType::HeavyWeapon,
    };

    ProjectileAttackType attacks[] = {
        ProjectileAttackType::Basic,
        ProjectileAttackType::Rapid,
        ProjectileAttackType::Sniper,
        ProjectileAttackType::Power,
    };

    int row = 0;
    int rowEnd = weaponDa.getRowCount();

    for (CreatureWieldType wield : ArrayRef(wields)) {
        for (ProjectileAttackType attack : ArrayRef(attacks)) {
            row = filterRowByDroid(weaponDa, row, rowEnd, /*isDroid*/ false);
            if (row == rowEnd) {
                return;
            }

            ProjectileSpec spec;
            if (parseWeaponDischargeRow(weaponDa, row, spec)) {
                _humanoids[{wield, attack}] = spec;
            }
            ++row;
        }
    }
}

void Projectiles::parseDroidWeaponDischarge(resource::TwoDA &weaponDa,
                                            resource::TwoDA &droidDa,
                                            resource::TwoDA &animDa) {
    DroidAnimIDs droidAnims;
    findDroidAnimations(animDa, droidAnims);

    std::vector<DroidSpec> droidSpecs;
    for (int i = 0; i < droidDa.getRowCount(); ++i) {
        DroidSpec spec;
        if (parseDroidDischargeRow(droidDa, i, droidAnims, spec)) {
            droidSpecs.push_back(spec);
        }
    }

    ProjectileAttackType attacks[] = {
        ProjectileAttackType::Basic,
        ProjectileAttackType::Rapid,
        ProjectileAttackType::Sniper,
        ProjectileAttackType::Power,
    };

    int maxDroidAnims = droidDa.getColumnCount();
    if (maxDroidAnims < 1) {
        return;
    }

    int row = 0;
    int rowEnd = weaponDa.getRowCount();
    int skipRows = 2;
    for (DroidSpec &droid : droidSpecs) {
        int i = 0;
        for (ProjectileAttackType attack : ArrayRef(attacks)) {
            row = filterRowByDroid(weaponDa, row, rowEnd, /*isDroid*/ true);
            if (skipRows) {
                // Droid rows a weird: there are empty lines that we already
                // ignore in filterRowByDroid, but the rest seems to be shifted
                // by 2 rows for some reason.
                row = std::min(row + skipRows, rowEnd);
                skipRows = 0;
            }
            if (row == rowEnd) {
                return;
            }
            ProjectileSpec spec;
            if (parseWeaponDischargeRow(weaponDa, row, spec)) {
                _droids[{droid.appearanceType, attack}] = spec;
            }
            ++row;
            ++i;
            if (i == maxDroidAnims) {
                break;
            }
        }
    }
}

void Projectiles::init() {
    std::shared_ptr<TwoDA> animDa(_twoDas.get("animations"));
    std::shared_ptr<TwoDA> droidDa(_twoDas.get("droiddischarge"));
    std::shared_ptr<TwoDA> weaponDa(_twoDas.get("weapondischarge"));

    if (!weaponDa) {
        return;
    }

    parseHumanoidWeaponDischarge(*weaponDa);

    if (!animDa || !droidDa) {
        return;
    }

    parseDroidWeaponDischarge(*weaponDa, *droidDa, *animDa);
}

void Projectiles::clear() {
    retireAreaRuntime();
    _humanoids.clear();
    _droids.clear();
}

ProjectileSpec *Projectiles::get(ProjectileAttackType attack, CreatureWieldType wield, int appearance) {
    auto droidIt = _droids.find({appearance, attack});
    if (droidIt != _droids.end()) {
        return &droidIt->second;
    }

    switch (wield) {
    case CreatureWieldType::DualSwords:
    case CreatureWieldType::BlasterPistol:
    case CreatureWieldType::DualPistols:
    case CreatureWieldType::BlasterRifle:
    case CreatureWieldType::HeavyWeapon: {
        auto it = _humanoids.find({wield, attack});
        return (it != _humanoids.end()) ? &it->second : nullptr;
    }
    default:
        return nullptr;
    }
}

void Projectiles::launchLightsaberThrow(Creature &caster, const EffectInstance &owner,
                                       Game &game, ServicesView &services) {
    auto weapon = caster.getEquippedItem(InventorySlots::rightWeapon);
    if (!weapon) return;
    auto casterObject = game.getObjectById(caster.id());
    std::vector<std::shared_ptr<Object>> objects {casterObject};
    for (size_t index = 0; index < 3; ++index) {
        auto target = owner.boundObjectParameter(index);
        if (!target) break;
        objects.push_back(std::move(target));
    }
    if (objects.size() == 1) return;
    objects.push_back(casterObject);
    auto spell = services.game.spells.get(static_cast<SpellType>(lightsaberThrowSpell(owner.integerParameter(0))));
    if (!spell || spell->castTime <= 0.0f)
        throw ValidationException("spells.2da: invalid Lightsaber Throw cast time");
    float totalDistance = 0.0f;
    for (size_t index = 1; index < objects.size(); ++index)
        totalDistance += glm::distance(objects[index - 1]->position(), objects[index]->position());
    if (totalDistance <= 0.0f) return;
    const float rate = totalDistance / (spell->castTime * 1000.0f) * 2000.0f;
    std::string modelName = caster.getWeaponModelName(InventorySlots::rightWeapon);
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(caster.sceneNode());
    if (body) if (auto *model = dynamic_cast<scene::ModelSceneNode *>(body->getAttachment("rhand")))
        modelName = model->model().name();
    const int dice = lightsaberThrowDice(caster.getSpellLevel(false));
    uint32_t deadline = 0;
    for (size_t index = 1; index < objects.size(); ++index) {
        auto source = objects[index - 1]; auto target = objects[index];
        const int milliseconds = static_cast<int>(glm::distance(source->position(), target->position()) / rate * 2000.0f);
        auto sourceRef = referenceTo(source.get(), game); auto targetRef = referenceTo(target.get(), game);
        auto active = std::make_shared<ActiveProjectile>();
        auto &route = active->saved;
        route.id = _nextPresentationId++; route.kind = 1;
        route.caster = referenceTo(&caster, game); route.weapon = referenceTo(weapon.get(), game);
        route.spellId = static_cast<int>(spell->type); route.path = 1;
        route.model = modelName; route.initialized = true;
        route.travelRate = rate; route.activationDelay = deadline / 1000.0f;
        route.released = deadline == 0;
        route.legs.push_back({sourceRef, targetRef,
            attachmentPosition(sourceRef, source == casterObject ? "rhand" : "impact", source->position()),
            throwTargetPosition(targetRef, route.caster, target->position()),
            milliseconds / 1000.0f, false});
        _active.push_back(active);
        if (route.released) startLeg(*active, game, services);
        // Each leg starts on its scheduled boundary. Moving targets may change
        // its visual duration, but never reschedule an already queued impact.
        deadline += milliseconds;
        if (target != casterObject) {
            DamagePacket packet;
            int amount = 0;
            for (int die = 0; die < dice; ++die) amount += randomInt(1, 6);
            packet.add(amount, DamageType::Blaster);
            packet.setDamageFlags(static_cast<int>(DamageType::Blaster));
            packet.resolveLightsaberThrow(*target, caster);
            DamageEffect::ApplicationContext context;
            context.damageAmounts[12] = packet.resolvedDamage();
            context.damageAmounts[14] = packet.resolvedDamage();
            context.suppressDamageShields = true;
            auto damage = owner.linkedChild(std::make_shared<DamageEffect>(std::move(packet), context));
            damage.creator = casterObject; damage.creatorId = caster.id();
            damage.setIntegerParameter(DamageEffect::kReactionDelay, 0);
            damage.setDuration(DurationType::Instant, 0.0f);
            game.queueEffectApplication(*target, std::move(damage), deadline);
            auto visual = owner.linkedChild(std::make_shared<VisualEffectMarkerEffect>(6001));
            visual.objectParameters[0] = caster.id(); visual.objectParameterObjects[0] = casterObject;
            visual.setDuration(DurationType::Instant, 0.0f);
            game.queueEffectApplication(*target, std::move(visual), deadline);
        }
    }
}

void Projectiles::attachPresentation(ActiveProjectile &active, Game &game, ServicesView &services, bool restoring) {
    auto &state = active.saved;
    auto caster = state.caster.boundObject();
    auto body = caster ? std::dynamic_pointer_cast<scene::ModelSceneNode>(caster->sceneNode()) : nullptr;
    if (state.kind == 1 && body) {
        auto creature = std::dynamic_pointer_cast<Creature>(caster);
        if (creature && creature->getEquippedItem(InventorySlots::rightWeapon) == state.weapon.boundObject())
            if (auto *hand = body->getAttachment("rhand")) hand->setEnabled(false);
    }
    if ((state.kind == 1 && !state.released) || active.model || !body || state.model.empty()) return;
    auto resource = services.resource.models.get(state.model);
    if (!resource) return;
    active.model = body->graph().newModel(*resource, scene::ModelUsage::Projectile);
    active.model->setLocalTransform(glm::translate(state.position) * glm::mat4_cast(state.orientation));
    if (state.kind == 1) {
        active.model->playAnimation("throwout");
        active.model->setAnimationTime(state.elapsed);
    } else if (!restoring) active.model->signalEvent(kModelEventDetonate);
    body->graph().addRoot(active.model);
}

static void initializeMotion(SavedProjectile &s) {
    auto &leg = s.legs[s.leg];
    s.acceleration = glm::vec3(0.0f);
    if (leg.duration <= 0.0f) { s.velocity = glm::vec3(0.0f); return; }
    const glm::vec3 delta = leg.destination - s.position;
    const float time = leg.duration;
    if (leg.motion == 2) s.acceleration.z = -9.81f;
    else if (leg.motion == 5) s.acceleration = 2.0f * delta / (time * time);
    else if (leg.motion == 10) {
        glm::vec3 arrival(delta.x, delta.y, 0.0f);
        if (glm::length2(arrival) > 0.0f) arrival = glm::normalize(arrival) * 7.5f;
        s.velocity = 2.0f * delta / time - arrival;
        s.acceleration = (arrival - s.velocity) / time;
        return;
    }
    s.velocity = delta / time - 0.5f * s.acceleration * time;
}

void Projectiles::startLeg(ActiveProjectile &active, Game &game, ServicesView &services, bool restoring) {
    auto &s = active.saved;
    if (s.leg >= s.legs.size()) return;
    auto &leg = s.legs[s.leg];
    if (!restoring) {
        s.elapsed = 0.0f;
        if (s.kind == 1) {
            leg.origin = attachmentPosition(leg.source,
                leg.source.boundObject() == s.caster.boundObject() ? "rhand" : "impact", leg.origin);
            leg.destination = throwTargetPosition(leg.target, s.caster, leg.destination);
            s.position = leg.origin;
            auto source = leg.source.boundObject();
            auto target = leg.target.boundObject();
            if (s.travelRate > 0.0f) {
                const auto from = source ? source->position() : leg.origin;
                const auto to = target ? target->position() : leg.destination;
                leg.duration = static_cast<int>(glm::distance(from, to) / s.travelRate * 2000.0f) / 1000.0f;
            }
            if (source) s.orientation = throwOrientation(*source);
            if (active.model) { active.model->graph().removeRoot(*active.model); active.model.reset(); }
        } else {
            leg.origin = s.position;
            if (s.kind == 2) leg.destination = safeLegDestination(s, leg);
        }
        initializeMotion(s);
    }
    attachPresentation(active, game, services, restoring);
}

uint64_t Projectiles::beginSpell(Object &caster, Object *target, const glm::vec3 &position,
                                 const Spell &spell, ProjectilePathType path, Game &game, ServicesView &services) {
    const auto selectedPath = effectiveProjectilePath(spell, path);
    if (!spell.projectile || !spell.projModel || selectedPath == ProjectilePathType::Burst) return 0;
    auto active = std::make_shared<ActiveProjectile>(); auto &s = active->saved;
    s.id = _nextPresentationId++; s.kind = 0; s.spellId = static_cast<int>(spell.type);
    s.caster = referenceTo(&caster, game); s.path = static_cast<int>(selectedPath);
    s.model = spell.projModel->name();
    s.sourceHook = spell.projectileSpawn == "head" ? "headhook" : "rhand";
    s.orientationMode = spell.projectileOrientation == "target" ? 1 : spell.projectileOrientation == "path" ? 2 : 0;
    s.orientation = glm::angleAxis(caster.getFacing(), glm::vec3(0, 0, 1));
    s.position = attachmentPosition(s.caster, s.sourceHook, caster.position());
    s.clockwise = selectedPath == ProjectilePathType::Spiral && randomInt(0, 1) != 0;
    s.initialized = true;
    s.legs.push_back({s.caster, referenceTo(target, game), s.position, position, 0.0f, false});
    attachPresentation(*active, game, services, false);
    _active.push_back(active);
    return s.id;
}

void Projectiles::releaseSpell(uint64_t id, float duration, Game &game, ServicesView &services) {
    for (auto &active : _active) {
        auto &s = active->saved;
        if (s.id != id || s.released || s.kind != 0) continue;
        s.released = true; s.elapsed = 0.0f;
        const auto target = s.legs.front().target;
        const auto origin = attachmentPosition(s.caster, s.sourceHook, s.position);
        const auto destination = attachmentPosition(target, "impact", s.legs.front().destination);
        s.position = origin; s.legs.clear(); s.leg = 0;
        duration = std::max(duration, 0.0f);
        auto append = [&](const glm::vec3 &end, float seconds, int motion,
                          SavedObjectReference follow = {}, glm::vec3 offset = glm::vec3(0)) {
            const auto start = s.legs.empty() ? origin : s.legs.back().destination;
            s.legs.push_back({{}, std::move(follow), start, end, seconds, false, motion, offset});
        };
        auto module = game.module();
        auto area = module ? module->area() : nullptr;
        auto ground = [&](glm::vec3 &point) {
            scene::Collision collision;
            if (!area || !area->graph().testElevation(glm::vec3(point.x, point.y, 1000.0f), collision)) return false;
            point.z = collision.intersection.z; return true;
        };
        switch (static_cast<ProjectilePathType>(s.path)) {
        case ProjectilePathType::HighBallistic: {
            const int halfMs = static_cast<int>(duration * 1000.0f) / 2;
            append(origin + glm::vec3(0, 0, 30), halfMs / 1000.0f, 1);
            append(destination + glm::vec3(0, 0, 30), 0.0f, 0, target, glm::vec3(0, 0, 30));
            append(destination, duration - halfMs / 1000.0f, 1, target);
            break;
        }
        case ProjectilePathType::Spiral: {
            const auto delta = destination - origin;
            glm::vec3 side(-delta.y, delta.x, 0);
            if (!s.clockwise) side = -side;
            if (glm::length2(side) < 0.00001f) side = glm::vec3(0, 1, 0);
            side = glm::normalize(side) * 2.0f;
            append(destination + side, std::max(0.0f, duration - 2.5f), 10, target, side);
            append(destination, std::min(duration, 2.5f), 6, target);
            break;
        }
        case ProjectilePathType::Linked:
            append(destination, duration, 7, target);
            break;
        case ProjectilePathType::Bounce: {
            const auto delta = destination - origin;
            float lateFraction = 0.25f;
            glm::vec3 late;
            do {
                lateFraction -= 0.05f;
                late = destination - delta * lateFraction;
                if (!ground(late)) late.z = (origin.z + destination.z) * 0.5f;
            } while (lateFraction > 0.00001f && destination.z - late.z >= 1.0f);
            if (lateFraction <= 0.00001f) { append(destination, duration, 2); break; }
            float earlyFraction = 0.55f;
            glm::vec3 early;
            do {
                earlyFraction -= 0.05f;
                early = destination - delta * earlyFraction;
                if (!ground(early)) early.z = (origin.z + destination.z) * 0.5f;
            } while (earlyFraction > lateFraction && late.z - early.z >= 0.5f);
            const int total = static_cast<int>(duration * 1000.0f);
            if (earlyFraction > lateFraction) {
                const int last = total * 2 / 10, middle = total * 3 / 10;
                append(early, (total - last - middle) / 1000.0f, 2);
                append(late, middle / 1000.0f, 2);
                append(destination, last / 1000.0f, 2);
            } else {
                const int last = total * 3 / 10;
                append(late, (total - last) / 1000.0f, 2);
                append(destination, last / 1000.0f, 2);
            }
            break;
        }
        case ProjectilePathType::Grenade: {
            if (!area) { append(destination, duration, 2); break; }
            std::array<glm::vec3, 4> contacts {glm::mix(origin, destination, 0.8f),
                glm::mix(origin, destination, 0.9f), glm::mix(origin, destination, 0.95f), destination};
            for (auto &point : contacts) if (!ground(point)) return;
            const int total = static_cast<int>(duration * 1000.0f);
            const int first = static_cast<int>(total * 0.6f), second = static_cast<int>(total * 0.2f);
            const int third = static_cast<int>(total * 0.1f);
            const std::array<int, 4> times {first, second, third, total - first - second - third};
            for (size_t i = 0; i < contacts.size(); ++i) append(contacts[i], times[i] / 1000.0f, 2);
            break;
        }
        default: {
            int motion = 1;
            if (s.path == static_cast<int>(ProjectilePathType::Ballistic)) motion = 2;
            else if (s.path == static_cast<int>(ProjectilePathType::Accelerating)) motion = 5;
            append(destination, duration, motion, target);
            break;
        }
        }
        startLeg(*active, game, services);
        return;
    }
}
void Projectiles::cancelSpell(uint64_t id) {
    _active.erase(std::remove_if(_active.begin(), _active.end(), [id](const auto &p) {
        return p->saved.id == id && p->saved.kind == 0 && !p->saved.released;
    }), _active.end());
}
bool Projectiles::blocksRangedParry(const Creature &creature) const {
    return creature.throwParryBlocked();
}

void Projectiles::launchReflected(Creature &defender, Creature &shooter, const Item &weapon,
                                  Game &game, ServicesView &services) {
    AttackEventFields fields;
    fields.result = static_cast<uint8_t>(AttackResultType::HitSuccessful);
    fields.reactionDelay = static_cast<uint16_t>(glm::distance(defender.position(), shooter.position()) * 1000.0f / 42.0f);
    launchSafeProjectile(defender, shooter, weapon, fields, game, services);
}

static glm::vec3 safeProjectileSourcePosition(Creature &source, uint8_t hand) {
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(source.sceneNode());
    if (body) {
        if (hand == 2) {
            if (auto *hook = body->getNodeByName("impact")) return hook->origin();
        } else {
            const bool offhand = hand == 1;
            if (auto *hook = body->getNodeByName(offhand ? "lbullet" : "rbullet")) return hook->origin();
            auto *weapon = dynamic_cast<scene::ModelSceneNode *>(body->getAttachment(offhand ? "lhand" : "rhand"));
            if (weapon) if (auto *hook = weapon->getNodeByName("bullethook")) return hook->origin();
        }
    }
    return source.position() + glm::vec3(0.0f, 0.0f, 1.25f);
}

static float safeProjectileShieldRadius(const Object &target, ServicesView &services) {
    auto *creature = dyn_cast<Creature>(&target);
    if (!creature) return 0.0f;
    int shield = 0;
    for (const auto &effect : creature->effects()) if (effect.serializedType == 107) {
        shield = effect.integerParameter(0);
        break;
    }
    auto table = services.resource.twoDas.get("forceshields");
    if (!table) return 0.0f;
    const int row = table->indexByLabel(std::to_string(shield));
    if (row < 0) return 0.0f;
    int slot = 0;
    int appearance = 0;
    while (appearance != creature->appearance()) {
        ++slot;
        if (slot > 4) break;
        appearance = table->getInt(row, "appearance_0" + std::to_string(slot), appearance);
    }
    return std::max(0.0f, table->getFloat(row, "radius_0" + std::to_string(slot)));
}

static glm::vec3 safeLegDestination(const SavedProjectile &projectile, const SavedProjectile::Leg &leg) {
    const auto &hook = leg.ownsTargetHook ? leg.targetHook : projectile.targetHook;
    auto endpoint = safeProjectileTargetPosition(leg.target, hook, leg.destination);
    auto approach = leg.origin - endpoint;
    if (leg.stopRadius > 0.0f && glm::length2(approach) > 0.0f)
        endpoint += glm::normalize(approach) * leg.stopRadius;
    return endpoint;
}

void Projectiles::launchSafeProjectile(Creature &source, Object &target, const Item &weapon,
                                     const AttackEventFields &fields, Game &game, ServicesView &services,
                                     uint16_t attackType) {
    auto ammunition = weapon.ammunitionType();
    if (!ammunition || !ammunition->model) return;
    auto p = std::make_shared<ActiveProjectile>();
    auto &s = p->saved;
    s.id = _nextPresentationId++; s.kind = 2; s.path = 1;
    s.released = true; s.initialized = true;
    s.caster = referenceTo(&source, game); s.weapon = referenceTo(&weapon, game);
    s.model = ammunition->model->name();
    s.position = safeProjectileSourcePosition(source, fields.weaponAttackType);
    s.combatResult = fields.result;
    s.soundVariant = attackType == 18 || attackType == 29 || attackType == 82 ? 1 : 0;
    s.orientationMode = 2;
    auto destination = referenceTo(&target, game);
    const auto result = static_cast<AttackResultType>(fields.result);
    const glm::vec3 missedTarget(fields.rangedTarget[0], fields.rangedTarget[1], fields.rangedTarget[2]);
    const bool intercepted = result == AttackResultType::Parried || result == AttackResultType::Deflected ||
        result == AttackResultType::ShieldHit;
    const bool hit = result == AttackResultType::HitSuccessful || result == AttackResultType::CriticalHit ||
        result == AttackResultType::AutomaticHit || result == AttackResultType::AttackResisted;
    SavedProjectile::Leg first;
    first.source = s.caster;
    first.target = hit || intercepted ? destination : SavedObjectReference {};
    first.origin = s.position;
    first.destination = hit || intercepted ? target.position() : missedTarget;
    first.targetHook = intercepted ? "impact_bolt" : "impact";
    first.ownsTargetHook = true;
    first.duration = fields.reactionDelay / 1000.0f;
    if (result == AttackResultType::ShieldHit) {
        first.targetHook.clear();
        first.stopRadius = safeProjectileShieldRadius(target, services);
    }
    first.destination = safeLegDestination(s, first);
    s.legs.push_back(first);
    if (intercepted) {
        SavedProjectile::Leg second;
        second.source = destination;
        second.target = result == AttackResultType::Deflected ? s.caster : SavedObjectReference {};
        second.origin = first.destination;
        second.destination = result == AttackResultType::Deflected ? source.position() : missedTarget;
        second.duration = (result == AttackResultType::Deflected ? fields.reactionDelay : fields.missedBy) / 1000.0f;
        second.targetHook = "impact_bolt";
        second.ownsTargetHook = true;
        second.destination = safeLegDestination(s, second);
        s.legs.push_back(std::move(second));
    }
    startLeg(*p, game, services);
    attachPresentation(*p, game, services, false);
    if (auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(source.sceneNode())) {
        if (ammunition->muzzleFlash) {
            p->flash = body->graph().newModel(*ammunition->muzzleFlash, scene::ModelUsage::Projectile);
            p->flash->setLocalTransform(glm::translate(s.position));
            p->flash->signalEvent(kModelEventDetonate);
            body->graph().addRoot(p->flash);
        }
    }
    if (auto item = std::dynamic_pointer_cast<Item>(s.weapon.boundObject())) item->playShotSound(s.soundVariant, s.position);
    _active.push_back(std::move(p));
}

void Projectiles::releaseHand(const ActiveProjectile &active) {
    const auto &s = active.saved;
    if (s.kind != 1) return;
    auto caster = std::dynamic_pointer_cast<Creature>(s.caster.boundObject());
    if (!caster || caster->getEquippedItem(InventorySlots::rightWeapon) != s.weapon.boundObject()) return;
    for (const auto &other : _active) if (other.get() != &active && other->saved.kind == 1 &&
        other->saved.caster.boundObject() == caster && other->saved.weapon.boundObject() == s.weapon.boundObject()) return;
    auto node = std::dynamic_pointer_cast<scene::ModelSceneNode>(caster->sceneNode());
    if (node) if (auto *hand = node->getAttachment("rhand")) hand->setEnabled(true);
}

void Projectiles::update(float dt, Game &game, ServicesView &services) {
    for (auto it = _active.begin(); it != _active.end();) {
        auto &active = **it; auto &s = active.saved;
        auto caster = s.caster.boundObject();
        if (!caster || (s.kind == 1 && caster->isDead())) {
            releaseHand(active); it = _active.erase(it); continue;
        }
        float frame = std::max(0.0f, dt);
        if (s.kind == 1 && !s.released) {
            const float wait = std::min(frame, s.activationDelay);
            s.activationDelay -= wait;
            frame -= wait;
            if (s.activationDelay <= 0.0f) {
                s.released = true;
                startLeg(active, game, services);
            }
        }
        attachPresentation(active, game, services, true);
        if (!s.released) {
            if (s.kind == 0) s.position = attachmentPosition(s.caster, s.sourceHook, s.position);
        } else {
            while (s.leg < s.legs.size()) {
                auto &leg = s.legs[s.leg];
                const float before = std::max(0.0f, leg.duration - s.elapsed);
                const float step = std::min(frame, before);
                if (s.kind == 1) leg.destination = throwTargetPosition(leg.target, s.caster, leg.destination);
                else if (s.kind == 2) leg.destination = safeLegDestination(s, leg);
                else if (leg.target.boundObject())
                    leg.destination = attachmentPosition(leg.target, "impact", leg.destination - leg.targetOffset) + leg.targetOffset;
                const glm::vec3 delta = leg.destination - s.position;
                const glm::vec3 previous = s.position;
                const bool completed = frame >= before;
                s.elapsed = completed ? leg.duration : s.elapsed + step;
                frame -= step;
                const float remaining = std::max(0.0f, leg.duration - s.elapsed);
                if (completed || remaining <= 0.0f || leg.motion == 0 || leg.motion == 7) {
                    s.position = leg.destination;
                } else if (leg.motion == 6) {
                    glm::vec3 tangent(-delta.y, delta.x, 0.0f);
                    if (glm::length2(tangent) > 0.0f) tangent = glm::normalize(tangent) * (s.clockwise ? 1.0f : -1.0f);
                    s.velocity = delta / remaining + tangent * 7.5f;
                    s.position += s.velocity * step;
                    if (glm::length2(s.position - previous) > glm::length2(delta)) s.position = leg.destination;
                } else {
                    s.velocity = (2.0f * delta - s.acceleration * remaining * remaining) / (2.0f * remaining);
                    s.position += s.velocity * step;
                    if (glm::length2(s.position - previous) > glm::length2(delta)) s.position = leg.destination;
                }
                if (s.orientationMode) {
                    glm::vec3 direction = s.orientationMode == 1 ? leg.destination - s.position : s.position - previous;
                    if (glm::length2(direction) > 0.000001f) s.orientation = glm::rotation(glm::vec3(0, 1, 0), glm::normalize(direction));
                }
                if (s.elapsed < leg.duration) break;
                if (active.model) active.model->setLocalTransform(glm::translate(s.position) * glm::mat4_cast(s.orientation));
                // Presentation completion never applies gameplay damage.
                const bool playImpact = s.kind == 2 && !leg.reacted && leg.target.boundObject();
                leg.reacted = true;
                if (playImpact) if (auto item = std::dynamic_pointer_cast<Item>(s.weapon.boundObject()))
                    item->playImpactSound(s.soundVariant, s.position);
                ++s.leg;
                if (s.leg == s.legs.size()) break;
                startLeg(active, game, services);
            }
        }
        if (s.released && s.leg >= s.legs.size()) {
            releaseHand(active); it = _active.erase(it); continue;
        }
        if (active.model) active.model->setLocalTransform(glm::translate(s.position) * glm::mat4_cast(s.orientation));
        ++it;
    }
}

std::vector<SavedProjectile> Projectiles::savePresentations() const {
    std::vector<SavedProjectile> result;
    for (const auto &p : _active) result.push_back(p->saved);
    return result;
}
void Projectiles::restorePresentations(std::vector<SavedProjectile> records, Game &game, ServicesView &services) {
    retireAreaRuntime();
    std::set<uint64_t> ids;
    for (auto &record : records) {
        if (record.kind == 0 && record.path == static_cast<int>(ProjectilePathType::Burst)) continue;
        if (!record.id || !ids.insert(record.id).second || record.kind < 0 || record.kind > 2 ||
            record.legs.empty() || record.leg < 0 || static_cast<size_t>(record.leg) >= record.legs.size() || !std::isfinite(record.elapsed) || record.elapsed < 0.0f ||
            !record.bindObjectReferences(game)) continue;
        bool valid = std::isfinite(record.activationDelay) && record.activationDelay >= 0.0f &&
            std::isfinite(record.travelRate) && record.travelRate >= 0.0f;
        for (const auto &leg : record.legs) valid &= std::isfinite(leg.duration) && leg.duration >= 0.0f &&
            std::isfinite(leg.stopRadius) && leg.stopRadius >= 0.0f && leg.motion != 9;
        if (!valid) continue;
        auto active = std::make_shared<ActiveProjectile>(); active->saved = std::move(record);
        _nextPresentationId = std::max(_nextPresentationId, active->saved.id + 1);
        _active.push_back(active);
        // No launch, random draw, payment, damage, reaction or item use on this path.
        attachPresentation(*active, game, services, true);
    }
}
void Projectiles::retireAreaRuntime() {
    while (!_active.empty()) {
        auto active = _active.back(); _active.pop_back(); releaseHand(*active);
    }
    _nextPresentationId = 1;
}

} // namespace game
} // namespace reone
