#pragma once

#include "MidiSystemReset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hybrid {

struct XgPartModeChange {
    std::size_t part {};
    bool rhythm {};
};

class XgPartModes {
public:
    static constexpr std::size_t partCount = 16;
    static constexpr std::size_t defaultRhythmPart = 9;
    static constexpr std::uint8_t rhythmBankMsb = 127;
    static constexpr std::uint8_t gm2RhythmBankMsb = 120;
    static constexpr std::uint8_t gm2MelodicBankMsb = 121;

    XgPartModes() noexcept { reset(); }

    void reset(MidiSystemReset system = MidiSystemReset::xg) noexcept;
    [[nodiscard]] std::optional<XgPartModeChange> observe(
        std::span<const std::uint8_t> sysex) noexcept;
    [[nodiscard]] std::optional<XgPartModeChange> selectBankMsb(
        std::size_t part, std::uint8_t bankMsb) noexcept;
    [[nodiscard]] bool isRhythm(std::size_t part) const noexcept;
    [[nodiscard]] std::uint8_t effectiveBankMsb(
        std::size_t part, std::uint8_t selectedBankMsb) const noexcept;
    [[nodiscard]] std::uint8_t effectiveBankLsb(
        std::size_t part, std::uint8_t selectedBankLsb) const noexcept;

private:
    std::array<bool, partCount> rhythmParts {};
    std::array<bool, partCount> explicitPartModes {};
    MidiSystemReset systemMode { MidiSystemReset::xg };
};

} // namespace hybrid
