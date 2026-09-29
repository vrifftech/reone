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
#include <string>

#include "reone/resource/types.h"
#include "reone/game/types.h"

namespace reone {

namespace graphics {
class Model;
}

namespace audio {
class AudioClip;
}

namespace resource {
class TwoDAs;
class TwoDA;
class AudioClips;
class Models;
} // namespace resource

namespace game {

inline const char *beamModelForProgram(int program, bool tsl = false) {
    static constexpr const char *models[] = {"v_lightns_dur", "v_lightnx_dur", "v_drddisab_dur",
        "v_drdkill_dur", "v_deathfld_dur", "v_drain_dur", "v_flame_dur", "v_stunray_dur",
        "v_coldray_dur", "v_ionray01_dur", "v_ionray02_dur", "v_fstorm_dur", "v_drdstun_dur", "v_fshock_dur",
        "v_flamep_dur", "v_flamepl_dur"};
    return program >= 608 && program <= (tsl ? 623 : 621) ? models[program - 608] : "";
}
inline std::string shieldTextureForProgram(int program) {
    if (program == 1426) return "fx_tex_stealth";
    if (program < 1401 || program > 1425) return {};
    const int texture = program - (program <= 1412 ? 1400 : 1399);
    return "fx_tex_" + std::string(texture < 10 ? "0" : "") + std::to_string(texture);
}
// Spell visual programs 1200-1202: model and attachment node.
struct SpellVisualProgram {
    const char *model;
    const char *hook;
};
inline const SpellVisualProgram *spellVisualForProgram(int program) {
    static constexpr SpellVisualProgram programs[] = {
        {"v_fizzle_imp", "headconjure"}, {"v_fizzle_imp", "handconjure"}, {"v_fresist_imp", "impact"}};
    return program >= 1200 && program <= 1202 ? &programs[program - 1200] : nullptr;
}
// Model attachment programs 1700-1703: model and hook. 1703 is TSL only and
// hooks the head model first.
inline const SpellVisualProgram *modelAttachmentForProgram(int program, bool tsl) {
    static constexpr SpellVisualProgram programs[] = {
        {"v_medal_dur", "medalhook"}, {"v_revmask1_dur", "revmask1hook"},
        {"v_revmask2_dur", "revmask2hook"}, {"v_MthMst", "maskhook"}};
    return program >= 1700 && program <= (tsl ? 1703 : 1702) ? &programs[program - 1700] : nullptr;
}
// The node of a creature a beam starts from, by body part; any other part
// starts it from the root.
inline const char *beamSourceHook(BodyNode part) {
    switch (part) {
    case BodyNode::Hand: return "handconjure";
    case BodyNode::Chest: return "impact";
    case BodyNode::Head: return "headconjure";
    case BodyNode::HandLeft: return "lhand";
    case BodyNode::HandRight: return "rhand";
    default: return "root";
    }
}

struct VisualEffectDesc {
    std::string label;
    // Models attached to an object: one at its root, chosen by its size, one
    // at its impact node and one at its head.
    std::shared_ptr<graphics::Model> rootHugeModel;
    std::shared_ptr<graphics::Model> rootLargeModel;
    std::shared_ptr<graphics::Model> rootMediumModel;
    std::shared_ptr<graphics::Model> impactModel;
    std::shared_ptr<graphics::Model> headModel;
    // Attached models follow their node's position but not its rotation.
    bool orientationOff {false};
    // The one model a visual shown at a point in the world uses: the first
    // of the head, impact and root impact models the row names.
    std::shared_ptr<graphics::Model> locationModel;
    std::shared_ptr<audio::AudioClip> soundImpact;
    int progFXImpact {-1};
    int progFXDuration {-1};
    // Type_FD "F": the row is only ever presented as an impact.
    bool fireAndForget {false};
    std::shared_ptr<audio::AudioClip> soundDuration;

    /**
     * The root model for an object of this size: a huge object takes the
     * huge model, then the large, then the medium one; a large object the
     * large, then the medium one; any other object the medium one.
     */
    const std::shared_ptr<graphics::Model> &rootModel(CreatureSize size) const {
        if (size == CreatureSize::Huge && rootHugeModel) return rootHugeModel;
        if ((size == CreatureSize::Huge || size == CreatureSize::Large) && rootLargeModel) return rootLargeModel;
        return rootMediumModel;
    }
};

class IVisualEffects {
public:
    virtual std::optional<const VisualEffectDesc *> get(uint32_t id) const = 0;
    /** The visual effect a hit of the damage slot shows, if any (damagehitvisual.2da). */
    virtual int hitVisual(int damageSlot, bool ranged) const = 0;
};

class VisualEffects : public IVisualEffects {
public:
    VisualEffects(resource::TwoDAs &twoDas,
                  resource::AudioClips &audioClips,
                  resource::Models &models) :
        _twoDas(twoDas),
        _audioClips(audioClips),
        _models(models) {}

    void init();
    void clear();

    std::optional<const VisualEffectDesc *> get(uint32_t id) const override;
    int hitVisual(int damageSlot, bool ranged) const override;

private:
    static constexpr int kDamageSlotCount = 14;
    resource::TwoDAs &_twoDas;
    resource::AudioClips &_audioClips;
    resource::Models &_models;
    std::map<uint32_t, VisualEffectDesc> _effects;
    // Melee, then ranged, per damage slot.
    std::array<std::array<int, 2>, kDamageSlotCount> _hitVisuals {};
};

} // namespace game

} // namespace reone
