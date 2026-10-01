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

#include "reone/game/animationutil.h"
#include "reone/game/combattables.h"

#include <array>

#include "reone/resource/2da.h"

using namespace reone::resource;

namespace reone {

namespace game {

static constexpr int kPauseAnimationId = 10000;
static constexpr int kDialogAnimationBase = 10000;

bool isAnimationLooping(AnimationType animation) {
    int ordinal = static_cast<int>(animation);

    return animation == AnimationType::LoopingChoke ||
           (ordinal >= static_cast<int>(AnimationType::LoopingPause) && ordinal <= static_cast<int>(AnimationType::LoopingMeditateStand));
}

int scriptAnimationId(int constant, bool tsl, bool characterModel) {
    static constexpr int kInjectConstant = 112;
    static constexpr int kInjectAnimationId = 10070;
    // Constants 0-46; 42 stands for its own row. KotOR names only 0-32.
    static const std::array<int, 47> kLooping {
        10000, 10052, 10030, 10032, 10033, 10038, 10039, 10040, 10041, 10042,
        10059, 10060, 10057, 10058, 10120, 10121, 10122, 10123, 10124, 10001,
        10118, 10023, 10137, 10139, 10151, 10152, 10006, 10154, 10155, 10160,
        10156, 10163, 10164, 10165, 10128, 10418, 10424, 10425, 10426, 10427,
        10428, 10414, 42, 10022, 10133, 10150, 10419};
    // Constants 100-124. KotOR names only 100-120, and 116 is its choke.
    static const std::array<int, 25> kFireForget {
        10053, 10054, 10055, 10056, 10034, 10035, 10029, 10028, 10044, 10044,
        10044, 10000, 10000, 10125, 10126, 10127, 10000, 10129, 10130, 10142,
        10159, 10017, 10021, 10000, 10414};
    if (tsl && constant >= 10001) return constant - 10000;
    if (constant == kInjectConstant) return characterModel ? kInjectAnimationId : kPauseAnimationId;
    if (constant >= 0 && constant <= 46) {
        return tsl || constant <= 32 ? kLooping[constant] : kPauseAnimationId;
    }
    if (constant >= 100 && constant <= 124) {
        if (tsl) return kFireForget[constant - 100];
        if (constant == 116) return 10150;
        return constant <= 120 ? kFireForget[constant - 100] : kPauseAnimationId;
    }
    return kPauseAnimationId;
}

bool playsAsOverlay(int id, bool tsl) {
    switch (id) {
    case 10029: // greeting
    case 10030: // listen
    case 10034: // salute
    case 10038: // the talk loops
    case 10039:
    case 10040:
    case 10041:
    case 10042:
    case 10070:
    case 10071:
    case 10129: // throw high
    case 10130: // throw low
    case 10136:
    case 10154: // injured talk
    case 10155: // injured listen
    case 10302: // damage flinch
        return true;
    case 10417:
        return tsl;
    default:
        return false;
    }
}

bool playsBackwards(int id) {
    switch (id) {
    case 10003:
    case 10081:
    case 10083:
    case 10212:
    case 10238: // from kneeling back to standing
    case 10256:
    case 10258:
    case 10271: // from the knees back to standing
        return true;
    default:
        return false;
    }
}

bool overlayFadesOut(int id) {
    switch (id) {
    case 10029:
    case 10030:
    case 10034:
    case 10038:
    case 10039:
    case 10040:
    case 10041:
    case 10042:
    case 10070:
    case 10071:
    case 10136:
    case 10145:
    case 10154:
    case 10155:
        return true;
    default:
        return false;
    }
}

LoopTransition loopTransition(int newId, int oldId, int currentId, bool characterModel) {
    static constexpr int kClosedLoop = 10022;
    static constexpr int kOpenLoop = 10050;
    static constexpr int kDeadLoop = 10006;
    static constexpr int kDeadProneLoop = 10156;
    static constexpr int kProneLoop = 10139;
    LoopTransition transition;
    int &clip = transition.clip;
    // Meditating and worshipping are knelt into and risen from.
    if ((newId & ~1) == 10032) clip = 10237;
    if ((oldId & ~1) == 10032) clip = 10238;
    if (newId == kClosedLoop) clip = currentId != kOpenLoop ? 337 : 336;
    if (oldId == kClosedLoop) clip = newId != kOpenLoop ? 335 : 334;
    // Lying on the back is fallen into and got up from.
    if (newId == kProneLoop) clip = 85;
    if (oldId == kProneLoop) clip = 86;
    if (newId == 10418) clip = 456;
    if (newId >= 10424 && newId <= 10428) clip = 20;
    if (newId == 10501) clip = 559;
    // The dead loops are died into and got up from.
    if (oldId == kDeadLoop) clip = 10223;
    if (oldId == 10008) clip = 10224;
    if (oldId == 10007) clip = newId != 10008 ? 10224 : 10222;
    if (oldId == 10005) clip = newId != kDeadLoop ? 10223 : 10221;
    if (newId == kDeadLoop) clip = 10221;
    if (newId == 10005) clip = 10219;
    if (newId == 10007) clip = 10220;
    if (newId == 10008) clip = 10222;
    if (oldId == 10402) clip = -1;
    if (oldId == 10405 || oldId == 10400) {
        clip = characterModel ? 85 : 272;
        transition.follow = characterModel ? 86 : 273;
    }
    if (newId == kDeadProneLoop) {
        if (oldId == kProneLoop) {
            clip = characterModel ? 374 : -1;
            transition.follow = -1;
        } else {
            clip = characterModel ? 85 : 272;
            transition.follow = characterModel ? 374 : -1;
        }
    } else if (oldId == kDeadProneLoop) {
        clip = characterModel ? 86 : 273;
        transition.follow = -1;
    }
    // Talking on the knees is knelt into and risen from.
    const bool newKneeling = newId == 10163 || newId == 10164;
    const bool oldKneeling = oldId == 10163 || oldId == 10164;
    if (newKneeling) clip = 383;
    if (oldKneeling) clip = newId == kDeadLoop ? 80 : (newKneeling ? 383 : 10271);
    return transition;
}

int loopTransitionRow(int id, bool characterModel) {
    switch (id) {
    case 10237:
    case 10238:
        return characterModel ? 23 : -1;
    case 10271:
        return characterModel ? 383 : -1;
    case 10219:
    case 10220:
        return characterModel ? 85 : 272;
    case 10221:
        return characterModel ? 80 : 274;
    case 10222:
        return characterModel ? 82 : 274;
    case 10223:
        return characterModel ? 381 : 273;
    case 10224:
        return characterModel ? 382 : 273;
    default:
        return id >= 0 && id < 1000 ? id : -1;
    }
}

bool isDialogAnimationRow(const TwoDA &table, int row) {
    return table.getBool(row, "dialog") && (table.getBool(row, "looping") || table.getBool(row, "fireforget"));
}

bool isDialogAnimation(const ICombatTables &tables, int id) {
    const auto *dialogAnimations = tables.dialogAnimationRows();
    if (dialogAnimations && id >= kDialogAnimationBase &&
        id < kDialogAnimationBase + static_cast<int>(dialogAnimations->size())) {
        return (*dialogAnimations)[id - kDialogAnimationBase];
    }
    const auto *animations = tables.animationDialogRows();
    if (animations && id >= 0 && id < std::min(kDialogAnimationBase, static_cast<int>(animations->size()))) {
        return (*animations)[id];
    }
    return (id >= 1000 && id <= 1327) || (id >= 1400 && id <= 1727);
}

} // namespace game

} // namespace reone
