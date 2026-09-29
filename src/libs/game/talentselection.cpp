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

#include "reone/game/object/creature.h"

#include "reone/game/attack.h"
#include "reone/game/d20/class.h"
#include "reone/game/d20/feats.h"
#include "reone/game/d20/spells.h"
#include "reone/game/di/services.h"
#include "reone/game/forcerules.h"
#include "reone/game/game.h"
#include "reone/game/object/item.h"
#include "reone/game/talent.h"
#include "reone/game/twodautil.h"
#include "reone/resource/2da.h"
#include "reone/system/randomutil.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace reone::game {
namespace {

// Read UsesPerDay on demand from the cached feat table without a second admission sweep.
int selectionNumber(const resource::TwoDA &table, int row, const char *column) {
    const auto cell = table.getString(row, column);
    int value = 0;
    std::sscanf(cell.c_str(), "%i", &value);
    return value;
}

bool categoryMatches(int requested, int actual) {
    constexpr unsigned categoryCount = 4;
    constexpr unsigned bitsPerCategory = 4;
    constexpr uint32_t wildcard = 0xfu;
    for (unsigned category = 0; category < categoryCount; ++category) {
        const unsigned shift = category * bitsPerCategory;
        const uint32_t selected = (static_cast<uint32_t>(requested) >> shift) & wildcard;
        const uint32_t value = (static_cast<uint32_t>(actual) >> shift) & wildcard;
        if (selected != wildcard && selected != value) return false;
    }
    return true;
}

bool inclusionMatches(const resource::TwoDA &table, int id, int inclusion) {
    const int exclusion = selectionNumber(table, id, "exclusion");
    // A zero exclusion or a zero bitwise-OR result passes.
    // This comparison is not an intersection test.
    return exclusion == 0 || (inclusion | exclusion) == 0;
}

struct Candidate {
    TalentType type;
    int id;
    int classIndex {kUnselectedCastingClass};
    uint32_t item {script::kObjectInvalid};
    int property {-1};
    int casterLevel {kUnspecifiedCasterLevel};
};

struct CandidatePool {
    std::vector<Candidate> entries;
    int bestCR {-100};

    void add(Candidate candidate, int cr, bool ranked) {
        if (ranked) {
            if (cr < bestCR) return;
            if (cr > bestCR) { entries.clear(); bestCR = cr; }
        }
        entries.push_back(std::move(candidate));
    }

    void addKnownPower(Candidate candidate, int cr, bool ranked) {
        // The known-power loops compare each CR with the fixed -100 value;
        // unlike the other providers, they never update a running best CR.
        if (ranked) {
            if (cr < -100) return;
            if (cr > -100) entries.clear();
        }
        entries.push_back(std::move(candidate));
    }

    std::shared_ptr<Talent> choose(Game &game) const {
        if (entries.empty()) return nullptr;
        const auto &entry = entries[randomInt(0, static_cast<int>(entries.size()) - 1)];
        return game.newTalent(entry.type, entry.id, entry.classIndex,
                              entry.item, entry.property, entry.casterLevel);
    }
};

int bodyCostShare(int cost, int level) {
    int percent = 0;
    switch (level) {
    case 0: percent = 50; break;
    case 1: percent = 40; break;
    case 2: percent = 30; break;
    default: break;
    }
    return cost * percent / 100;
}

} // namespace

int Creature::featRemainingUses(FeatType feat) const {
    feat = static_cast<FeatType>(static_cast<uint16_t>(feat));
    const auto definition = _services.game.feats.get(feat);
    if (!definition || !hasEffectiveFeat(feat)) return 0;
    const auto table = getRequiredTwoDA(_services.resource.twoDas, "feat");
    const int maximum = static_cast<uint8_t>(selectionNumber(*table, static_cast<int>(feat), "usesperday"));
    // Base feats create tracked counters only for nonzero UsesPerDay. Effect-granted feats
    // use a separate membership list and do not create counters.
    if (maximum == 0 || !_attributes.hasFeat(feat)) return 100;
    const int spent = _attributes.spentFeatUses(feat);
    // Narrow the difference before comparing it. Wider counter storage
    // does not make a negative difference equivalent to zero remaining uses.
    const int remaining = static_cast<uint8_t>(maximum - spent);
    return remaining < 100 || maximum == spent ? remaining : 100;
}

void Creature::spendFeatUse(FeatType feat) {
    feat = static_cast<FeatType>(static_cast<uint16_t>(feat));
    const auto definition = _services.game.feats.get(feat);
    if (!definition || !_attributes.hasFeat(feat)) return;
    const auto table = getRequiredTwoDA(_services.resource.twoDas, "feat");
    if (static_cast<uint8_t>(selectionNumber(*table, static_cast<int>(feat), "usesperday")) != 0)
        _attributes.spendFeatUse(feat);
}

FeatType Creature::attackFeatToUse(FeatType feat) const {
    if (!hasEffectiveFeat(feat)) return FeatType::Invalid;
    // A held rank gives way to the next rank of its chain while that is held.
    for (auto successor = _services.game.feats.get(feat)->successor;
         successor != FeatType::Invalid && hasEffectiveFeat(successor);
         successor = _services.game.feats.get(feat)->successor) {
        feat = successor;
    }
    return isPhysicalAttackFeat(feat) ? feat : FeatType::Invalid;
}

bool Creature::talentPowerAffordable(const Spell &spell) const {
    const int cost = adjustedSpellForcePointCost(spell, isPartyMember());
    const int body = _game.isTSL() ? forceBodyLevel() : -1;
    if (body == -1) return cost <= narrowSignedResource(currentForce());
    const int share = bodyCostShare(cost, body);
    // The known-power prefilter compares the Force Body share. The uses-left
    // query compares the remainder and uses a different HP bound.
    return share <= narrowSignedResource(currentForce()) && currentHitPoints() - share > 1;
}

bool Creature::hasAffordableSpell(const Spell &spell) const {
    return _attributes.hasSpell(spell.type) && talentPowerAffordable(spell);
}

bool Creature::hasSpellUsesLeft(const Spell &spell, size_t classIndex) const {
    if (classIndex >= _attributes.classLevels().size()) return false;
    const int cost = adjustedSpellForcePointCost(spell, isPartyMember());
    const int body = _game.isTSL() ? forceBodyLevel() : -1;
    const int share = body == -1 ? 0 : bodyCostShare(cost, body);
    return static_cast<uint32_t>(cost - share) <= static_cast<uint32_t>(narrowSignedResource(currentForce())) &&
           (body == -1 || currentHitPoints() - share > 0);
}

const Object *Creature::itemRepositoryOwner(bool useParty) const {
    if ((isPartyMember() && useParty) || _game.inventoryMenuCharacter() != -1)
        return _game.party().player().get();
    return this;
}

std::vector<Creature::ItemPowerSource> Creature::talentItemPowers() const {
    std::vector<ItemPowerSource> result;
    const auto append = [&](const std::shared_ptr<Item> &item, bool equipped) {
        if (!item || (!equipped && item->isEquippable())) return;
        const auto &properties = item->properties();
        for (size_t i = 0; i < properties.size(); ++i) {
            const auto &property = properties[i];
            if (property.propertyName != static_cast<uint16_t>(ItemProperty::ActivateItem)) continue;
            if (!property.usable && (equipped || item->charges() == 0)) continue;
            if (!item->isPropertyActive(property)) continue;
            result.push_back({item, i, equipped});
        }
    };
    for (int slot = 0; slot != 20; ++slot) {
        const auto found = _equipment.find(slot);
        if (found != _equipment.end()) append(found->second, true);
    }
    const Object *repository = itemRepositoryOwner();
    if (repository) for (const auto &item : repository->items()) append(item, false);
    return result;
}

std::optional<std::pair<std::shared_ptr<Item>, size_t>> Creature::itemForPower(int spellId) const {
    for (const auto &source : talentItemPowers()) {
        if (source.item->properties()[source.property].subtype == spellId)
            return std::make_pair(source.item, source.property);
    }
    return std::nullopt;
}

bool Creature::hasTalent(TalentType type, int id) const {
    if (static_cast<int>(type) == -1 || id == -1) return false;
    if ((_effectAIStateMask & 6) != 6 && isDead()) return false;
    if (isPartyMember() && currentHitPoints() <= 0) return false;
    switch (type) {
    case TalentType::Feat:
        return featRemainingUses(static_cast<FeatType>(static_cast<uint16_t>(id))) != 0;
    case TalentType::Skill: {
        const int rank = getUnopposedSkillRank(static_cast<SkillType>(static_cast<uint8_t>(id)));
        if (rank > 0) return true;
        const auto skills = getRequiredTwoDA(_services.resource.twoDas, "skills");
        return (selectionNumber(*skills, static_cast<uint16_t>(id), "untrained") & 1) != 0;
    }
    case TalentType::Spell: {
        const auto spell = _services.game.spells.get(static_cast<SpellType>(id));
        if (!spell) return false;
        bool found = false;
        const auto &classes = _attributes.classLevels();
        for (size_t index = 0; index < classes.size(); ++index) {
            const bool known = hasAffordableSpell(*spell) && hasSpellUsesLeft(*spell, index);
            if (isForceUsingClass(classes[index].first->type(), _game.isTSL())) {
                found |= known;
            } else {
                const bool special = std::any_of(_spellLikeAbilities.begin(), _spellLikeAbilities.end(),
                    [id](const auto &ability) { return ability.spell == id && ability.flags == 1; });
                found |= special || known || itemForPower(id).has_value();
            }
        }
        return found && (_effectAIStateMask & 8) != 0;
    }
    default:
        return false;
    }
}

std::shared_ptr<Talent> Creature::selectTalent(int category, int crMax, int inclusion,
                                               int excludeType, int excludeId) const {
    const auto spells = getRequiredTwoDA(_services.resource.twoDas, "spells");
    const auto feats = getRequiredTwoDA(_services.resource.twoDas, "feat");
    const bool ranked = crMax != -1;
    const int excludedSpell = excludeType == 0 ? excludeId : -1;
    const int excludedFeat = excludeType == 1 ? excludeId : -1;
    const auto suitable = [&](const Spell &spell, int excluded) {
        const int id = static_cast<int>(spell.type);
        if (id == excluded || !hasTalent(TalentType::Spell, id)) return false;
        return !spatialArea() || categoryMatches(category, selectionNumber(*spells, id, "category"));
    };

    const auto specialAbility = [&]() -> std::shared_ptr<Talent> {
        CandidatePool pool;
        for (const auto &ability : _spellLikeAbilities) {
            if (ability.flags == 0) continue;
            const auto spell = _services.game.spells.get(static_cast<SpellType>(ability.spell));
            if (!spell || !suitable(*spell, -1) || !inclusionMatches(*spells, ability.spell, inclusion)) continue;
            pool.add({TalentType::Spell, ability.spell, kSpellLikeAbilityClass, script::kObjectInvalid, -1, ability.casterLevel},
                     selectionNumber(*spells, ability.spell, "maxcr"), ranked);
        }
        return pool.choose(_game);
    };
    const auto knownPower = [&]() -> std::shared_ptr<Talent> {
        const auto &classes = _attributes.classLevels();
        for (size_t index = 0; index < classes.size(); ++index) {
            const auto clazz = classes[index].first->type();
            if (!isForceUsingClass(clazz, _game.isTSL())) continue;
            CandidatePool pool;
            for (const auto type : _attributes.spellsForClass(clazz)) {
                const auto spell = _services.game.spells.get(type);
                if (!spell || !talentPowerAffordable(*spell) || !suitable(*spell, excludedSpell)) continue;
                const int id = static_cast<int>(type);
                pool.addKnownPower({TalentType::Spell, id, static_cast<uint8_t>(index)},
                                   selectionNumber(*spells, id, "maxcr"), ranked);
            }
            if (auto selected = pool.choose(_game)) return selected;
        }
        return nullptr;
    };
    const auto itemPower = [&]() -> std::shared_ptr<Talent> {
        CandidatePool pool;
        for (const auto &source : talentItemPowers()) {
            const int id = source.item->properties()[source.property].subtype;
            const auto spell = _services.game.spells.get(static_cast<SpellType>(id));
            if (!spell) continue;
            if (source.equipped) {
                if (!suitable(*spell, -1) || !inclusionMatches(*spells, id, inclusion)) continue;
            } else if (!categoryMatches(category, selectionNumber(*spells, id, "category"))) {
                continue;
            }
            pool.add({TalentType::Spell, id, kUnselectedCastingClass, source.item->id(), static_cast<int>(source.property)},
                     selectionNumber(*spells, id, "maxcr"), ranked);
        }
        return pool.choose(_game);
    };
    const auto feat = [&]() -> std::shared_ptr<Talent> {
        CandidatePool pool;
        // This provider enumerates the main feat list. BonusFeat still affects
        // HasTalent/HasFeat, but its separate array is not an extra draw pool.
        for (const auto type : _attributes.featOrder()) {
            const int id = static_cast<uint16_t>(type);
            if (!_services.game.feats.get(type) ||
                !categoryMatches(category, selectionNumber(*feats, id, "category")) ||
                !inclusionMatches(*feats, id, inclusion) || id == excludedFeat || !hasTalent(TalentType::Feat, id)) continue;
            pool.add({TalentType::Feat, id}, selectionNumber(*feats, id, "maxcr"), ranked);
        }
        return pool.choose(_game);
    };

    bool powersExhausted = false;
    bool featsExhausted = false;
    while (!powersExhausted || !featsExhausted) {
        if (randomInt(0, 1) == 0) {
            if (powersExhausted) continue;
            if (auto selected = specialAbility()) return selected;
            if (auto selected = knownPower()) return selected;
            if (auto selected = itemPower()) return selected;
            powersExhausted = true;
        } else {
            if (featsExhausted) continue;
            // The top-level exclusion branch sets the other family's exhausted
            // flag here; it does not skip the feat-provider call.
            if (excludeType == 1 && excludeId == -1) powersExhausted = true;
            if (auto selected = feat()) return selected;
            featsExhausted = true;
        }
    }
    return _game.newTalent(static_cast<TalentType>(-1), -1, kUnselectedCastingClass);
}

} // namespace reone::game
