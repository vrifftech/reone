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

/** The difficulty level in the configuration's Game Options section, or level when the key is absent. */
uint8_t loadDifficultyLevel(const std::filesystem::path &path, uint8_t level);
/** Write the difficulty level into the configuration's Game Options section, keeping every other line. */
void saveDifficultyLevel(const std::filesystem::path &path, uint8_t level);

/** Each bit of the unlocked planet songs opens one group of the main menu's music list; there are eleven. */
constexpr int kAllPlanetSongs = 0x7FF;
/** Write the unlocked planet songs into the configuration's Game Options section, keeping every other line. */
void saveUnlockedPlanetSongs(const std::filesystem::path &path, int songs);

/** The mouse options, stored in the configuration's Game Options section. */
struct MouseOptions {
    /** The mouse turns the camera unless the right button or a Ctrl key is held. */
    bool mouseLook {false};
    /** How far a frame of mouse travel turns the camera, 0 to 255. */
    uint8_t sensitivity {44};

    /** The defaults with the configuration's keys applied; absent keys keep their defaults. */
    static MouseOptions load(const std::filesystem::path &path);
    /** Write both keys into the configuration's Game Options section, keeping every other line. */
    void save(const std::filesystem::path &path) const;
};

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
