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

#include <algorithm>

#include "reone/game/game.h"
#include "reone/game/effect/visual.h"
#include "reone/audio/mixer.h"
#include "reone/audio/source.h"
#include "reone/game/di/services.h"
#include "reone/game/object.h"
#include "reone/game/object/area.h"
#include "reone/game/object/creature.h"
#include "reone/game/object/door.h"
#include "reone/game/object/placeable.h"
#include "reone/game/visualeffects.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/texture.h"
#include "reone/resource/provider/models.h"
#include "reone/resource/provider/audioclips.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/mesh.h"
#include "reone/scene/node/model.h"
#include "reone/system/randomutil.h"

namespace reone::game {
static float impactDuration(const std::shared_ptr<graphics::Model> &model,
                            const std::shared_ptr<audio::AudioClip> &sound) {
    auto animation = model ? model->getAnimation("impact") : nullptr;
    return std::max(animation ? animation->length() : 0.0f, sound ? sound->duration() : 0.0f);
}
static void collectBeamMeshes(scene::SceneNode &node, std::vector<scene::MeshSceneNode *> &meshes) {
    if (node.type() == scene::SceneNodeType::Mesh) meshes.push_back(static_cast<scene::MeshSceneNode *>(&node));
    for (auto *child : node.children()) collectBeamMeshes(*child, meshes);
}
static std::string placeableNodePrefix(const scene::ModelSceneNode &model) {
    const std::string &name = model.model().name();
    return name.substr(std::min<size_t>(4, name.size()));
}
static std::string visualSiteNode(const Object &object, const scene::ModelSceneNode &model, VisualSite site) {
    if (isa<Placeable>(&object)) {
        static constexpr const char *suffixes[] = {"_ground", "_impact", "_head_hit"};
        return placeableNodePrefix(model) + suffixes[static_cast<int>(site)];
    }
    if (isa<Door>(&object)) {
        static constexpr const char *suffixes[] = {"grnd", "impc", "hhit"};
        return model.model().name() + suffixes[static_cast<int>(site)];
    }
    static constexpr const char *nodes[] = {"root", "impact", "talkdummy"};
    return nodes[static_cast<int>(site)];
}
// On a creature the head site is on its head model when it has one.
static scene::SceneNode *visualSiteHook(const Object &object, scene::ModelSceneNode &body, VisualSite site) {
    scene::ModelSceneNode *owner = &body;
    if (site == VisualSite::Head && isa<Creature>(&object)) {
        if (auto *head = dynamic_cast<scene::ModelSceneNode *>(body.getAttachment("headhook"))) owner = head;
    }
    return owner->getNodeByName(visualSiteNode(object, body, site));
}
// The breath hooks the head model, and the body only when there is no head
// model.
static scene::SceneNode *attachmentHook(scene::ModelSceneNode &body, int program, const char *hook) {
    auto *head = program == 1703 ? dynamic_cast<scene::ModelSceneNode *>(body.getAttachment("headhook")) : nullptr;
    return head ? head->getNodeByName(hook) : body.getNodeByName(hook);
}
// The node a beam starts from on its source, by body part; the source's root
// when its model lacks that node.
static scene::SceneNode *beamSourceNode(const Object &source, scene::ModelSceneNode &model, int bodyNode) {
    std::string name;
    if (isa<Placeable>(&source)) {
        static constexpr const char *suffixes[] = {"_hand", "_impact", "_head"};
        name = placeableNodePrefix(model) + (bodyNode >= 0 && bodyNode <= 2 ? suffixes[bodyNode] : "_ground");
    } else if (isa<Door>(&source)) {
        name = model.model().name() + (bodyNode >= 0 && bodyNode <= 2 ? "impc" : "grnd");
    } else {
        name = beamSourceHook(static_cast<BodyNode>(bodyNode));
    }
    auto *node = model.getNodeByName(name);
    return node ? node : model.getNodeByName("root");
}
// A missed spell is aimed at a point near its target. From farther than 10 m
// the point lies up to 2 m from the target, turned up to 120 degrees either
// way from the direction of the caster, 1 m above the target's feet. From
// closer it lies 10 to 19 m beyond the target, pushed sideways past the
// target's personal space. A miss with no caster is aimed at the world
// origin.
static glm::vec3 spellRangedMissPoint(const Object *caster, const Object &target) {
    if (!caster) return glm::vec3(0.0f);
    float personalSpace = 0.5f;
    if (auto *creature = dyn_cast<Creature>(&target)) personalSpace = creature->creaturePersonalSpace() - 0.1f;
    const float reach = static_cast<float>(randomInt(0, 1999) + 1) / 1000.0f;
    const glm::vec3 &targetPosition = target.position();
    const glm::vec3 toCaster = caster->position() - targetPosition;
    if (glm::length2(toCaster) > 100.0f) {
        const glm::vec3 direction = glm::normalize(toCaster);
        float angle = std::atan(direction.y / direction.x);
        const bool clockwise = randomInt(0, 1) == 1;
        const float turn = static_cast<float>(randomInt(0, 2093)) / 1000.0f;
        angle = clockwise ? angle - turn : angle + turn;
        if (angle > glm::two_pi<float>()) angle -= glm::two_pi<float>();
        if (angle < 0.0f) angle += glm::two_pi<float>();
        glm::vec2 offset(std::cos(angle), std::sin(angle));
        if (direction.x < 0.0f) offset = -offset;
        offset *= reach;
        return glm::vec3(targetPosition.x + offset.x, targetPosition.y + offset.y, targetPosition.z + 1.0f);
    }
    const glm::vec3 away = targetPosition - caster->position();
    const glm::vec3 direction = glm::normalize(away);
    glm::vec2 side = randomInt(0, 1) == 1 ? glm::vec2(direction.y, -direction.x) : glm::vec2(-direction.y, direction.x);
    side *= 1.0f + personalSpace;
    if (glm::length2(away) > 25.0f) {
        side *= 1.0f + static_cast<float>(randomInt(0, static_cast<int>(reach * 1000.0f) - 1)) / 1000.0f;
    }
    const glm::vec3 heading = glm::normalize(glm::vec3(away.x + side.x, away.y + side.y, away.z));
    const float distance = static_cast<float>(randomInt(0, 9)) + 10.0f;
    return glm::vec3(targetPosition.x + distance * heading.x, targetPosition.y + distance * heading.y,
                     2.0f * targetPosition.z + 1.0f);
}
void setShell(scene::ModelSceneNode &body, graphics::Texture *texture) {
    auto apply = [texture](scene::ModelSceneNode &model) {
        if (texture) model.setBumpedOutShell(texture, 0.02f);
        else model.clearBumpedOutShell();
    };
    apply(body);
    for (const char *hook : {"headhook", "rhand"})
        if (auto *model = dynamic_cast<scene::ModelSceneNode *>(body.getAttachment(hook))) apply(*model);
}

VisualEffect::VisualEffect(int visualEffectId, bool missEffect, ServicesView &services) :
    CopyableEffect(EffectType::Visual), _visualEffectId(visualEffectId), _missEffect(missEffect),
    _desc(services.game.visualEffects.get(visualEffectId).value_or(nullptr)), _services(services) {
    setSaveFacingInteger(0, visualEffectId);
    setSaveFacingInteger(2, missEffect ? 1 : 0);
}
VisualEffect::VisualEffect(const VisualEffect &other) :
    CopyableEffect(other), _visualEffectId(other._visualEffectId),
    _missEffect(other._missEffect), _desc(other._desc),
    _location(other._location), _services(other._services) {
    // Scene nodes, beams, shells and sound instances remain application-local.
}
VisualEffect::~VisualEffect() { clearPresentation(); }
void VisualEffect::retireAreaRuntime(const std::set<const Object *> &) {
    // A held motion blur comes back when its holder is presented again.
    const bool motionBlur = _motionBlurProgram;
    clearPresentation();
    _motionBlurProgram = motionBlur;
}
void VisualEffect::clearPresentation(Object *holder) {
    if (_durationSound) { _durationSound->stop(); _durationSound.reset(); }
    for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(nullptr);
    _beamMeshes.clear();
    _beamSourceOwner.reset();
    _beamTargetOwner.reset();
    if (_node) { _node->graph().removeRoot(*_node); _node.reset(); }
    if (_beamEnd) { _beamEnd->graph().removeRoot(*_beamEnd); _beamEnd.reset(); }
    for (auto &nodeModel : _nodeModels) {
        if (nodeModel.hook && !nodeModel.followsPositionOnly) nodeModel.hook->removeChild(*nodeModel.model);
        else nodeModel.model->graph().removeRoot(*nodeModel.model);
    }
    _nodeModels.clear();
    stopProgram(holder);
}
// The impact presentation of a visual shown at a point in the world.
float VisualEffect::duration() const {
    return _desc ? impactDuration(_desc->locationModel, _desc->soundImpact) : 0.0f;
}
EffectApplicationResult VisualEffect::onApply(Object &object, EffectInstance &instance) {
    // A reusable VM descriptor must not share render nodes between applications.
    auto runtime = std::make_shared<VisualEffect>(instance.integerParameter(0),
        instance.integerParameter(2) != 0, _services);
    instance.effect = runtime;
    return runtime->present(object, instance);
}
// A beam or spell visual comes from object parameter 0, or from the creator
// when that parameter holds no object.
static constexpr int kSpeedKnightVisual = 1020;
static std::shared_ptr<Object> visualSource(const EffectInstance &instance) {
    auto source = instance.boundObjectParameter(0);
    return source ? source : instance.boundCreator();
}
float VisualEffect::impactProgramLength(int program) const {
    // Every impact program lasts one second, except the hand fizzle, which
    // lasts as long as its model's impact animation.
    if (program == 1201) {
        if (auto model = _services.resource.models.get(spellVisualForProgram(program)->model)) {
            if (auto animation = model->getAnimation("impact")) return animation->length();
        }
    }
    // The widened view on the player lasts 4.75 s.
    if (program == 1500 && _viewAngleHold) return 4.75f;
    return 1.0f;
}
// Program 1500 on the player: the view angle widens to 135 degrees over
// 0.75 s with the dolly zoom out of combat, narrows back from the moment
// 4 s remain, and the saved angle returns when the program ends.
static constexpr float kWideViewAngle = 135.0f;
static constexpr float kWideningSeconds = 0.75f;
static constexpr float kNarrowingRemaining = 4.0f;
bool VisualEffect::startViewAngleProgram(Object &object) {
    auto *active = object.game().getActiveCamera();
    if (!active || object.game().party().getLeader().get() != &object) return false;
    auto camera = std::dynamic_pointer_cast<Camera>(object.game().getObjectById(active->id()));
    if (!camera) return false;
    _viewAngleCamera = camera;
    _viewAngleHold = camera->beginViewAngleHold();
    camera->beginViewAngleAnimation(kWideViewAngle, kWideningSeconds);
    _viewAngleNarrowed = false;
    return true;
}
void VisualEffect::updateViewAngleProgram(Object &object) {
    if (!_viewAngleHold) return;
    auto camera = _viewAngleCamera.lock();
    // A pause has already returned the angle.
    if (!camera || !camera->holdsViewAngle(_viewAngleHold)) {
        _viewAngleHold = 0;
        return;
    }
    if (object.game().party().getLeader().get() != &object) {
        stopViewAngleProgram();
        return;
    }
    if (!_viewAngleNarrowed && _impactRemaining <= kNarrowingRemaining) {
        camera->endViewAngleAnimation();
        camera->beginViewAngleAnimation(camera->heldViewAngle(), _impactRemaining);
        _viewAngleNarrowed = true;
    }
}
void VisualEffect::stopViewAngleProgram() {
    if (auto camera = _viewAngleCamera.lock()) camera->releaseViewAngleHold(_viewAngleHold);
    _viewAngleCamera.reset();
    _viewAngleHold = 0;
}
// Programs 1601 and 1602 hold the motion blur. It follows the player's drive
// speed every frame, and leaves while the holder is not the player or is
// debilitated.
static constexpr float kMotionBlurRatio = 0.75f;
void VisualEffect::startMotionBlurProgram(Object &object) {
    auto &game = object.game();
    _motionBlurGame = &game;
    _motionBlurProgram = true;
    game.addMotionBlurProgram(game.party().getLeader().get() == &object);
    _motionBlurApplied = true;
}
void VisualEffect::updateMotionBlurProgram(Object &object) {
    auto &game = object.game();
    auto *creature = dyn_cast<Creature>(&object);
    const bool player = creature && game.party().getLeader().get() == &object;
    if (!player || creature->isDebilitated()) {
        if (_motionBlurApplied) game.removeMotionBlurProgram();
        _motionBlurApplied = false;
        return;
    }
    if (!_motionBlurApplied) {
        game.addMotionBlurProgram(true);
        _motionBlurApplied = true;
    }
    const float speed = creature->driveSpeed() / creature->driveMaxSpeed();
    game.setSpeedBlurRatio(speed > 1.0f ? kMotionBlurRatio : kMotionBlurRatio * speed);
}
bool VisualEffect::startProgram(int program, Object &object, const EffectInstance &instance) {
    if (program == 1500) return startViewAngleProgram(object);
    if (program == 1601 || program == 1602) {
        startMotionBlurProgram(object);
        return true;
    }
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(object.sceneNode());
    if (!body) return false;
    const bool tsl = object.game().isTSL();
    if (const char *beamName = beamModelForProgram(program, tsl); *beamName) {
        std::shared_ptr<scene::ModelSceneNode> sourceModel;
        std::shared_ptr<scene::ModelSceneNode> targetModel;
        scene::SceneNode *sourceNode = nullptr;
        scene::SceneNode *targetNode = nullptr;
        const bool ends = resolveBeamEnds(object, instance, sourceModel, sourceNode, targetModel, targetNode);
        auto model = ends ? _services.resource.models.get(beamName) : nullptr;
        if (!model) return false;
        _beam = true;
        _beamSourceOwner = sourceModel;
        _beamTargetOwner = targetModel;
        _node = body->graph().newModel(*model, scene::ModelUsage::Projectile);
        _node->setCullingEnabled(false);
        _node->setLocalTransform(sourceNode->absoluteTransform());
        _node->playAnimation("cast01");
        collectBeamMeshes(*_node, _beamMeshes);
        for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(targetNode);
        body->graph().addRoot(_node);
        return true;
    }
    if (const std::string texture = shieldTextureForProgram(program); !texture.empty()) {
        _shellTexture = _services.resource.textures.get(texture, graphics::TextureUsage::MainTex);
        if (!_shellTexture) return false;
        _shellOwner = body;
        setShell(*body, _shellTexture.get());
        return true;
    }
    const auto *spellVisual = spellVisualForProgram(program);
    const auto *attachment = spellVisual ? spellVisual : modelAttachmentForProgram(program, tsl);
    if (!attachment) return false;
    auto model = _services.resource.models.get(attachment->model);
    auto *hook = attachmentHook(*body, program, attachment->hook);
    if (!model || !hook) return false;
    _hookNode = hook;
    _attachedOwner = body;
    _attachmentProgram = program;
    if (program == 1703) {
        auto module = object.game().module();
        _releaseArea = module ? module->area() : nullptr;
    }
    _attached = body->graph().newModel(*model, scene::ModelUsage::Projectile);
    _hookNode->addChild(*_attached);
    if (program == 1202) {
        // The resist visual faces its source.
        if (auto source = visualSource(instance)) {
            glm::vec3 direction = source->position() - _hookNode->origin();
            if (glm::length2(direction) >= 1e-4f) {
                float facing = glm::half_pi<float>() - glm::atan(direction.x, direction.y);
                _attached->setLocalTransform(_hookNode->absoluteTransformInverse() *
                    glm::translate(_hookNode->origin()) * glm::eulerAngleZ(facing));
            }
        }
    }
    if (spellVisual) _attached->playAnimation("impact");
    else if (program == 1703) _attached->playAnimation("impact", nullptr,
        scene::AnimationProperties::fromFlags(scene::AnimationFlags::loop));
    return true;
}
// A beam runs from its source's node for the body part to the target's
// impact node, or its root when the model lacks one. A missed beam runs to its
// stand-in instead.
bool VisualEffect::resolveBeamEnds(Object &object, const EffectInstance &instance,
                                   std::shared_ptr<scene::ModelSceneNode> &sourceModel, scene::SceneNode *&sourceNode,
                                   std::shared_ptr<scene::ModelSceneNode> &targetModel, scene::SceneNode *&targetNode) {
    auto source = visualSource(instance);
    sourceModel = source ? std::dynamic_pointer_cast<scene::ModelSceneNode>(source->sceneNode()) : nullptr;
    sourceNode = sourceModel ? beamSourceNode(*source, *sourceModel, instance.integerParameter(1)) : nullptr;
    if (!_beamEndNode.empty()) {
        if (!_beamEnd) return false;
        targetModel = _beamEnd;
        targetNode = _beamEnd->getNodeByName(_beamEndNode);
    } else {
        targetModel = std::dynamic_pointer_cast<scene::ModelSceneNode>(object.sceneNode());
        targetNode = targetModel ? targetModel->getNodeByName(visualSiteNode(object, *targetModel, VisualSite::Impact)) : nullptr;
    }
    if (targetModel && !targetNode) targetNode = targetModel->getNodeByName("root");
    return sourceNode && targetNode;
}
void VisualEffect::showLatestShell(Object &holder) {
    const VisualEffect *latest = nullptr;
    uint64_t latestOrder = 0;
    for (const auto &applied : holder.effects()) {
        auto *visual = dynamic_cast<const VisualEffect *>(applied.effect.get());
        if (visual && visual->_shellTexture && !visual->_shellOwner.expired() &&
            (!latest || applied.applicationOrder >= latestOrder)) {
            latest = visual;
            latestOrder = applied.applicationOrder;
        }
    }
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(holder.sceneNode());
    // A creature's own shell began after every application ordered before it.
    if (auto *creature = dyn_cast<Creature>(&holder)) {
        uint64_t order = 0;
        if (auto *own = creature->latestOwnShell(order); own && (!latest || order > latestOrder)) {
            if (body) setShell(*body, own);
            return;
        }
    }
    if (latest) {
        if (auto owner = latest->_shellOwner.lock()) setShell(*owner, latest->_shellTexture.get());
    } else if (body) {
        setShell(*body, nullptr);
    }
}
void VisualEffect::stopProgram(Object *holder) {
    // The removed breath holds the first frame of its stop animation where
    // its hook last was, and the area keeps it until its emitter empties.
    auto releaseArea = _attachmentProgram == 1703 ? _releaseArea.lock() : nullptr;
    if (_hookNode && _attached && releaseArea) {
        scene::AnimationProperties hold;
        hold.speed = 0.0f;
        _attached->playAnimation("stop", nullptr, hold);
        const glm::mat4 world = _attached->absoluteTransform();
        _hookNode->removeChild(*_attached);
        _attached->setLocalTransform(world);
        _attached->graph().addRoot(_attached);
        releaseArea->releaseEffectModel(std::move(_attached));
    } else if (_hookNode && _attached) {
        _hookNode->removeChild(*_attached);
    }
    _attached.reset(); _hookNode = nullptr; _attachedOwner.reset();
    _attachmentProgram = -1;
    _releaseArea.reset();
    if (_viewAngleHold) stopViewAngleProgram();
    if (_motionBlurApplied && _motionBlurGame) _motionBlurGame->removeMotionBlurProgram();
    _motionBlurApplied = false;
    _motionBlurProgram = false;
    // Ending a shell empties the slot. The latest shell left then shows again,
    // unless the ended shell belonged to a kept application and was not the
    // one showing.
    if (auto body = _shellOwner.lock()) {
        const bool shown = body->bumpedOutShellTexture() == _shellTexture.get();
        setShell(*body, nullptr);
        _shellOwner.reset(); _shellTexture.reset();
        if (holder && (shown || !_kept)) showLatestShell(*holder);
    }
    _shellOwner.reset(); _shellTexture.reset();
}
// A node model plays its impact animation. Without one, a kept application
// goes straight to its looping duration animation and any other holds the
// model for a second.
static constexpr float kNodeModelHold = 1.0f;
void VisualEffect::startNodeModel(scene::ISceneGraph &graph, graphics::Model &model, scene::SceneNode *hook,
                                  VisualSite site, std::shared_ptr<scene::ModelSceneNode> owner,
                                  const glm::vec3 &position, bool restoring) {
    NodeModel nodeModel;
    nodeModel.model = graph.newModel(model, scene::ModelUsage::Projectile);
    nodeModel.owner = std::move(owner);
    nodeModel.site = site;
    nodeModel.hook = hook;
    nodeModel.followsPositionOnly = hook && _desc->orientationOff;
    if (hook && !nodeModel.followsPositionOnly) {
        hook->addChild(*nodeModel.model);
    } else {
        graph.addRoot(nodeModel.model);
        nodeModel.model->setLocalTransform(glm::translate(hook ? hook->origin() : position));
    }
    auto impact = restoring ? nullptr : model.getAnimation("impact");
    if (impact) {
        nodeModel.model->playAnimation("impact");
        nodeModel.impactRemaining = impact->length();
    } else if (_kept) {
        nodeModel.model->playAnimation("duration", nullptr,
            scene::AnimationProperties::fromFlags(scene::AnimationFlags::loop));
    } else {
        nodeModel.impactRemaining = kNodeModelHold;
    }
    _nodeModels.push_back(std::move(nodeModel));
}
// The root model goes to the object's root, chosen by its size (an object
// that is not a creature counts as medium), the impact model to its impact
// node and the head model to its head: on a creature, the talk dummy of its
// head model when it has one. A model whose node is missing is not shown.
void VisualEffect::attachNodeModels(Object &object, const std::shared_ptr<scene::ModelSceneNode> &body, bool restoring) {
    auto *creature = dyn_cast<Creature>(&object);
    const CreatureSize size = creature ? creature->size() : CreatureSize::Medium;
    const std::pair<const std::shared_ptr<graphics::Model> *, VisualSite> models[] = {
        {&_desc->rootModel(size), VisualSite::Root},
        {&_desc->impactModel, VisualSite::Impact},
        {&_desc->headModel, VisualSite::Head}};
    for (const auto &[model, site] : models) {
        if (!*model) continue;
        auto *hook = visualSiteHook(object, *body, site);
        if (!hook) continue;
        startNodeModel(body->graph(), **model, hook, site, body, glm::vec3(0.0f), restoring);
    }
}
void VisualEffect::detachFromBody() {
    for (auto &nodeModel : _nodeModels) {
        if (!nodeModel.owner || !nodeModel.hook) continue;
        if (nodeModel.followsPositionOnly) nodeModel.model->graph().removeRoot(*nodeModel.model);
        else nodeModel.hook->removeChild(*nodeModel.model);
        nodeModel.hook = nullptr;
    }
    if (_hookNode && _attached) _hookNode->removeChild(*_attached);
    _hookNode = nullptr;
}
void VisualEffect::reattachToBody(Object &object, scene::ModelSceneNode &body) {
    for (auto &nodeModel : _nodeModels) {
        if (!nodeModel.owner) continue;
        nodeModel.hook = visualSiteHook(object, body, nodeModel.site);
        if (!nodeModel.hook) continue;
        if (nodeModel.followsPositionOnly) {
            nodeModel.model->setLocalTransform(glm::translate(nodeModel.hook->origin()));
            body.graph().addRoot(nodeModel.model);
        } else {
            nodeModel.hook->addChild(*nodeModel.model);
        }
    }
    if (_attached) {
        const auto *spellVisual = spellVisualForProgram(_attachmentProgram);
        const auto *attachment = spellVisual ? spellVisual
                                             : modelAttachmentForProgram(_attachmentProgram, object.game().isTSL());
        _hookNode = attachmentHook(body, _attachmentProgram, attachment->hook);
        if (_hookNode) _hookNode->addChild(*_attached);
    }
}
void VisualEffect::updateNodeModels(float dt) {
    for (auto it = _nodeModels.begin(); it != _nodeModels.end();) {
        if (it->followsPositionOnly && it->hook) it->model->setLocalTransform(glm::translate(it->hook->origin()));
        if (it->impactRemaining > 0.0f) {
            it->impactRemaining -= dt;
            if (it->impactRemaining <= 0.0f) {
                if (!_kept) {
                    if (it->hook && !it->followsPositionOnly) it->hook->removeChild(*it->model);
                    else it->model->graph().removeRoot(*it->model);
                    it = _nodeModels.erase(it);
                    continue;
                }
                it->model->playAnimation("duration", nullptr,
                    scene::AnimationProperties::fromFlags(scene::AnimationFlags::loop));
            }
        }
        ++it;
    }
}
float VisualEffect::nodeModelsImpactLength() const {
    float length = 0.0f;
    for (const auto &nodeModel : _nodeModels) length = std::max(length, nodeModel.impactRemaining);
    return length;
}
EffectApplicationResult VisualEffect::present(Object &object, EffectInstance &instance) {
    // A second 1020 on an object that still carries one is not shown at all.
    if (_visualEffectId == kSpeedKnightVisual && std::any_of(object.effects().begin(), object.effects().end(),
            [](const EffectInstance &applied) {
                return applied.type() == EffectType::Visual && applied.integerParameter(0) == kSpeedKnightVisual;
            })) {
        return instance.durationType() == DurationType::Instant ? EffectApplicationResult::Applied
                                                                 : EffectApplicationResult::Retained;
    }
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(object.sceneNode());
    scene::ISceneGraph *graph = body ? &body->graph() : nullptr;
    const bool tsl = object.game().isTSL();
    // Only a timed application that hit, of a row that is not fire-and-forget,
    // is kept on the object. Any other is shown once as an impact, which never
    // runs the duration program; a beam shown once still runs it for the
    // application's duration.
    const bool beamRow = _desc && *beamModelForProgram(_desc->progFXDuration, tsl);
    _kept = instance.durationType() != DurationType::Instant && !_missEffect && !(_desc && _desc->fireAndForget);
    const bool runsDuration = _kept || (beamRow && instance.duration != 0.0f);
    const int durationProgram = _desc && runsDuration ? _desc->progFXDuration : -1;
    // A miss is aimed at a point near the target: a missed beam ends at a
    // stand-in there, reaching its impact node when the point is 1 m above
    // the target's feet and its root otherwise; any other missed visual is
    // shown at the point. The second title still draws the point's random
    // terms but aims every miss at the world origin.
    if (_missEffect) {
        auto caster = instance.boundCreator();
        const glm::vec3 drawnPoint = spellRangedMissPoint(caster.get(), object);
        const glm::vec3 missPoint = tsl ? glm::vec3(0.0f) : drawnPoint;
        if (beamRow) {
            _beamEndNode = missPoint.z == object.position().z + 1.0f ? "impact" : "root";
            auto standIn = graph ? _services.resource.models.get("c_dummy") : nullptr;
            if (standIn) {
                _beamEnd = graph->newModel(*standIn, scene::ModelUsage::Projectile);
                graph->addRoot(_beamEnd);
                _beamEnd->setLocalTransform(glm::translate(missPoint));
            }
        } else {
            _location = missPoint;
        }
    }
    const glm::vec3 position = _location.value_or(object.position());
    float presentationDuration = 0.0f;

    if (_desc && graph) {
        // The impact program runs first and the duration program follows it.
        // Without an impact program the duration program starts at once.
        const int impactProgram = instance.restoring ? -1 : _desc->progFXImpact;
        if (impactProgram >= 0) {
            startProgram(impactProgram, object, instance);
            _impactRemaining = impactProgramLength(impactProgram);
            _pendingDurationProgram = durationProgram;
            presentationDuration = _impactRemaining;
        } else if (durationProgram >= 0) {
            startProgram(durationProgram, object, instance);
        }
        // A restored application only brings back what it keeps showing.
        if (!_beam && (!instance.restoring || _kept)) {
            if (_location) {
                if (_desc->locationModel) {
                    startNodeModel(*graph, *_desc->locationModel, nullptr, VisualSite::Root, nullptr, position,
                                   instance.restoring);
                }
            } else if (body) {
                attachNodeModels(object, body, instance.restoring);
            }
        }
        presentationDuration = std::max(presentationDuration, nodeModelsImpactLength());
        // The impact sound plays where the visual lands: a missed beam's
        // stand-in, otherwise the object or point.
        if (!instance.restoring && _desc->soundImpact) {
            const glm::vec3 soundPosition = _beamEnd ? _beamEnd->origin() : position;
            _services.audio.mixer.play(_desc->soundImpact, audio::AudioType::Sound, 1.0f, false, soundPosition);
            presentationDuration = std::max(presentationDuration, _desc->soundImpact->duration());
        }
        if (_desc->soundDuration)
            _durationSound = _services.audio.mixer.play(_desc->soundDuration, audio::AudioType::Sound, 1.0f, true, position);
    }
    if (_kept) return EffectApplicationResult::Retained;
    // An application shown once is not kept: it lasts as long as what it
    // shows, and a beam shown once lasts for the application's duration.
    if (runsDuration && _beam) presentationDuration = std::max(presentationDuration, instance.duration);
    if (presentationDuration <= 0.0f) return EffectApplicationResult::Applied;
    instance.setDuration(DurationType::Temporary, presentationDuration);
    return EffectApplicationResult::Retained;
}
void VisualEffect::onUpdate(Object &object, const EffectInstance &instance, float dt) {
    if (_impactRemaining > 0.0f) {
        _impactRemaining -= dt;
        if (_impactRemaining <= 0.0f) {
            stopProgram(&object);
            if (_pendingDurationProgram >= 0) startProgram(_pendingDurationProgram, object, instance);
            _pendingDurationProgram = -1;
        } else {
            updateViewAngleProgram(object);
        }
    }
    if (_motionBlurProgram && _impactRemaining <= 0.0f) updateMotionBlurProgram(object);
    updateNodeModels(dt);
    if (_durationSound) _durationSound->setPosition(object.position());
    if (_beam) {
        std::shared_ptr<scene::ModelSceneNode> sourceModel;
        std::shared_ptr<scene::ModelSceneNode> targetModel;
        scene::SceneNode *sourceNode = nullptr;
        scene::SceneNode *targetNode = nullptr;
        resolveBeamEnds(object, instance, sourceModel, sourceNode, targetModel, targetNode);
        if (sourceNode && _node) _node->setLocalTransform(sourceNode->absoluteTransform());
        for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(sourceNode ? targetNode : nullptr);
        _beamSourceOwner = sourceModel;
        _beamTargetOwner = targetModel;
    }
}
void VisualEffect::onRemove(Object &object, const EffectInstance &) {
    clearPresentation(&object);
}
} // namespace reone::game
