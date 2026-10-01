/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "reone/game/menupresentation.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

namespace reone::game {
namespace {
const std::array<std::string, 5> keys {
    "k2-menu-selector", "k2-menu-gender", "k2-menu-appearance",
    "k2-menu-body", "k2-menu-texture"};

std::string trim(const std::string &s) {
    auto begin = s.find_first_not_of(" \t\r");
    return begin == std::string::npos ? "" : s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

std::optional<size_t> menuKey(const std::string &line) {
    auto key = trim(line.substr(0, line.find('=')));
    for (size_t i = 0; i < keys.size(); ++i) {
        if (key == keys[i]) return i;
    }
    return std::nullopt;
}

// The trimmed key before '=' of an option line; empty when the line has no '='.
std::string gameOptionKey(const std::string &line) {
    return line.find('=') == std::string::npos ? "" : trim(line.substr(0, line.find('=')));
}

// Visit every line inside the section, in file order.
void forEachSectionLine(const std::filesystem::path &path, const std::string &section,
                        const std::function<void(const std::string &)> &visit) {
    std::ifstream input(path);
    bool inSection = false;
    for (std::string line; std::getline(input, line);) {
        auto trimmed = trim(line);
        if (trimmed.rfind("[", 0) == 0) {
            inSection = trimmed == section;
            continue;
        }
        if (inSection) visit(line);
    }
}

const std::string autoPauseSection {"[Autopause Options]"};
const std::array<std::string, 6> autoPauseKeys {
    "End Of Combat Round", "Enemy Sighted", "Mine Sighted",
    "Party Killed", "Action Menu", "New Target Selected"};

// Write the whole configuration through a sibling file so a failed write never
// truncates the live configuration.
void replaceConfiguration(const std::filesystem::path &path, const std::string &content,
                          const char *suffix, const std::string &purpose) {
    auto temporary = path;
    temporary += suffix;
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << content;
    file.close();
    if (!file) throw std::runtime_error("Cannot write " + purpose + " configuration: " + temporary.string());
    std::filesystem::rename(temporary, path);
}
} // namespace

std::string MenuPresentation::modelResRef(bool tsl) const {
    if (!tsl) return "mainmenu";
    return "mainmenu0" + std::to_string(validateSelector(selector) + 1);
}

MenuPresentation MenuPresentation::load(const std::filesystem::path &path) {
    MenuPresentation result;
    std::ifstream input(path);
    std::map<size_t, int> values;
    for (std::string line; std::getline(input, line);) {
        if (trim(line).rfind("[", 0) == 0) break; // These are top-level engine options.
        auto key = menuKey(line);
        if (!key || line.find('=') == std::string::npos) continue;
        std::istringstream value(line.substr(line.find('=') + 1));
        int number;
        if (value >> number) values[*key] = number;
    }
    result.selector = validateSelector(values[0]);
    if (values.count(1) && values.count(2) && values.count(3) && values.count(4) &&
        values[1] >= 0 && values[1] <= 4 && values[2] >= 0 && values[2] <= 65535 &&
        values[3] >= 0 && values[3] < 26 && values[4] >= 0 && values[4] <= 255) {
        result.leader = CreaturePresentation {values[1], values[2], values[3], values[4]};
    }
    return result;
}

void MenuPresentation::save(const std::filesystem::path &path) const {
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    if (!input && std::filesystem::exists(path)) {
        throw std::runtime_error("Cannot read menu configuration: " + path.string());
    }
    std::ostringstream output;
    output << keys[0] << '=' << validateSelector(selector) << '\n';
    if (leader) {
        output << keys[1] << '=' << leader->gender << '\n'
               << keys[2] << '=' << leader->appearance << '\n'
               << keys[3] << '=' << leader->bodyVariation << '\n'
               << keys[4] << '=' << leader->textureVariation << '\n';
    }
    // Retain every unrelated byte, including comments, sections, and unknown keys.
    bool topLevel = true;
    for (std::string line; std::getline(input, line);) {
        if (trim(line).rfind("[", 0) == 0) topLevel = false;
        if (topLevel && menuKey(line)) continue;
        output << line;
        if (!input.eof()) output << '\n';
    }
    // Windows cannot replace the configuration while our read handle is open.
    if (input.is_open()) input.close();
    replaceConfiguration(path, output.str(), ".menu.tmp", "menu");
}

namespace {

const std::string gameOptionsSection {"[Game Options]"};
const std::array<std::pair<const char *, uint16_t>, 9> feedbackKeys {{
    {"Hide Unequippable", feedbackoption::kHideUnequippable},
    {"Tutorial Popups", feedbackoption::kTutorialPopups},
    {"Subtitles", feedbackoption::kSubtitles},
    {"Mini Map", feedbackoption::kMiniMap},
    {"Floating Numbers", feedbackoption::kFloatingNumbers},
    {"Status Summary", feedbackoption::kStatusSummary},
    {"Use Small Fonts", feedbackoption::kSmallFonts},
    {"Hide InGame GUI", feedbackoption::kHideInGameGui},
    {"Enable Tooltips", feedbackoption::kTooltips},
}};

std::optional<uint16_t> feedbackBit(const std::string &line) {
    auto key = gameOptionKey(line);
    for (const auto &[name, bit] : feedbackKeys) {
        if (key == name) return bit;
    }
    return std::nullopt;
}

} // namespace

uint16_t loadFeedbackOptions(const std::filesystem::path &path, uint16_t options) {
    forEachSectionLine(path, gameOptionsSection, [&options](const std::string &line) {
        if (auto bit = feedbackBit(line)) {
            // Only the lowest bit of the number counts.
            const bool on = (std::atoi(line.substr(line.find('=') + 1).c_str()) & 1) != 0;
            options = static_cast<uint16_t>(on ? (options | *bit) : (options & ~*bit));
        }
    });
    return options;
}

namespace {

const std::string difficultyLevelKey {"Difficulty Level"};

bool isDifficultyLevelLine(const std::string &line) {
    return gameOptionKey(line) == difficultyLevelKey;
}

// Rewrite some keys of the Game Options section at its end; every other line,
// in that section and elsewhere, is kept.
void rewriteGameOptions(const std::filesystem::path &path,
                        const std::function<bool(const std::string &)> &ownsLine,
                        const std::function<void(std::ostringstream &)> &writeKeys,
                        const char *suffix, const std::string &purpose) {
    std::ifstream input(path, std::ios::binary);
    if (!input && std::filesystem::exists(path)) {
        throw std::runtime_error("Cannot read " + purpose + " configuration: " + path.string());
    }
    std::ostringstream output;
    bool inSection = false;
    bool written = false;
    for (std::string line; std::getline(input, line);) {
        auto trimmed = trim(line);
        if (trimmed.rfind("[", 0) == 0) {
            if (inSection && !written) {
                writeKeys(output);
                written = true;
            }
            inSection = trimmed == gameOptionsSection;
        } else if (inSection && ownsLine(line)) {
            continue;
        }
        output << line << '\n';
    }
    if (!written) {
        if (!inSection) output << gameOptionsSection << '\n';
        writeKeys(output);
    }
    if (input.is_open()) input.close();
    replaceConfiguration(path, output.str(), suffix, purpose);
}

} // namespace

void saveFeedbackOptions(const std::filesystem::path &path, uint16_t options) {
    if (path.empty()) return;
    rewriteGameOptions(
        path,
        [](const std::string &line) { return feedbackBit(line).has_value(); },
        [options](std::ostringstream &output) {
            for (const auto &[name, bit] : feedbackKeys) output << name << '=' << ((options & bit) ? 1 : 0) << '\n';
        },
        ".feedback.tmp", "feedback");
}

uint8_t loadDifficultyLevel(const std::filesystem::path &path, uint8_t level) {
    forEachSectionLine(path, gameOptionsSection, [&level](const std::string &line) {
        // The number is taken as written, without a range check; an empty
        // value reads as zero.
        if (isDifficultyLevelLine(line)) {
            level = static_cast<uint8_t>(std::atoi(line.substr(line.find('=') + 1).c_str()));
        }
    });
    return level;
}

void saveDifficultyLevel(const std::filesystem::path &path, uint8_t level) {
    if (path.empty()) return;
    rewriteGameOptions(
        path,
        isDifficultyLevelLine,
        [level](std::ostringstream &output) { output << difficultyLevelKey << '=' << static_cast<int>(level) << '\n'; },
        ".difficulty.tmp", "difficulty");
}

namespace {

const std::string unlockedPlanetSongsKey {"UnlockedPlanetSongs"};

} // namespace

void saveUnlockedPlanetSongs(const std::filesystem::path &path, int songs) {
    if (path.empty()) return;
    rewriteGameOptions(
        path,
        [](const std::string &line) { return gameOptionKey(line) == unlockedPlanetSongsKey; },
        [songs](std::ostringstream &output) { output << unlockedPlanetSongsKey << '=' << songs << '\n'; },
        ".songs.tmp", "music");
}

namespace {

const std::string mouseLookKey {"Mouse Look"};
const std::string mouseSensitivityKey {"Mouse Sensitivity"};

bool isMouseOptionLine(const std::string &line) {
    auto key = gameOptionKey(line);
    return key == mouseLookKey || key == mouseSensitivityKey;
}

} // namespace

MouseOptions MouseOptions::load(const std::filesystem::path &path) {
    MouseOptions result;
    forEachSectionLine(path, gameOptionsSection, [&result](const std::string &line) {
        // Mouse Look keeps only the lowest bit of its number. The sensitivity
        // is taken as written, as a byte, without a range check. An empty
        // value reads as zero.
        auto key = gameOptionKey(line);
        if (key != mouseLookKey && key != mouseSensitivityKey) return;
        const int value = std::atoi(line.substr(line.find('=') + 1).c_str());
        if (key == mouseLookKey) {
            result.mouseLook = (value & 1) != 0;
        } else {
            result.sensitivity = static_cast<uint8_t>(value);
        }
    });
    return result;
}

void MouseOptions::save(const std::filesystem::path &path) const {
    if (path.empty()) return;
    rewriteGameOptions(
        path,
        isMouseOptionLine,
        [this](std::ostringstream &output) {
            output << mouseLookKey << '=' << (mouseLook ? 1 : 0) << '\n'
                   << mouseSensitivityKey << '=' << static_cast<int>(sensitivity) << '\n';
        },
        ".mouse.tmp", "mouse");
}

AutoPauseOptions AutoPauseOptions::defaults(bool tsl) {
    AutoPauseOptions result;
    result.mineSighted = !tsl;
    return result;
}

AutoPauseOptions AutoPauseOptions::load(const std::filesystem::path &path, bool tsl) {
    auto result = defaults(tsl);
    std::array<bool *, 6> values {
        &result.endOfCombatRound, &result.enemySighted, &result.mineSighted,
        &result.partyKilled, &result.actionMenu, &result.newTargetSelected};
    forEachSectionLine(path, autoPauseSection, [&values](const std::string &line) {
        auto key = gameOptionKey(line);
        for (size_t i = 0; i < autoPauseKeys.size(); ++i) {
            if (key != autoPauseKeys[i]) continue;
            std::istringstream value(line.substr(line.find('=') + 1));
            int number;
            if (value >> number) *values[i] = number != 0;
        }
    });
    return result;
}

void AutoPauseOptions::save(const std::filesystem::path &path) const {
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    if (!input && std::filesystem::exists(path)) {
        throw std::runtime_error("Cannot read autopause configuration: " + path.string());
    }
    const std::array<bool, 6> values {
        endOfCombatRound, enemySighted, mineSighted, partyKilled, actionMenu, newTargetSelected};
    // Retain every other section and top-level line; this section is rewritten last.
    std::ostringstream output;
    bool inSection = false;
    for (std::string line; std::getline(input, line);) {
        auto trimmed = trim(line);
        if (trimmed.rfind("[", 0) == 0) inSection = trimmed == autoPauseSection;
        if (inSection) continue;
        output << line << '\n';
    }
    output << autoPauseSection << '\n';
    for (size_t i = 0; i < autoPauseKeys.size(); ++i)
        output << autoPauseKeys[i] << '=' << (values[i] ? 1 : 0) << '\n';
    if (input.is_open()) input.close();
    replaceConfiguration(path, output.str(), ".autopause.tmp", "autopause");
}
} // namespace reone::game
