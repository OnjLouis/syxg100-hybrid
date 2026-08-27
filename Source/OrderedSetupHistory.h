#pragma once

#include <array>
#include <cstddef>

namespace hybrid {

template <typename Event, std::size_t Capacity>
bool retainOrderedSetupEvent(std::array<Event, Capacity>& events,
                             std::size_t& eventCount,
                             const Event& event) noexcept
{
    if (eventCount == events.size())
        return false;
    events[eventCount++] = event;
    return true;
}

} // namespace hybrid
