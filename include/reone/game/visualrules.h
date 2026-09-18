/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <string>
#include "types.h"
namespace reone::game {
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
inline const char *beamSourceHook(BodyNode part) {
    switch (part) {
    case BodyNode::Hand: return "handconjure";
    case BodyNode::Chest: return "impact";
    case BodyNode::Head: return "headconjure";
    case BodyNode::HandLeft: return "lhand";
    case BodyNode::HandRight: return "rhand";
    default: return "";
    }
}
} // namespace reone::game
