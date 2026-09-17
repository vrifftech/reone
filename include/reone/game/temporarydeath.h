/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <optional>

namespace reone::game {

/** TempDeathUpdate's millisecond counters, independent of world simulation.
 * Game owns readiness/game-over/modal admission. Blocked calls must not sample
 * the clock: the next admitted call includes that real-time interval.
 */
class TemporaryDeathRecovery {
public:
    void reset() {
        _lastSample.reset();
        _hostileScanElapsed = 0;
        _safeElapsed = 0;
    }

    template <class HostileScan>
    bool sample(std::uint32_t now, bool hasTemporaryDeath, HostileScan scan) {
        const std::uint32_t elapsed = _lastSample ? now - *_lastSample : 0;
        _lastSample = now;
        return update(elapsed, hasTemporaryDeath, scan);
    }

    template <class HostileScan>
    bool update(std::uint32_t elapsedMilliseconds, bool hasTemporaryDeath, HostileScan scan) {
        if (!hasTemporaryDeath) {
            // The scan accumulator survives a no-down-member gap.
            _safeElapsed = 0;
            return false;
        }
        _hostileScanElapsed += elapsedMilliseconds;
        if (_hostileScanElapsed > 1000u) {
            _hostileScanElapsed = 0;
            if (scan()) {
                _safeElapsed = 0;
                return false;
            }
        }
        _safeElapsed += elapsedMilliseconds;
        // Keep retrying until the next party scan finds no temporary deaths,
        // including cases where resurrection was refused.
        return _safeElapsed > 5000u;
    }

private:
    std::optional<std::uint32_t> _lastSample;
    std::uint32_t _hostileScanElapsed {0};
    std::uint32_t _safeElapsed {0};
};

} // namespace reone::game
