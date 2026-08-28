#include "StreamingRateAdapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hybrid {

std::uint64_t hostFrameToNative(std::uint64_t hostFrame,
                                std::uint32_t nativeRate,
                                std::uint32_t hostRate) noexcept
{
    if (hostRate == 0)
        return 0;
    const auto seconds = hostFrame / hostRate;
    const auto remainder = hostFrame % hostRate;
    return seconds * nativeRate + remainder * nativeRate / hostRate;
}

void StreamingRateAdapter::configure(std::uint32_t nativeRate,
                                     std::uint32_t hostRate,
                                     std::size_t busCount,
                                     std::size_t maximumOutputFrames)
{
    if (nativeRate == 0 || hostRate == 0 || busCount == 0)
        throw std::invalid_argument("invalid sample-rate adapter configuration");
    buses = busCount;
    maximumFrames = maximumOutputFrames;
    sourceFramesPerOutputFrame = static_cast<double>(nativeRate) / hostRate;
    conversionActive = nativeRate != hostRate;
    const auto maximumInput = static_cast<std::size_t>(std::ceil(
        maximumOutputFrames * sourceFramesPerOutputFrame));
    capacityFrames = std::max<std::size_t>(maximumInput + 4, 4);
    if (capacityFrames > std::numeric_limits<std::size_t>::max() / buses)
        throw std::length_error("sample-rate adapter capacity overflow");
    samples.assign(capacityFrames * buses, 0.0f);
    reset();
}

void StreamingRateAdapter::reset() noexcept
{
    bufferedFrames = 0;
    sourcePosition = 0.0;
}

bool StreamingRateAdapter::active() const noexcept
{
    return conversionActive;
}

std::size_t StreamingRateAdapter::inputFramesNeeded(
    std::size_t outputFrames) const
{
    if (!conversionActive || outputFrames == 0)
        return 0;
    if (outputFrames > maximumFrames)
        throw std::length_error("sample-rate adapter output exceeds capacity");
    const auto lastPosition = sourcePosition
        + static_cast<double>(outputFrames - 1) * sourceFramesPerOutputFrame;
    const auto required = static_cast<std::size_t>(std::floor(lastPosition)) + 2;
    return required > bufferedFrames ? required - bufferedFrames : 0;
}

void StreamingRateAdapter::append(std::span<const float* const> input,
                                  std::size_t frames)
{
    if (input.size() != buses)
        throw std::invalid_argument("sample-rate adapter input bus mismatch");
    if (frames > capacityFrames - bufferedFrames)
        throw std::length_error("sample-rate adapter input exceeds capacity");
    for (std::size_t index = 0; index < buses; ++index) {
        if (input[index] == nullptr)
            throw std::invalid_argument("sample-rate adapter input is null");
        std::copy_n(input[index], frames, bus(index) + bufferedFrames);
    }
    bufferedFrames += frames;
}

void StreamingRateAdapter::process(std::span<float* const> output,
                                   std::size_t frames)
{
    if (!conversionActive)
        throw std::logic_error("sample-rate adapter direct path was processed");
    if (output.size() != buses)
        throw std::invalid_argument("sample-rate adapter output bus mismatch");
    if (inputFramesNeeded(frames) != 0)
        throw std::runtime_error("sample-rate adapter input underflow");
    if (std::any_of(output.begin(), output.end(),
                    [](const float* bus) { return bus == nullptr; })) {
        throw std::invalid_argument("sample-rate adapter output is null");
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto position = sourcePosition
            + static_cast<double>(frame) * sourceFramesPerOutputFrame;
        const auto sourceFrame = static_cast<std::size_t>(std::floor(position));
        const auto fraction = static_cast<float>(position - sourceFrame);
        for (std::size_t index = 0; index < buses; ++index) {
            const auto* source = bus(index);
            output[index][frame] = source[sourceFrame]
                + (source[sourceFrame + 1] - source[sourceFrame]) * fraction;
        }
    }

    sourcePosition += static_cast<double>(frames) * sourceFramesPerOutputFrame;
    const auto consumed = static_cast<std::size_t>(std::floor(sourcePosition));
    if (consumed != 0) {
        for (std::size_t index = 0; index < buses; ++index) {
            auto* source = bus(index);
            std::move(source + consumed, source + bufferedFrames, source);
        }
        bufferedFrames -= consumed;
        sourcePosition -= static_cast<double>(consumed);
    }
}

float* StreamingRateAdapter::bus(std::size_t index) noexcept
{
    return samples.data() + index * capacityFrames;
}

const float* StreamingRateAdapter::bus(std::size_t index) const noexcept
{
    return samples.data() + index * capacityFrames;
}

} // namespace hybrid
