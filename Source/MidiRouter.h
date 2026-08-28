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
    [[nodiscard]] bool hasHeldVlNotes(std::uint8_t channel) const noexcept;
    [[nodiscard]] bool hasHeldXgNotes(std::uint8_t channel) const noexcept;

    std::array<std::uint8_t, 16> bankMsb {};
    std::array<std::uint8_t, 16> bankLsb {};
    std::array<std::array<std::uint16_t, 128>, 16> heldVlNotes {};
    std::array<std::array<std::uint16_t, 128>, 16> heldXgNotes {};
    std::array<bool, 16> heldVlSustain {};
    std::array<bool, 16> heldXgSustain {};
};

} // namespace hybrid
