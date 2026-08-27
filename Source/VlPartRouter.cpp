#include "VlPartRouter.h"

namespace hybrid {

std::uint32_t remapVlShortMessage(std::uint32_t packedMessage)
{
    return remapVlShortMessage(packedMessage, canonicalVlChannel);
}

std::uint32_t remapVlShortMessage(std::uint32_t packedMessage,
                                  std::uint8_t nativeChannel)
{
    const auto status = static_cast<std::uint8_t>(packedMessage & 0xff);
    if (status < 0x80 || status >= 0xf0)
        return packedMessage;
    return (packedMessage & ~std::uint32_t { 0x0f })
        | (nativeChannel & 0x0f);
}

VlSysexRoute routeVlSysex(std::span<const std::uint8_t> bytes,
                          std::uint8_t sourceChannel)
{
    const bool xgParameter = bytes.size() >= 9 && bytes[0] == 0xf0
        && bytes[1] == 0x43 && bytes[3] == 0x4c;
    if (!xgParameter)
        return VlSysexRoute::passThrough;

    if (bytes[4] == 0x08 || bytes[4] == 0x09) {
        return bytes[5] == sourceChannel ? VlSysexRoute::remapPart
                                         : VlSysexRoute::drop;
    }
    if (bytes[4] == 0x70 && bytes[5] == 0x00) {
        return bytes[7] == sourceChannel
            ? VlSysexRoute::remapVoiceAssignment
            : VlSysexRoute::drop;
    }
    return VlSysexRoute::passThrough;
}

std::optional<VlVoiceAssignment> vlVoiceAssignment(
    std::span<const std::uint8_t> bytes)
{
    const bool assignment = bytes.size() >= 9 && bytes[0] == 0xf0
        && bytes[1] == 0x43 && bytes[3] == 0x4c && bytes[4] == 0x70
        && bytes[5] == 0x00 && bytes[6] < 8 && bytes[7] < 16;
    if (!assignment)
        return std::nullopt;
    return VlVoiceAssignment { bytes[6], bytes[7] };
}

void applyVlSysexRoute(std::span<std::uint8_t> bytes, VlSysexRoute route)
{
    applyVlSysexRoute(bytes, route, canonicalVlChannel);
}

void applyVlSysexRoute(std::span<std::uint8_t> bytes, VlSysexRoute route,
                       std::uint8_t nativeChannel)
{
    if (route == VlSysexRoute::remapPart) {
        bytes[5] = nativeChannel;
    } else if (route == VlSysexRoute::remapVoiceAssignment) {
        bytes[6] = 0;
        bytes[7] = nativeChannel;
    }
}

} // namespace hybrid
