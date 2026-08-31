#pragma once

#include <cstdint>
#include <span>

namespace hybrid {

enum class MidiSystemReset : std::uint8_t {
    none,
    gm1,
    gm2,
    gs,
    xg,
};

[[nodiscard]] MidiSystemReset classifySystemReset(
    std::span<const std::uint8_t> bytes) noexcept;

} // namespace hybrid
