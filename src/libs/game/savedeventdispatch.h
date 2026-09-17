/* Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace reone::game::detail {
// Existing saved-index order breaks ties. Newly appended zero-delay events
// participate only after the callback that enqueued them has returned.
template<class Events, class Live, class Clock, class Deliver>
void dispatchDueSavedEvents(Events &events, bool &dispatching, Live live,
                           Clock now, Deliver deliver) {
    if (dispatching) return;
    dispatching = true;
    struct Dispatch { bool &active; ~Dispatch() { active = false; } } dispatch {dispatching};
    for (;;) {
        std::size_t next = events.size();
        const auto time = now();
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].delivered || !live(events[i].savedIndex) ||
                events[i].dueMilliseconds > time) continue;
            if (next == events.size() || events[i].dueMilliseconds < events[next].dueMilliseconds)
                next = i;
        }
        if (next == events.size()) return;
        // No vector reference or iterator crosses a callback. Its payload may
        // enqueue another event, cancel a sibling, or request another pump.
        auto event = events[next];
        events[next].delivered = true;
        deliver(event);
    }
}
} // namespace reone::game::detail
