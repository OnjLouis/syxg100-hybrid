#include "MidiSystemReset.h"

namespace hybrid {

namespace {

bool isRolandGsInitialization(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() != 11 || bytes[0] != 0xf0 || bytes[1] != 0x41
        || bytes[2] > 0x1f || bytes[3] != 0x42 || bytes[4] != 0x12
        || bytes[10] != 0xf7) {
        return false;
    }

    const auto checksum = static_cast<std::uint8_t>((0x80
        - ((bytes[5] + bytes[6] + bytes[7] + bytes[8]) & 0x7f)) & 0x7f);
    if (bytes[9] != checksum)
        return false;

    const bool gsReset = bytes[5] == 0x40 && bytes[6] == 0x00
        && bytes[7] == 0x7f && bytes[8] == 0x00;
    const bool sc88SystemMode = bytes[5] == 0x00 && bytes[6] == 0x00
        && bytes[7] == 0x7f && bytes[8] <= 0x01;
    return gsReset || sc88SystemMode;
}

} // namespace

MidiSystemReset classifySystemReset(
    std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() == 6 && bytes[0] == 0xf0 && bytes[1] == 0x7e
        && bytes[3] == 0x09 && bytes[5] == 0xf7) {
        if (bytes[4] == 0x01)
            return MidiSystemReset::gm1;
        if (bytes[4] == 0x03)
            return MidiSystemReset::gm2;
    }

    if (isRolandGsInitialization(bytes))
        return MidiSystemReset::gs;

    const bool xg = bytes.size() == 9 && bytes[0] == 0xf0
        && bytes[1] == 0x43 && (bytes[2] & 0xf0) == 0x10
        && bytes[3] == 0x4c && bytes[4] == 0x00 && bytes[5] == 0x00
        && bytes[6] == 0x7e && bytes[7] == 0x00 && bytes[8] == 0xf7;
    return xg ? MidiSystemReset::xg : MidiSystemReset::none;
}

} // namespace hybrid
