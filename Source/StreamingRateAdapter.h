#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hybrid {

[[nodiscard]] std::uint64_t hostFrameToNative(
    std::uint64_t hostFrame, std::uint32_t nativeRate,
    std::uint32_t hostRate) noexcept;

class StreamingRateAdapter {
public:
    void configure(std::uint32_t nativeRate, std::uint32_t hostRate,
                   std::size_t busCount, std::size_t maximumOutputFrames);
    void reset() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::size_t inputFramesNeeded(
        std::size_t outputFrames) const;
    void append(std::span<const float* const> input, std::size_t frames);
    void process(std::span<float* const> output, std::size_t frames);

private:
    [[nodiscard]] float* bus(std::size_t index) noexcept;
    [[nodiscard]] const float* bus(std::size_t index) const noexcept;

    std::vector<float> samples;
    std::size_t buses {};
    std::size_t capacityFrames {};
    std::size_t bufferedFrames {};
    std::size_t maximumFrames {};
    double sourceFramesPerOutputFrame {1.0};
    double sourcePosition {};
    bool conversionActive {};
};

} // namespace hybrid
