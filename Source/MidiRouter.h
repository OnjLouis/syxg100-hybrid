#pragma once

#include <array>
#include <cstdint>

namespace hybrid {

enum class MidiDestination {
    xg,
    vl,
    both,
};

class MidiRouter {
public:
    [[nodiscard]] MidiDestination routeShortMessage(
        std::uint32_t packedMessage);
    [[nodiscard]] bool observeShortMessage(std::uint32_t packedMessage);
    [[nodiscard]] bool isVlChannel(std::uint8_t channel) const;
    void reset();

private:
    static bool isVlBank(std::uint8_t bankMsb);

    std::array<std::uint8_t, 16> bankMsb {};
    std::array<std::uint8_t, 16> bankLsb {};
};

} // namespace hybrid
