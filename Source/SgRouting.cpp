#include "SgRouting.h"

namespace hybrid {

bool isSgConfiguration(std::span<const std::uint8_t> bytes) noexcept
{
    return bytes.size() >= 5 && bytes[0] == 0xf0 && bytes[1] == 0x43
        && bytes[3] == 0x5d;
}

bool sgOwnsNote(std::uint32_t packedMessage,
                std::uint32_t routeMask) noexcept
{
    const auto operation = static_cast<std::uint8_t>(packedMessage & 0xf0);
    if (operation != 0x80 && operation != 0x90)
        return false;
    const auto channel = static_cast<std::uint8_t>(packedMessage & 0x0f);
    return (routeMask & (std::uint32_t {1} << channel)) != 0;
}

} // namespace hybrid
