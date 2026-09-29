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

#include "reone/game/gui/actionslot.h"
#include "reone/game/contextaction.h"
#include "reone/game/d20/feat.h"
#include "reone/game/d20/feats.h"
#include "reone/game/d20/skill.h"
#include "reone/game/d20/skills.h"
#include "reone/game/d20/spell.h"
#include "reone/game/di/services.h"
#include "reone/game/game.h"
#include "reone/game/object/item.h"
#include "reone/game/types.h"
#include "reone/graphics/context.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/strings.h"
#include "reone/game/gui/sounds.h"
#include "reone/audio/mixer.h"

namespace reone {
namespace game {

static std::string g_attackIcon("i_attack");

void ActionMenuFeedback::reject(PowerUnavailableReason reason, ServicesView &services) {
    // DoPersonalAction and DoTargetAction use these localized GUI string refs.
    switch (reason) {
    case PowerUnavailableReason::InsufficientForce: _message = 38613; break;
    case PowerUnavailableReason::ForbiddenEquipment: _message = 38614; break;
    case PowerUnavailableReason::RequiredEquipment: _message = 38615; break;
    case PowerUnavailableReason::MinimumRange: _message = 42422; break;
    case PowerUnavailableReason::InsufficientVitality:
    case PowerUnavailableReason::None:
        // Keep the previous message for these reasons. If none exists, display no text.
        break;
    }
    if (reason != PowerUnavailableReason::None) _remaining = 5.0f;
    auto clip = services.game.guiSounds.getActionUnavailable();
    if (clip) _sound = services.audio.mixer.play(std::move(clip), audio::AudioType::Sound);
}

void ActionMenuFeedback::accept(ServicesView &services) {
    auto clip = services.game.guiSounds.getActionAccepted();
    if (clip) _sound = services.audio.mixer.play(std::move(clip), audio::AudioType::Sound);
}

// A form, implant-mode or behaviour selection announces itself for four seconds.
static constexpr float kAnnouncementDuration = 4.0f;

void ActionMenuFeedback::announce(uint32_t message) {
    _announcement = message;
    _announcementRemaining = kAnnouncementDuration;
}

void ActionMenuFeedback::update(float dt) {
    if (_remaining != -1.0f) {
        _remaining -= dt;
        if (_remaining <= 0.0f) _remaining = -1.0f;
    }
    if (_announcementRemaining > 0.0f) _announcementRemaining -= dt;
}

std::string ActionMenuFeedback::text(const Game &game) const {
    return _message ? game.getInterfaceText(*_message) : std::string();
}

std::string ActionMenuFeedback::announcementText(const Game &game) const {
    return game.getInterfaceText(_announcement);
}

float ActionMenuFeedback::descriptionOpacity() const {
    return active() ? std::min(1.0f, 2.0f * _remaining / 5.0f) : 1.0f;
}

void renderContextActionIcon(const ContextAction &action, glm::mat4 transform, ServicesView &services) {
    std::shared_ptr<graphics::Texture> texture;

    if (action.implant) texture = services.resource.textures.get(action.implant->icon, graphics::TextureUsage::GUI);
    if (action.behavior) texture = services.resource.textures.get(action.behavior->icon, graphics::TextureUsage::GUI);
    switch (action.type) {
    case ActionType::AttackObject:
        texture = services.resource.textures.get(g_attackIcon, graphics::TextureUsage::GUI);
        break;
    case ActionType::UseFeat: {
        std::shared_ptr<Feat> feat(services.game.feats.get(action.feat));
        if (feat) {
            texture = feat->icon;
        }
        break;
    }
    case ActionType::UseSkill: {
        if (action.item) {
            texture = action.item->icon();
        } else if (action.skill == SkillType::Demolitions) {
            const bool recover = action.subSkill == static_cast<int>(SubSkill::RecoverTrap);
            texture = services.resource.textures.get(recover ? "i_recovermine" : "i_disablemine", graphics::TextureUsage::GUI);
        } else if (std::shared_ptr<Skill> skill = services.game.skills.get(action.skill)) {
            texture = skill->icon;
        }
        break;
    }
    case ActionType::CastSpellAtObject: {
        // A power a feat grants shows its feat's icon.
        if (action.feat != FeatType::Invalid) {
            if (std::shared_ptr<Feat> feat = services.game.feats.get(action.feat)) texture = feat->icon;
        } else if (const auto &spellIcon = action.spell->icon) {
            texture = spellIcon;
        } else if (action.item) {
            texture = action.item->icon();
        }
        break;
    }
    default:
        break;
    }
    if (!texture)
        return;

    services.graphics.context.bindTexture(*texture);

    services.graphics.uniforms.setLocals([transform, available = action.availability.available()](auto &locals) {
        locals.reset();
        locals.model = transform;
        // Available icons use full opacity; unavailable icons use 0.25 opacity.
        locals.color.a = available ? 1.0f : 0.25f;
    });
    services.graphics.context.useProgram(services.graphics.shaderRegistry.get(graphics::ShaderProgramId::mvpIcon));
    services.graphics.meshRegistry.get(graphics::MeshName::quad).draw(services.graphics.statistic);
}

} // namespace game
} // namespace reone
