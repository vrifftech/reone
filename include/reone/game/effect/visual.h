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
class ISceneGraph;
class ModelSceneNode;
class MeshSceneNode;
class SceneNode;
}

namespace audio { class AudioSource; }
namespace graphics { class Model; class Texture; }

namespace game {

class Area;
class Camera;
class Game;
struct ServicesView;
struct VisualEffectDesc;

// Where a visual's models attach on an object. Creatures name their nodes;
// a placeable prefixes its with its model name after the first four
// characters, a door with its whole model name.
enum class VisualSite {
    Root,
    Impact,
    Head
};

/** Wrap a body and its head and hand models in a bumped-out shell, or clear it. */
void setShell(scene::ModelSceneNode &body, graphics::Texture *texture);

class VisualEffect : public CopyableEffect<VisualEffect> {
public:
    VisualEffect(int visualEffectId, bool missEffect, ServicesView &services);
    VisualEffect(const VisualEffect &other);
    ~VisualEffect();

    EffectApplicationResult onApply(Object &object, EffectInstance &) override;
    void onRemove(Object &object, const EffectInstance &) override;
    void onUpdate(Object &object, const EffectInstance &, float dt) override;
    void retireAreaRuntime(
        const std::set<const Object *> &retainedObjects) override;
    // A visual sent to an area carries the point it plays at in floats 0-2.
    void setLocation(glm::vec3 loc) {
        for (size_t i = 0; i < 3; ++i) setSaveFacingFloat(i, loc[static_cast<glm::length_t>(i)]);
    }
    float duration() const;
    /**
     * Shells share one slot on a body: the latest shell live on the holder
     * shows, whether a visual effect's or the creature's own, and with none
     * the slot is empty.
     */
    static void showLatestShell(Object &holder);
    /**
     * A rebuilt body takes back what the visual shows on it: its models come
     * off the old body and go back to the same nodes of the new one, carrying
     * on where they were. A model whose node the new body lacks is not shown.
     */
    void detachFromBody();
    void reattachToBody(Object &object, scene::ModelSceneNode &body);

private:
    // A model attached to one of the target's nodes: it plays its impact
    // animation, then its duration animation while the application is kept,
    // or goes away when it is not.
    struct NodeModel {
        std::shared_ptr<scene::ModelSceneNode> model;
        // No owner: the model stands at a point in the world.
        std::shared_ptr<scene::ModelSceneNode> owner;
        VisualSite site {VisualSite::Root};
        // No hook on a body's model: the body lacks its node.
        scene::SceneNode *hook {nullptr};
        bool followsPositionOnly {false};
        float impactRemaining {0.0f};
    };

    int _visualEffectId;
    bool _missEffect;
    const VisualEffectDesc *_desc {nullptr};
    std::optional<glm::vec3> _location;
    ServicesView &_services;

    std::shared_ptr<scene::ModelSceneNode> _node;
    std::vector<NodeModel> _nodeModels;
    // A missed beam ends at a point: a stand-in model there, and the node of
    // it the beam reaches.
    std::shared_ptr<scene::ModelSceneNode> _beamEnd;
    std::string _beamEndNode;
    std::shared_ptr<scene::ModelSceneNode> _attached;
    std::shared_ptr<scene::ModelSceneNode> _attachedOwner;
    scene::SceneNode *_hookNode {nullptr};
    int _attachmentProgram {-1};
    std::weak_ptr<Area> _releaseArea;
    std::weak_ptr<Camera> _viewAngleCamera;
    uint32_t _viewAngleHold {0};
    bool _viewAngleNarrowed {false};
    Game *_motionBlurGame {nullptr};
    bool _motionBlurProgram {false};
    bool _motionBlurApplied {false};
    std::weak_ptr<scene::ModelSceneNode> _shellOwner;
    std::shared_ptr<graphics::Texture> _shellTexture;
    std::shared_ptr<audio::AudioSource> _durationSound;
    std::vector<scene::MeshSceneNode *> _beamMeshes;
    std::shared_ptr<scene::ModelSceneNode> _beamSourceOwner;
    std::shared_ptr<scene::ModelSceneNode> _beamTargetOwner;
    bool _beam {false};
    // Whether the application is kept on the object, rather than shown once.
    bool _kept {false};
    // Time left in the impact program, and the duration program that follows.
    float _impactRemaining {0.0f};
    int _pendingDurationProgram {-1};
    EffectApplicationResult present(Object &object, EffectInstance &instance);
    void attachNodeModels(Object &object, const std::shared_ptr<scene::ModelSceneNode> &body, bool restoring);
    void startNodeModel(scene::ISceneGraph &graph, graphics::Model &model, scene::SceneNode *hook, VisualSite site,
                        std::shared_ptr<scene::ModelSceneNode> owner, const glm::vec3 &position, bool restoring);
    float nodeModelsImpactLength() const;
    void updateNodeModels(float dt);
    bool resolveBeamEnds(Object &object, const EffectInstance &instance,
                         std::shared_ptr<scene::ModelSceneNode> &sourceModel, scene::SceneNode *&sourceNode,
                         std::shared_ptr<scene::ModelSceneNode> &targetModel, scene::SceneNode *&targetNode);
    bool startProgram(int program, Object &object, const EffectInstance &instance);
    // Ending a shell shows the latest shell left on the holder, if given.
    void stopProgram(Object *holder = nullptr);
    bool startViewAngleProgram(Object &object);
    void updateViewAngleProgram(Object &object);
    void stopViewAngleProgram();
    void startMotionBlurProgram(Object &object);
    void updateMotionBlurProgram(Object &object);
    float impactProgramLength(int program) const;
    void clearPresentation(Object *holder = nullptr);
};

} // namespace game

} // namespace reone
