#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace hybrid {

inline void patchNativeSgStartup(std::span<std::uint8_t> image)
{
    constexpr std::size_t initializerOffset = 0x1533c8;
    constexpr std::array<std::uint8_t, 4> original {0x80, 0x65, 0xfc, 0x00};
    constexpr std::array<std::uint8_t, 4> corrected {0xc6, 0x45, 0xfc, 0x01};
    if (image.size() < initializerOffset + original.size()
        || !std::equal(original.begin(), original.end(),
                       image.begin() + initializerOffset))
        throw std::runtime_error("unsupported native SG startup code");

    // The legacy one-slot setup passes zero to one-based slot helpers,
    // wrapping to index 255 and writing beyond their state allocation.
    std::copy(corrected.begin(), corrected.end(), image.begin() + initializerOffset);
}

} // namespace hybrid
