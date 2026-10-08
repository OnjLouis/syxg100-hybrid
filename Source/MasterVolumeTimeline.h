#pragma once

#include "MidiSystemReset.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hybrid {

// Original XG voices and the SG renderer already receive master volume. Use
// this envelope only for VL and 2006LE; never attenuate the full mix again.
class MasterVolumeTimeline {
public:
    static constexpr std::size_t capacity = 4096;

    static std::optional<float> messageGain(
        std::span<const std::uint8_t> bytes) noexcept
    {
        if (classifySystemReset(bytes) != MidiSystemReset::none)
            return 1.0f;
        if (bytes.size() < 8 || bytes.front() != 0xf0
            || bytes.back() != 0xf7
            || !std::all_of(bytes.begin() + 1, bytes.end() - 1,
                            [](auto value) { return value < 0x80; }))
            return std::nullopt;

        float normalized;
        constexpr std::uint8_t masterVolumeAddress = 0x04;
        if (bytes.size() >= 9 && bytes[1] == 0x43
            && (bytes[2] & 0xf0) == 0x10 && bytes[3] == 0x4c
            && bytes[4] == 0 && bytes[5] == 0
            && bytes[6] <= masterVolumeAddress) {
            const auto index = std::size_t { 7 }
                + masterVolumeAddress - bytes[6];
            if (index + 1 >= bytes.size())
                return std::nullopt;
            normalized = bytes[index] / 127.0f;
        } else if (bytes.size() == 8 && bytes[1] == 0x7f
                   && bytes[3] == 0x04 && bytes[4] == 0x01) {
            const auto value = bytes[5] | (bytes[6] << 7);
            normalized = value / 16383.0f;
        } else {
            return std::nullopt;
        }
        // Power-law MIDI attenuation, not an additional output-level boost.
        // Yamaha's quantized voice envelopes are not bit-identical to this
        // external-bus envelope, but maximum and silence are exact.
        return normalized * normalized;
    }

    bool observe(std::span<const std::uint8_t> bytes,
                 std::uint64_t frame) noexcept
    {
        const auto gain = messageGain(bytes);
        if (!gain)
            return true;
        if (consumed != 0) {
            std::move(changes.begin() + consumed, changes.begin() + count,
                      changes.begin());
            count -= consumed;
            consumed = 0;
        }
        if (count == capacity)
            return false;
        auto position = count;
        while (position != 0 && changes[position - 1].frame > frame) {
            changes[position] = changes[position - 1];
            --position;
        }
        changes[position] = { frame, *gain };
        ++count;
        return true;
    }

    float gainAt(std::uint64_t frame) noexcept
    {
        while (consumed < count && changes[consumed].frame <= frame)
            currentGain = changes[consumed++].gain;
        return currentGain;
    }

private:
    struct Change {
        std::uint64_t frame {};
        float gain { 1.0f };
    };
    std::array<Change, capacity> changes {};
    std::size_t count {};
    std::size_t consumed {};
    float currentGain { 1.0f };
};

} // namespace hybrid
