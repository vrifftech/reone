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
    if (line.find('=') == std::string::npos) return std::nullopt;
    auto key = trim(line.substr(0, line.find('=')));
    for (const auto &[name, bit] : feedbackKeys) {
        if (key == name) return bit;
    }
    return std::nullopt;
}

} // namespace

uint16_t loadFeedbackOptions(const std::filesystem::path &path, uint16_t options) {
    std::ifstream input(path);
    bool inSection = false;
    for (std::string line; std::getline(input, line);) {
        auto trimmed = trim(line);
        if (trimmed.rfind("[", 0) == 0) {
            inSection = trimmed == gameOptionsSection;
            continue;
        }
        if (!inSection) continue;
        if (auto bit = feedbackBit(line)) {
            // Only the lowest bit of the number counts.
            const bool on = (std::atoi(line.substr(line.find('=') + 1).c_str()) & 1) != 0;
            options = static_cast<uint16_t>(on ? (options | *bit) : (options & ~*bit));
        }
    }
    return options;
}

void saveFeedbackOptions(const std::filesystem::path &path, uint16_t options) {
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    if (!input && std::filesystem::exists(path)) {
        throw std::runtime_error("Cannot read feedback configuration: " + path.string());
    }
    auto writeKeys = [options](std::ostringstream &output) {
        for (const auto &[name, bit] : feedbackKeys) output << name << '=' << ((options & bit) ? 1 : 0) << '\n';
    };
    // The keys are rewritten at the end of the Game Options section; every
    // other line, in that section and elsewhere, is kept.
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
        } else if (inSection && feedbackBit(line)) {
            continue;
        }
        output << line << '\n';
    }
    if (!written) {
        if (!inSection) output << gameOptionsSection << '\n';
        writeKeys(output);
    }
    if (input.is_open()) input.close();
    replaceConfiguration(path, output.str(), ".feedback.tmp", "feedback");
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
    std::ifstream input(path);
    bool inSection = false;
    for (std::string line; std::getline(input, line);) {
        auto trimmed = trim(line);
        if (trimmed.rfind("[", 0) == 0) {
            inSection = trimmed == autoPauseSection;
            continue;
        }
        if (!inSection || line.find('=') == std::string::npos) continue;
        auto key = trim(line.substr(0, line.find('=')));
        for (size_t i = 0; i < autoPauseKeys.size(); ++i) {
            if (key != autoPauseKeys[i]) continue;
            std::istringstream value(line.substr(line.find('=') + 1));
            int number;
            if (value >> number) *values[i] = number != 0;
        }
    }
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
