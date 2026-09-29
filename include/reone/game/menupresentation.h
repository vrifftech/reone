/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace reone::game {

// Visual values only: never an object ID, Party binding, or live creature.
struct CreaturePresentation {
    int gender {0};
    int appearance {0};
    int bodyVariation {0}; // appearance.2da model/tex column: a = 0
    int textureVariation {1};
};

struct MenuPresentation {
    int selector {0};
    std::optional<CreaturePresentation> leader;

    static int validateSelector(int value) { return value >= 0 && value <= 4 ? value : 0; }
    std::string modelResRef(bool tsl) const;
    static MenuPresentation load(const std::filesystem::path &path);
    void save(const std::filesystem::path &path) const;
};

/**
 * Feedback option bits, as in the Game Options section of the configuration:
 * hide unequippable items, tutorial popups, subtitles, mini map, floating
 * numbers, status summary, small fonts, hide the in-game GUI and tooltips.
 */
namespace feedbackoption {
constexpr uint16_t kHideUnequippable = 0x001;
constexpr uint16_t kTutorialPopups = 0x002;
constexpr uint16_t kSubtitles = 0x004;
constexpr uint16_t kMiniMap = 0x008;
constexpr uint16_t kFloatingNumbers = 0x010;
constexpr uint16_t kStatusSummary = 0x020;
constexpr uint16_t kSmallFonts = 0x100;
constexpr uint16_t kHideInGameGui = 0x200;
constexpr uint16_t kTooltips = 0x400;
} // namespace feedbackoption

/** Tutorials, subtitles, mini map, floating numbers, status summary and tooltips on. */
constexpr uint16_t kDefaultFeedbackOptions = 0x4BE;

/** The feedback options with the configuration's Game Options keys applied; absent keys keep their bits. */
uint16_t loadFeedbackOptions(const std::filesystem::path &path, uint16_t options);
/** Write the feedback keys into the configuration's Game Options section, keeping every other line. */
void saveFeedbackOptions(const std::filesystem::path &path, uint16_t options);

/** Situations that pause play on their own, stored in the configuration's Autopause Options section. */
struct AutoPauseOptions {
    bool endOfCombatRound {false};
    bool enemySighted {true};
    bool mineSighted {false};
    bool partyKilled {true};
    bool actionMenu {false};
    bool newTargetSelected {true};

    static AutoPauseOptions defaults(bool tsl);
    static AutoPauseOptions load(const std::filesystem::path &path, bool tsl);
    void save(const std::filesystem::path &path) const;
};

} // namespace reone::game
