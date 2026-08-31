#include "XgPartModes.h"

namespace hybrid {

void XgPartModes::reset(MidiSystemReset system) noexcept
{
    rhythmParts.fill(false);
    rhythmParts[defaultRhythmPart] = true;
    explicitPartModes.fill(false);
    systemMode = system == MidiSystemReset::none
        ? MidiSystemReset::xg : system;
}

std::optional<XgPartModeChange> XgPartModes::observe(
    std::span<const std::uint8_t> sysex) noexcept
{
    constexpr std::size_t partOffset = 5;
    constexpr std::size_t addressOffset = 6;
    constexpr std::size_t dataOffset = 7;
    constexpr std::uint8_t partParameterGroup = 0x08;
    constexpr std::uint8_t partModeAddress = 0x07;

    if (sysex.size() <= dataOffset + 1 || sysex.front() != 0xf0
        || sysex.back() != 0xf7 || sysex[1] != 0x43
        || (sysex[2] & 0xf0) != 0x10 || sysex[3] != 0x4c
        || sysex[4] != partParameterGroup || sysex[partOffset] >= partCount) {
        return std::nullopt;
    }

    const auto part = static_cast<std::size_t>(sysex[partOffset]);
    const auto startAddress = sysex[addressOffset];
    for (std::size_t index = dataOffset; index + 1 < sysex.size(); ++index) {
        const auto address = static_cast<std::size_t>(startAddress)
            + index - dataOffset;
        if (address != partModeAddress)
            continue;
        const bool rhythm = sysex[index] != 0;
        const bool changed = rhythmParts[part] != rhythm;
        explicitPartModes[part] = true;
        if (!changed)
            return std::nullopt;
        rhythmParts[part] = rhythm;
        return XgPartModeChange { part, rhythm };
    }
    return std::nullopt;
}

std::optional<XgPartModeChange> XgPartModes::selectBankMsb(
    std::size_t part, std::uint8_t bankMsb) noexcept
{
    if (part >= partCount || explicitPartModes[part])
        return std::nullopt;

    bool rhythm = false;
    if (systemMode == MidiSystemReset::xg) {
        rhythm = bankMsb == rhythmBankMsb;
    } else if (systemMode == MidiSystemReset::gm2) {
        if (bankMsb == gm2RhythmBankMsb)
            rhythm = true;
        else if (bankMsb != gm2MelodicBankMsb)
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (rhythmParts[part] == rhythm)
        return std::nullopt;
    rhythmParts[part] = rhythm;
    return XgPartModeChange { part, rhythm };
}

bool XgPartModes::isRhythm(std::size_t part) const noexcept
{
    return part < rhythmParts.size() && rhythmParts[part];
}

std::uint8_t XgPartModes::effectiveBankMsb(
    std::size_t part, std::uint8_t selectedBankMsb) const noexcept
{
    if (!isRhythm(part))
        return selectedBankMsb;
    return systemMode == MidiSystemReset::gm2
        ? gm2RhythmBankMsb : rhythmBankMsb;
}

std::uint8_t XgPartModes::effectiveBankLsb(
    std::size_t part, std::uint8_t selectedBankLsb) const noexcept
{
    return isRhythm(part) ? 0 : selectedBankLsb;
}

} // namespace hybrid
