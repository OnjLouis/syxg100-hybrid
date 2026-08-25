#include "MidiRouter.h"

namespace hybrid {

bool MidiRouter::observeShortMessage(std::uint32_t packedMessage)
{
    const auto status = static_cast<std::uint8_t>(packedMessage & 0xff);
    if (status < 0x80 || status >= 0xf0)
        return false;

    const auto channel = static_cast<std::uint8_t>(status & 0x0f);
    const auto operation = static_cast<std::uint8_t>(status & 0xf0);
    const auto data1 = static_cast<std::uint8_t>((packedMessage >> 8) & 0x7f);
    const auto data2 = static_cast<std::uint8_t>((packedMessage >> 16) & 0x7f);
    const auto wasVl = isVlChannel(channel);

    if (operation == 0xb0 && data1 == 0)
        bankMsb[channel] = data2;
    else if (operation == 0xb0 && data1 == 32)
        bankLsb[channel] = data2;

    const auto isVl = isVlChannel(channel);
    const auto isBankSelection = operation == 0xb0 && (data1 == 0 || data1 == 32);
    return isVl || (isBankSelection && wasVl);
}

bool MidiRouter::isVlChannel(std::uint8_t channel) const
{
    return channel < bankMsb.size() && isVlBank(bankMsb[channel]);
}

bool MidiRouter::isVlBank(std::uint8_t value)
{
    return value == 33 || value == 81 || value == 97;
}

} // namespace hybrid

