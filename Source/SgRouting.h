#pragma once

#include <cstdint>
#include <span>

namespace hybrid {

[[nodiscard]] bool isSgConfiguration(
    std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool sgOwnsNote(std::uint32_t packedMessage,
                              std::uint32_t routeMask) noexcept;

} // namespace hybrid
