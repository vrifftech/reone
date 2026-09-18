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

#include "reone/game/effect/source.h"

#include <algorithm>
#include <limits>

#include "reone/game/effect.h"
#include "reone/game/object/item.h"

namespace reone {

namespace game {

EffectSourceKey getEffectSourceKey(const EffectInstance &effect) {
    if (auto creator = effect.boundCreator()) {
        if (dyn_cast<Item>(creator.get())) {
            return {
                EffectSourceKind::Item,
                creator->runtimeIncarnation(),
            };
        }
    }
    if (effect.spellId != std::numeric_limits<uint32_t>::max()) {
        return {
            EffectSourceKind::Spell,
            effect.spellId,
        };
    }
    return {EffectSourceKind::Independent, effect.id};
}

} // namespace game

} // namespace reone

namespace reone {
namespace game {

void EffectModifierReducer::addIncrease(
    EffectSourceKey source, int subtype, int amount) {
    add(_increases, source, subtype, amount);
}

void EffectModifierReducer::addDecrease(
    EffectSourceKey source, int subtype, int amount) {
    add(_decreases, source, subtype, amount);
}

int EffectModifierReducer::totalIncrease(int cap) const {
    return cappedTotal(_increases, cap);
}

int EffectModifierReducer::totalDecrease(int cap) const {
    return cappedTotal(_decreases, cap);
}

std::map<int, int> EffectModifierReducer::increasesBySubtype(int cap) const {
    return cappedBySubtype(_increases, cap);
}

std::map<int, int> EffectModifierReducer::decreasesBySubtype(int cap) const {
    return cappedBySubtype(_decreases, cap);
}

void EffectModifierReducer::add(
    Values &values,
    EffectSourceKey source,
    int subtype,
    int amount) {
    if (amount <= 0) {
        return;
    }
    Key key {source, subtype};
    auto it = values.find(key);
    if (it == values.end() || amount > it->second) {
        values.insert_or_assign(key, amount);
    }
}

int EffectModifierReducer::cappedTotal(const Values &values, int cap) {
    int result = 0;
    for (const auto &[key, amount] : values) {
        result = std::min(cap, result + amount);
        if (result == cap) {
            break;
        }
    }
    return result;
}

std::map<int, int> EffectModifierReducer::cappedBySubtype(
    const Values &values, int cap) {
    std::map<int, int> totals;
    for (const auto &[key, amount] : values) {
        totals[key.second] += amount;
    }

    std::map<int, int> result;
    int remaining = cap;
    for (const auto &[subtype, amount] : totals) {
        if (remaining <= 0) {
            break;
        }
        int retained = std::min(remaining, amount);
        if (retained > 0) {
            result[subtype] = retained;
            remaining -= retained;
        }
    }
    return result;
}

void AbilityEffectReducer::addIncrease(EffectSourceKey source, int amount) {
    add(_increases, source, amount, true);
}

void AbilityEffectReducer::addDecrease(EffectSourceKey source, int amount) {
    add(_decreases, source, amount, false);
}

void AbilityEffectReducer::add(
    Buckets &buckets, EffectSourceKey source, int amount, bool increase) {
    if (amount <= 0) return;
    const size_t capacity = getAbilityEffectSourceCapacity(_tsl);
    if (source.kind != EffectSourceKind::Independent) {
        bool found = false;
        for (size_t i = 0; i < capacity; ++i) {
            auto &bucket = buckets[i];
            if (bucket.hasSource && bucket.source.kind == source.kind &&
                bucket.source.value == source.value) {
                bucket.amount = increase ? std::max(bucket.amount, amount)
                                         : bucket.amount + amount;
                found = true;
                // Positive lookup stops after replacing a smaller
                // value. Normally a grouped source owns exactly one slot.
                if (increase) break;
            }
        }
        if (found) return;
    }
    for (size_t i = 0; i < capacity; ++i) {
        auto &bucket = buckets[i];
        if (bucket.amount == -1) {
            bucket = {source, amount, true};
            return;
        }
    }
    // Both directions have fixed, independent slot arrays. Existing
    // sources can still be updated when there is no slot for a new source.
}

int AbilityEffectReducer::total(const Buckets &buckets, int cap) const {
    int sum = 0;
    for (size_t i = 0; i < getAbilityEffectSourceCapacity(_tsl); ++i) {
        if (buckets[i].amount != -1) sum += buckets[i].amount;
    }
    return std::min(sum, cap);
}

int AbilityEffectReducer::total() const {
    return total(_increases, getAbilityEffectIncreaseCap(_tsl)) -
           total(_decreases, getAbilityEffectDecreaseCap(_tsl));
}



} // namespace game
} // namespace reone
