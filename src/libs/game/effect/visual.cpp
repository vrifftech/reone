/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/game/game.h"
#include "reone/game/effect/visual.h"
#include "reone/audio/mixer.h"
#include "reone/audio/source.h"
#include "reone/game/di/services.h"
#include "reone/game/object.h"
#include "reone/game/object/area.h"
#include "reone/game/visualeffects.h"
#include "reone/game/visualrules.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/model.h"
#include "reone/graphics/texture.h"
#include "reone/resource/provider/models.h"
#include "reone/resource/provider/audioclips.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/mesh.h"
#include "reone/scene/node/model.h"

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
static void setShell(scene::ModelSceneNode &body, graphics::Texture *texture) {
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
void VisualEffect::retireAreaRuntime(const std::set<const Object *> &) { clearPresentation(); }
void VisualEffect::clearPresentation() {
    if (_durationSound) { _durationSound->stop(); _durationSound.reset(); }
    for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(nullptr);
    _beamMeshes.clear();
    _beamSourceOwner.reset();
    _beamTargetOwner.reset();
    if (_node) { _node->graph().removeRoot(*_node); _node.reset(); }
    if (_hookNode && _attached) _hookNode->removeChild(*_attached);
    _attached.reset(); _hookNode = nullptr; _attachedOwner.reset();
    if (auto body = _shellOwner.lock()) setShell(*body, nullptr);
    _shellOwner.reset(); _shellTexture.reset();
}
float VisualEffect::duration() const {
    return _desc ? impactDuration(_desc->impRootMNode, _desc->soundImpact) : 0.0f;
}
EffectApplicationResult VisualEffect::onApply(Object &object, EffectInstance &instance) {
    // A reusable VM descriptor must not share render nodes between applications.
    auto runtime = std::make_shared<VisualEffect>(instance.integerParameter(0),
        instance.integerParameter(2) != 0, _services);
    runtime->_location = _location;
    instance.effect = runtime;
    return runtime->present(object, instance);
}
EffectApplicationResult VisualEffect::present(Object &object, EffectInstance &instance) {
    auto body = std::dynamic_pointer_cast<scene::ModelSceneNode>(object.sceneNode());
    scene::ISceneGraph *graph = body ? &body->graph() : nullptr;
    if (auto *area = dyn_cast<Area>(&object)) graph = &area->graph();
    float presentationDuration = duration();
    _persistent = _desc && (*beamModelForProgram(_desc->progFXDuration, object.game().isTSL()) ||
        !shieldTextureForProgram(_desc->progFXDuration).empty());
    const glm::vec3 position = _location.value_or(object.position());

    if (_desc && graph) {
        const char *beamName = beamModelForProgram(_desc->progFXDuration, object.game().isTSL());
        auto source = instance.boundObjectParameter(0);
        auto sourceModel = source ? std::dynamic_pointer_cast<scene::ModelSceneNode>(source->sceneNode()) : nullptr;
        auto *sourceNode = sourceModel ? sourceModel->getNodeByName(beamSourceHook(
            static_cast<BodyNode>(instance.integerParameter(1)))) : nullptr;
        auto *targetNode = body ? body->getNodeByName("impact") : nullptr;
        if (*beamName && sourceNode && targetNode) {
            auto model = _services.resource.models.get(beamName);
            if (model) {
                _beam = true;
                _beamSourceOwner = sourceModel;
                _beamTargetOwner = body;
                _node = graph->newModel(*model, scene::ModelUsage::Projectile);
                _node->setCullingEnabled(false);
                _node->setLocalTransform(sourceNode->absoluteTransform());
                _node->playAnimation("cast01");
                collectBeamMeshes(*_node, _beamMeshes);
                for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(targetNode);
                graph->addRoot(_node);
            }
        } else if (_desc->impRootMNode && (!instance.restoring || _persistent)) {
            _node = graph->newModel(*_desc->impRootMNode, scene::ModelUsage::Projectile);
            graph->addRoot(_node);
            _node->setLocalTransform(glm::translate(position));
            if (_persistent) _node->playAnimation("duration", nullptr,
                scene::AnimationProperties::fromFlags(scene::AnimationFlags::loop));
            else _node->playAnimation("impact");
        }
        if (body) {
            const std::string texture = shieldTextureForProgram(_desc->progFXDuration);
            if (!texture.empty()) {
                _shellTexture = _services.resource.textures.get(texture, graphics::TextureUsage::MainTex);
                if (_shellTexture) { _shellOwner = body; setShell(*body, _shellTexture.get()); }
            }
        }
        if (!instance.restoring && _desc->soundImpact && !_beam)
            _services.audio.mixer.play(_desc->soundImpact, audio::AudioType::Sound, 1.0f, false, position);
        if (_desc->soundDuration)
            _durationSound = _services.audio.mixer.play(_desc->soundDuration, audio::AudioType::Sound, 1.0f, true, position);
    }
    // Impact-only attachments are not replayed when restoring a retained visual.
    if (!instance.restoring && body && (_visualEffectId == 4036 || _visualEffectId == 4037)) {
        const bool resisted = _visualEffectId == 4037;
        auto model = _services.resource.models.get(resisted ? "fxresist" : "fxfail");
        auto sound = _services.resource.audioClips.get(resisted ? "v_fresist_imp" : "v_fizzle_imp");
        presentationDuration = std::max(presentationDuration, impactDuration(model, sound));
        _hookNode = body->getNodeByName(resisted ? "impact" : "handconjure");
        if (model && _hookNode) {
            _attachedOwner = body;
            _attached = graph->newModel(*model, scene::ModelUsage::Projectile);
            _hookNode->addChild(*_attached);
            if (resisted) {
                if (auto source = instance.boundObjectParameter(0)) {
                    glm::vec3 direction = source->position() - _hookNode->origin();
                    if (glm::length2(glm::vec2(direction)) > 0.0f) {
                        float facing = glm::half_pi<float>() - glm::atan(direction.x, direction.y);
                        _attached->setLocalTransform(_hookNode->absoluteTransformInverse() *
                            glm::translate(_hookNode->origin()) * glm::eulerAngleZ(facing));
                    }
                }
            }
            _attached->playAnimation("impact");
            if (sound) _services.audio.mixer.play(sound, audio::AudioType::Sound, 1.0f, false, _hookNode->origin());
        }
    }
    if (instance.durationType() == DurationType::Instant && presentationDuration > 0.0f)
        instance.setDuration(DurationType::Temporary, presentationDuration);
    return instance.durationType() == DurationType::Instant ? EffectApplicationResult::Applied : EffectApplicationResult::Retained;
}
void VisualEffect::onUpdate(Object &object, const EffectInstance &instance, float) {
    if (_durationSound) _durationSound->setPosition(object.position());
    if (_beam) {
        auto source = instance.boundObjectParameter(0);
        auto sourceModel = source ? std::dynamic_pointer_cast<scene::ModelSceneNode>(source->sceneNode()) : nullptr;
        auto targetModel = std::dynamic_pointer_cast<scene::ModelSceneNode>(object.sceneNode());
        auto *sourceNode = sourceModel ? sourceModel->getNodeByName(beamSourceHook(static_cast<BodyNode>(instance.integerParameter(1)))) : nullptr;
        auto *targetNode = targetModel ? targetModel->getNodeByName("impact") : nullptr;
        if (sourceNode && _node) _node->setLocalTransform(sourceNode->absoluteTransform());
        for (auto *mesh : _beamMeshes) mesh->setProjectedBeamTarget(sourceNode ? targetNode : nullptr);
        _beamSourceOwner = sourceModel;
        _beamTargetOwner = targetModel;
    } else if (_persistent && _node) _node->setLocalTransform(object.transform());
}
EffectRemovalResult VisualEffect::onRemove(Object &object, const EffectInstance &) {
    clearPresentation();
    if (_desc && _desc->soundCessation)
        _services.audio.mixer.play(_desc->soundCessation, audio::AudioType::Sound, 1.0f, false, object.position());
    return EffectRemovalResult::Removed;
}
} // namespace reone::game
