#include "NativeEventTimeline.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

bool expect(bool condition, const char* description)
{
    if (!condition)
        std::fprintf(stderr, "FAILED: %s\n", description);
    return condition;
}

} // namespace

int main()
{
    constexpr std::array<std::int32_t, 5> sourceFrames {0, 16, 32, 96, 160};
    std::array<std::uint64_t, sourceFrames.size()> scheduled {};
    for (std::size_t index = 0; index < sourceFrames.size(); ++index)
        scheduled[index] = hybrid::scheduleNativeMidiFrame(1'000, sourceFrames[index]);

    bool passed = true;
    for (std::size_t index = 1; index < scheduled.size(); ++index) {
        passed &= expect(scheduled[index] - scheduled[index - 1]
                             == sourceFrames[index] - sourceFrames[index - 1],
                         "look-ahead preserves controller spacing");
    }
    passed &= expect(scheduled.front()
                         == 1'000 + hybrid::nativeMidiLookaheadFrames,
                     "first event is delayed by one native render quantum");

    std::array<hybrid::TimedNativeMidi, 8> events {};
    std::size_t eventCount = 0;
    passed &= expect(hybrid::queueNativeMidi(
                         events, eventCount, {scheduled[2], 32}),
                     "queues later controller event");
    passed &= expect(hybrid::queueNativeMidi(
                         events, eventCount, {scheduled[0], 0}),
                     "queues earlier controller event");
    passed &= expect(events[0].message == 0 && events[1].message == 32,
                     "queue keeps controller events in timestamp order");

    std::array<std::int32_t, 8> eventOffsets {};
    std::size_t sentCount = 0;
    std::int32_t renderedFrames = 0;
    std::uint64_t timelineFrame = 1'000;
    hybrid::renderNativeMidiTimeline(
        events, eventCount, timelineFrame, 128,
        [&](std::int32_t, std::int32_t frames) { renderedFrames += frames; },
        [&](std::uint32_t message) {
            eventOffsets[sentCount++] = renderedFrames;
            eventOffsets[sentCount++] = static_cast<std::int32_t>(message);
        });
    passed &= expect(renderedFrames == 128 && sentCount == 2
                         && eventOffsets[0] == 128 && eventOffsets[1] == 0,
                     "first look-ahead block reaches the first controller only");
    hybrid::renderNativeMidiTimeline(
        events, eventCount, timelineFrame, 64,
        [&](std::int32_t, std::int32_t frames) { renderedFrames += frames; },
        [&](std::uint32_t message) {
            eventOffsets[sentCount++] = renderedFrames;
            eventOffsets[sentCount++] = static_cast<std::int32_t>(message);
        });
    passed &= expect(eventCount == 0 && renderedFrames == 192
                         && sentCount == 4 && eventOffsets[2] == 160
                         && eventOffsets[3] == 32,
                     "next block retains the exact controller interval");
    return passed ? 0 : 1;
}
