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

#pragma once

#include "../effect.h"

namespace reone {

namespace scene {
class ModelSceneNode;
class MeshSceneNode;
class SceneNode;
}

namespace audio { class AudioSource; }
namespace graphics { class Texture; }

namespace game {

class ServicesView;
struct VisualEffectDesc;

class VisualEffect : public CopyableEffect<VisualEffect> {
public:
    VisualEffect(int visualEffectId, bool missEffect, ServicesView &services);
    VisualEffect(const VisualEffect &other);
    ~VisualEffect();

    EffectApplicationResult onApply(Object &object, EffectInstance &) override;
    EffectRemovalResult onRemove(Object &object, const EffectInstance &) override;
    void onUpdate(Object &object, const EffectInstance &, float dt) override;
    void retireAreaRuntime(
        const std::set<const Object *> &retainedObjects) override;
    void setLocation(glm::vec3 loc) { _location = loc; }
    float duration() const;

private:
    int _visualEffectId;
    bool _missEffect;
    const VisualEffectDesc *_desc {nullptr};
    std::optional<glm::vec3> _location;
    ServicesView &_services;

    std::shared_ptr<scene::ModelSceneNode> _node;
    std::shared_ptr<scene::ModelSceneNode> _attached;
    std::shared_ptr<scene::ModelSceneNode> _attachedOwner;
    scene::SceneNode *_hookNode {nullptr};
    std::weak_ptr<scene::ModelSceneNode> _shellOwner;
    std::shared_ptr<graphics::Texture> _shellTexture;
    std::shared_ptr<audio::AudioSource> _durationSound;
    std::vector<scene::MeshSceneNode *> _beamMeshes;
    std::shared_ptr<scene::ModelSceneNode> _beamSourceOwner;
    std::shared_ptr<scene::ModelSceneNode> _beamTargetOwner;
    bool _beam {false};
    bool _persistent {false};
    EffectApplicationResult present(Object &object, EffectInstance &instance);
    void clearPresentation();
};

} // namespace game

} // namespace reone
