#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace hybrid {

constexpr std::uint8_t canonicalVlChannel = 2;

enum class VlSysexRoute {
    passThrough,
    remapPart,
    remapVoiceAssignment,
    drop,
};

struct VlVoiceAssignment {
    std::uint8_t voice {};
    std::uint8_t channel {};
};

[[nodiscard]] std::uint32_t remapVlShortMessage(std::uint32_t packedMessage);
[[nodiscard]] std::uint32_t remapVlShortMessage(
    std::uint32_t packedMessage, std::uint8_t nativeChannel);
[[nodiscard]] constexpr std::uint8_t nativeVlChannel(
    bool explicitAssignments, std::uint8_t sourceChannel) noexcept
{
    return explicitAssignments ? canonicalVlChannel : sourceChannel;
}
[[nodiscard]] VlSysexRoute routeVlSysex(
    std::span<const std::uint8_t> bytes, std::uint8_t sourceChannel);
[[nodiscard]] std::optional<VlVoiceAssignment> vlVoiceAssignment(
    std::span<const std::uint8_t> bytes);
void applyVlSysexRoute(std::span<std::uint8_t> bytes, VlSysexRoute route);
void applyVlSysexRoute(std::span<std::uint8_t> bytes, VlSysexRoute route,
                       std::uint8_t nativeChannel);

} // namespace hybrid
