#include "MidiRouter.h"

#include <algorithm>
#include <limits>

namespace hybrid {

MidiDestination MidiRouter::routeShortMessage(std::uint32_t packedMessage)
{
    const auto status = static_cast<std::uint8_t>(packedMessage & 0xff);
    if (status < 0x80 || status >= 0xf0)
        return MidiDestination::xg;

    const auto channel = static_cast<std::uint8_t>(status & 0x0f);
    const auto operation = static_cast<std::uint8_t>(status & 0xf0);
    const auto data1 = static_cast<std::uint8_t>((packedMessage >> 8) & 0x7f);
    const auto data2 = static_cast<std::uint8_t>((packedMessage >> 16) & 0x7f);
    const auto wasVl = isVlChannel(channel);
    const bool noteOn = operation == 0x90 && data2 != 0;
    const bool noteOff = operation == 0x80
        || (operation == 0x90 && data2 == 0);
    const bool retainedNoteRelease = noteOff
        && heldVlNotes[channel][data1] != 0;
    const bool sustain = operation == 0xb0 && data1 == 64;
    const bool retainedSustainRelease = sustain && data2 < 64
        && heldVlSustain[channel];
    const bool clearsHeldNotes = operation == 0xb0
        && (data1 == 120 || data1 == 123);
    const bool retainedAllNotesOff = clearsHeldNotes
        && (hasHeldVlNotes(channel) || heldVlSustain[channel]);
    const bool retainedXgNoteRelease = noteOff
        && heldXgNotes[channel][data1] != 0;
    const bool retainedXgSustainRelease = sustain && data2 < 64
        && heldXgSustain[channel];
    const bool retainedXgAllNotesOff = clearsHeldNotes
        && (hasHeldXgNotes(channel) || heldXgSustain[channel]);

    if (operation == 0xb0 && data1 == 0)
        bankMsb[channel] = data2;
    else if (operation == 0xb0 && data1 == 32)
        bankLsb[channel] = data2;

    const auto isVl = isVlChannel(channel);
    if (noteOn) {
        auto& count = isVl ? heldVlNotes[channel][data1]
                           : heldXgNotes[channel][data1];
        if (count != std::numeric_limits<std::uint16_t>::max())
            ++count;
    } else if (noteOff) {
        if (heldVlNotes[channel][data1] != 0)
            --heldVlNotes[channel][data1];
        if (heldXgNotes[channel][data1] != 0)
            --heldXgNotes[channel][data1];
    }
    if (sustain) {
        if (data2 >= 64) {
            if (isVl)
                heldVlSustain[channel] = true;
            else
                heldXgSustain[channel] = true;
        } else {
            heldVlSustain[channel] = false;
            heldXgSustain[channel] = false;
        }
    }
    if (clearsHeldNotes) {
        heldVlNotes[channel].fill(0);
        heldXgNotes[channel].fill(0);
    }

    const auto isBankSelection = operation == 0xb0 && (data1 == 0 || data1 == 32);
    if (isBankSelection && wasVl && !isVl)
        return MidiDestination::both;
    if (!isVl && (retainedNoteRelease || retainedSustainRelease
                  || retainedAllNotesOff)) {
        return MidiDestination::both;
    }
    if (isVl && (retainedXgNoteRelease || retainedXgSustainRelease
                 || retainedXgAllNotesOff)) {
        return MidiDestination::both;
    }
    return isVl ? MidiDestination::vl : MidiDestination::xg;
}

bool MidiRouter::observeShortMessage(std::uint32_t packedMessage)
{
    return routeShortMessage(packedMessage) != MidiDestination::xg;
}

bool MidiRouter::isVlChannel(std::uint8_t channel) const
{
    return channel < bankMsb.size() && isVlBank(bankMsb[channel]);
}

void MidiRouter::reset()
{
    bankMsb.fill(0);
    bankLsb.fill(0);
    for (auto& channel : heldVlNotes)
        channel.fill(0);
    for (auto& channel : heldXgNotes)
        channel.fill(0);
    heldVlSustain.fill(false);
    heldXgSustain.fill(false);
}

bool MidiRouter::isVlBank(std::uint8_t value)
{
    return value == 33 || value == 81 || value == 97;
}

bool MidiRouter::hasHeldVlNotes(std::uint8_t channel) const noexcept
{
    return std::any_of(heldVlNotes[channel].begin(),
                       heldVlNotes[channel].end(),
                       [](std::uint16_t count) { return count != 0; });
}

bool MidiRouter::hasHeldXgNotes(std::uint8_t channel) const noexcept
{
    return std::any_of(heldXgNotes[channel].begin(),
                       heldXgNotes[channel].end(),
                       [](std::uint16_t count) { return count != 0; });
}

} // namespace hybrid
