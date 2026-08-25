#pragma once

#include <array>
#include <cstdint>

namespace hybrid {

class MidiRouter {
public:
    [[nodiscard]] bool observeShortMessage(std::uint32_t packedMessage);
    [[nodiscard]] bool isVlChannel(std::uint8_t channel) const;

private:
    static bool isVlBank(std::uint8_t bankMsb);

    std::array<std::uint8_t, 16> bankMsb {};
    std::array<std::uint8_t, 16> bankLsb {};
};

} // namespace hybrid

