#include "StreamingRateAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

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
    bool passed = true;
    hybrid::StreamingRateAdapter adapter;
    adapter.configure(44'100, 48'000, 2, 2'048);

    passed &= expect(adapter.active(), "48 kHz enables conversion");
    passed &= expect(adapter.inputFramesNeeded(512) == 471,
                     "first 48 kHz block requests exact native look-ahead");

    std::array<std::vector<float>, 2> source {
        std::vector<float>(471), std::vector<float>(471)
    };
    for (std::size_t frame = 0; frame < source[0].size(); ++frame) {
        source[0][frame] = static_cast<float>(frame);
        source[1][frame] = static_cast<float>(frame * 2);
    }
    std::array<const float*, 2> sourcePointers {
        source[0].data(), source[1].data()
    };
    adapter.append(sourcePointers, source[0].size());

    std::array<std::vector<float>, 2> output {
        std::vector<float>(512), std::vector<float>(512)
    };
    std::array<float*, 2> outputPointers {
        output[0].data(), output[1].data()
    };
    adapter.process(outputPointers, output[0].size());
    passed &= expect(std::abs(output[0][1] - 0.91875f) < 0.00001f,
                     "linear conversion uses the exact 44.1-to-48 ratio");
    passed &= expect(adapter.inputFramesNeeded(512) == 470,
                     "second block reuses retained source continuity");

    const auto before = output[0].back();
    source[0].resize(470);
    source[1].resize(470);
    for (std::size_t frame = 0; frame < source[0].size(); ++frame) {
        source[0][frame] = static_cast<float>(471 + frame);
        source[1][frame] = static_cast<float>((471 + frame) * 2);
    }
    sourcePointers = {source[0].data(), source[1].data()};
    adapter.append(sourcePointers, source[0].size());
    adapter.process(outputPointers, output[0].size());
    passed &= expect(output[0].front() > before,
                     "successive blocks remain monotonic at their boundary");

    passed &= expect(hybrid::hostFrameToNative(48'000, 44'100, 48'000)
                         == 44'100,
                     "one host second maps to one native second");
    passed &= expect(hybrid::hostFrameToNative(24'000, 44'100, 48'000)
                         == 22'050,
                     "event offsets map without cumulative rounding drift");

    const auto convertInBlocks = [](std::span<const std::size_t> blocks) {
        hybrid::StreamingRateAdapter converter;
        converter.configure(44'100, 48'000, 1, 2'048);
        std::vector<float> result;
        std::uint64_t sourceFrame = 0;
        for (const auto block : blocks) {
            const auto needed = converter.inputFramesNeeded(block);
            std::vector<float> input(needed);
            for (std::size_t frame = 0; frame < needed; ++frame) {
                input[frame] = std::sin(static_cast<float>(
                    (sourceFrame + frame) * 0.017));
            }
            sourceFrame += needed;
            const std::array<const float*, 1> inputPointer {input.data()};
            converter.append(inputPointer, needed);
            const auto offset = result.size();
            result.resize(offset + block);
            const std::array<float*, 1> outputPointer {result.data() + offset};
            converter.process(outputPointer, block);
        }
        return result;
    };
    constexpr std::array<std::size_t, 2> largeBlocks {512, 512};
    constexpr std::array<std::size_t, 5> unevenBlocks {127, 263, 5, 511, 118};
    const auto largeOutput = convertInBlocks(largeBlocks);
    const auto unevenOutput = convertInBlocks(unevenBlocks);
    float maximumDifference = 0.0f;
    for (std::size_t frame = 0; frame < largeOutput.size(); ++frame) {
        maximumDifference = std::max(
            maximumDifference,
            std::abs(largeOutput[frame] - unevenOutput[frame]));
    }
    passed &= expect(maximumDifference < 0.00001f,
                     "conversion is continuous across arbitrary host blocks");

    adapter.configure(44'100, 44'100, 2, 2'048);
    passed &= expect(!adapter.active(), "44.1 kHz keeps the direct path");
    return passed ? 0 : 1;
}
