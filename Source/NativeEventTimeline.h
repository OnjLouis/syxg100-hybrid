#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hybrid {

constexpr std::int32_t nativeMidiLookaheadFrames = 128;

struct TimedNativeMidi {
    std::uint64_t frame {};
    std::uint32_t message {};
};

[[nodiscard]] constexpr std::uint64_t scheduleNativeMidiFrame(
    std::uint64_t hostFrame, std::int32_t deltaFrames) noexcept
{
    return hostFrame + static_cast<std::uint64_t>(std::max(0, deltaFrames))
        + nativeMidiLookaheadFrames;
}

template <std::size_t Capacity>
bool queueNativeMidi(std::array<TimedNativeMidi, Capacity>& events,
                     std::size_t& count, TimedNativeMidi event) noexcept
{
    if (count == events.size())
        return false;
    auto insertion = count;
    while (insertion != 0 && events[insertion - 1].frame > event.frame) {
        events[insertion] = events[insertion - 1];
        --insertion;
    }
    events[insertion] = event;
    ++count;
    return true;
}

template <std::size_t Capacity, typename Render, typename Send>
void renderNativeMidiTimeline(std::array<TimedNativeMidi, Capacity>& events,
                              std::size_t& count,
                              std::uint64_t& timelineFrame,
                              std::int32_t frames, Render&& render,
                              Send&& send)
{
    const auto safeFrames = static_cast<std::uint64_t>(std::max(0, frames));
    const auto endFrame = timelineFrame + safeFrames;
    std::uint64_t position = timelineFrame;
    std::size_t consumed = 0;
    while (consumed < count && events[consumed].frame <= endFrame) {
        const auto eventFrame = std::clamp(events[consumed].frame, position,
                                           endFrame);
        render(static_cast<std::int32_t>(position - timelineFrame),
               static_cast<std::int32_t>(eventFrame - position));
        send(events[consumed].message);
        position = eventFrame;
        ++consumed;
    }
    render(static_cast<std::int32_t>(position - timelineFrame),
           static_cast<std::int32_t>(endFrame - position));
    if (consumed != 0) {
        std::move(events.begin() + consumed, events.begin() + count,
                  events.begin());
        count -= consumed;
    }
    timelineFrame = endFrame;
}

} // namespace hybrid
