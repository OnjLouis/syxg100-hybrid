#include "VlPluginVoiceBulk.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 11> header {
    0xf0, 0x43, 0x00, 0x64, 0x00, 0x00, 0x0e, 0x1f, 0x00, 0x5a, 0xf7
};
constexpr std::array<std::uint8_t, 11> footer {
    0xf0, 0x43, 0x00, 0x64, 0x00, 0x00, 0x0f, 0x1f, 0x00, 0x59, 0xf7
};

std::vector<std::uint8_t> block(
    std::array<std::uint8_t, 3> address,
    std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> bytes {
        0xf0, 0x43, 0x00, 0x64,
        static_cast<std::uint8_t>(data.size() >> 7),
        static_cast<std::uint8_t>(data.size() & 0x7f),
        address[0], address[1], address[2],
    };
    bytes.insert(bytes.end(), data.begin(), data.end());
    std::uint32_t sum {};
    for (std::size_t index = 4; index < bytes.size(); ++index)
        sum += bytes[index];
    bytes.push_back(static_cast<std::uint8_t>((-sum) & 0x7f));
    bytes.push_back(0xf7);
    return bytes;
}

std::vector<std::uint8_t> element(std::uint8_t msb, std::uint8_t lsb,
                                  std::uint8_t program)
{
    std::array<std::uint8_t, 35> data {};
    data[0] = msb;
    data[1] = lsb;
    data[2] = program;
    return block({0x4c, 0x10, 0x00}, data);
}

std::vector<std::uint8_t> common()
{
    constexpr std::array<std::uint8_t, 13> data {
        60, 1, 0, 0, 0, 66, 62, 0, 1, 12, 0, 29, 17
    };
    return block({0x4c, 0x00, 0x00}, data);
}

} // namespace

int main()
{
    hybrid::VlPluginVoiceBulk parser;
    assert(!parser.observe(header));
    assert(!parser.observe(common()));
    assert(!parser.observe(element(81, 114, 66)));
    const auto first = parser.observe(footer);
    assert(first.has_value());
    assert(first->bankMsb == 81);
    assert(first->bankLsb == 114);
    assert(first->program == 66);
    assert(first->volume == 60);
    assert(first->monoPoly == 0);
    assert(first->pitchBendRange == 2);
    assert(first->portamentoSwitch == 1);
    assert(first->portamentoTime == 12);
    assert(first->reverbSend == 29);
    assert(first->chorusSend == 17);

    assert(!parser.observe(header));
    assert(!parser.observe(element(81, 112, 65)));
    const auto second = parser.observe(footer);
    assert(second.has_value());
    assert(second->bankLsb == 112);
    assert(second->program == 65);
    assert(second->volume == 127);

    auto damaged = element(33, 1, 48);
    damaged[12] ^= 1;
    assert(!parser.observe(header));
    assert(!parser.observe(damaged));
    assert(!parser.observe(footer));

    assert(!parser.observe(header));
    assert(!parser.observe(common()));
    assert(!parser.observe(footer));

    assert(!parser.observe(header));
    constexpr std::array<std::uint8_t, 3> foreignData {1, 2, 3};
    const auto foreign = block({0x40, 0x00, 0x00}, foreignData);
    assert(!parser.observe(foreign));
    assert(!parser.observe(element(33, 1, 48)));
    assert(!parser.observe(footer));

    assert(!parser.observe(header));
    assert(!parser.observe(element(33, 1, 48)));
    const auto recovered = parser.observe(footer);
    assert(recovered.has_value());
    assert(recovered->bankMsb == 33);
    assert(recovered->bankLsb == 1);
    assert(recovered->program == 48);
}
