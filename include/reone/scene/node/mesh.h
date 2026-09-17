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

#include "modelnode.h"

namespace reone {

namespace graphics {
struct Material;
}

namespace scene {

class ModelSceneNode;

class MeshSceneNode : public ModelNodeSceneNode {
public:
    MeshSceneNode(
        ModelSceneNode &model,
        graphics::ModelNode &modelNode,
        ISceneGraph &sceneGraph,
        graphics::GraphicsServices &graphicsSvc,
        audio::AudioServices &audioSvc,
        resource::ResourceServices &resourceSvc) :
        ModelNodeSceneNode(
            modelNode,
            SceneNodeType::Mesh,
            sceneGraph,
            graphicsSvc,
            audioSvc,
            resourceSvc),
        _model(model) {
    }

    void init();

    void update(float dt) override;

    void render(IRenderPass &pass);
    void renderBumpedOutShell(
        IRenderPass &pass,
        graphics::Texture &texture,
        float offset);
    void renderShadow(IRenderPass &pass);

    bool shouldRender() const;
    bool shouldCastShadows() const;

    bool isTransparent() const;

    ModelSceneNode &model() { return _model; }
    const ModelSceneNode &model() const { return _model; }

    void setMainTexture(graphics::Texture *texture) override;
    void setEnvironmentMap(graphics::Texture *texture) override;
    void setAlpha(float alpha) { _alpha = alpha; }
    void setSelfIllumColor(glm::vec3 color) { _selfIllumColor = std::move(color); }
    void setProjectedBeamTarget(SceneNode *target) {
        _projectedBeam = true;
        _projectedBeamTarget = target;
    }

private:
    struct NodeTextures {
        graphics::Texture *diffuse {nullptr};
        graphics::Texture *lightmap {nullptr};
        graphics::Texture *envmap {nullptr};
        graphics::Texture *bumpmap {nullptr};
    } _nodeTextures;

    struct DanglyVertex {
        glm::vec3 position {0.0f};
        glm::vec3 displacement {0.0f};
        glm::vec3 velocity {0.0f};
    };

    struct DanglyMesh {
        std::vector<DanglyVertex> vertices;
        glm::vec3 prevWorldPos {0.0f};
    } _dangly;

    struct SaberVertex {
        glm::vec3 position {0.0f};
        glm::vec3 displacement {0.0f};
    };

    struct SaberMesh {
        glm::vec3 displacement {0.0f};
        glm::vec3 prevWorldPos {0.0f};
    } _saber;

    ModelSceneNode &_model;

    glm::vec2 _uvOffset {0.0f};
    float _bumpmapCycleTime {0.0f};
    int _bumpmapCycleFrame {0};
    float _alpha {1.0f};
    glm::vec3 _selfIllumColor {0.0f};

    float _windTime {0.0f};

    bool _projectedBeam {false};
    SceneNode *_projectedBeamTarget {nullptr};

    void initTextures();
    void initDanglyMesh();

    void refreshAdditionalTextures();

    bool isLightingEnabled() const;
    std::vector<glm::mat4> buildSkinBoneTransforms() const;
    void drawWithMaterial(
        IRenderPass &pass,
        graphics::Material &material);
    void renderProjectedBeam(IRenderPass &pass);

    // Animation

    void updateUVAnimation(float dt, const graphics::ModelNode::TriangleMesh &mesh);
    void updateBumpmapAnimation(float dt, const graphics::ModelNode::TriangleMesh &mesh);
    void updateDanglyAnimation(float dt, const graphics::ModelNode::Danglymesh &mesh);
    void updateSaberAnimation(float dt);

    // END Animation
};

} // namespace scene

} // namespace reone
