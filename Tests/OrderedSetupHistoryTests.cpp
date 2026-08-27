#include "OrderedSetupHistory.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

struct SetupEvent {
    std::uint8_t channel {};
    std::uint32_t message {};

    bool operator==(const SetupEvent&) const = default;
};

constexpr std::uint32_t controlChange(std::uint8_t controller,
                                      std::uint8_t value)
{
    return 0xb0u | (static_cast<std::uint32_t>(controller) << 8)
        | (static_cast<std::uint32_t>(value) << 16);
}

} // namespace

int main()
{
    const std::array<SetupEvent, 5> pitchBendRange {
        SetupEvent {0, controlChange(101, 0)},
        SetupEvent {0, controlChange(100, 0)},
        SetupEvent {0, controlChange(6, 12)},
        SetupEvent {0, controlChange(101, 127)},
        SetupEvent {0, controlChange(100, 127)},
    };
    std::array<SetupEvent, 5> retained {};
    std::size_t retainedCount = 0;
    for (const auto& event : pitchBendRange) {
        if (!hybrid::retainOrderedSetupEvent(retained, retainedCount, event)) {
            std::fprintf(stderr, "setup history filled unexpectedly\n");
            return 1;
        }
    }
    if (retainedCount != pitchBendRange.size()
        || retained != pitchBendRange) {
        std::fprintf(stderr, "RPN setup order was not preserved\n");
        return 1;
    }
    const SetupEvent overflow {0, controlChange(6, 2)};
    if (hybrid::retainOrderedSetupEvent(retained, retainedCount, overflow)) {
        std::fprintf(stderr, "bounded setup history accepted overflow\n");
        return 1;
    }
    return 0;
}
