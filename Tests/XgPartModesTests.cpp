#include "XgPartModes.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main()
{
    hybrid::XgPartModes modes;
    expect(modes.isRhythm(9), "channel 10 defaults to rhythm");
    const auto melodicChannel10 = modes.selectBankMsb(9, 0);
    expect(melodicChannel10 && !melodicChannel10->rhythm,
           "a melodic bank releases channel 10 from its default rhythm mode");
    expect(modes.effectiveBankMsb(9, 0) == 0,
           "channel 10 preserves a melodic bank MSB");
    expect(modes.effectiveBankLsb(9, 115) == 115,
           "channel 10 preserves a melodic bank LSB");
    const auto drumChannel10 = modes.selectBankMsb(9, 127);
    expect(drumChannel10 && drumChannel10->rhythm,
           "drum bank 127 restores channel 10 rhythm mode");
    expect(modes.effectiveBankLsb(9, 115) == 0,
           "restored rhythm mode clears melodic bank variation");
    (void)modes.selectBankMsb(9, 0);

    const std::array<std::uint8_t, 9> enable {
        0xf0, 0x43, 0x10, 0x4c, 0x08, 0x05, 0x07, 0x02, 0xf7
    };
    const auto enabled = modes.observe(enable);
    expect(enabled && enabled->part == 5 && enabled->rhythm,
           "an explicit Yamaha part mode enables rhythm");
    (void)modes.selectBankMsb(5, 0);
    expect(modes.isRhythm(5),
           "explicit rhythm mode takes precedence over bank select");

    modes.reset();
    expect(modes.isRhythm(9) && !modes.isRhythm(5),
           "reset restores the XG rhythm-part default");

    for (const auto system : { hybrid::MidiSystemReset::gm1,
                               hybrid::MidiSystemReset::gs }) {
        modes.reset(system);
        const auto gsStyleBankSelect = modes.selectBankMsb(9, 0);
        expect(!gsStyleBankSelect && modes.isRhythm(9),
               "GM and GS bank-zero setup keeps channel 10 in rhythm mode");
    }

    constexpr std::array<std::uint8_t, 11> demoGsReset {
        0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7
    };
    modes.reset(hybrid::classifySystemReset(demoGsReset));
    expect(!modes.selectBankMsb(9, 0) && modes.isRhythm(9),
           "DEMO0002 GS Reset followed by bank zero keeps drums on channel 10");

    modes.reset(hybrid::MidiSystemReset::gm2);
    expect(!modes.selectBankMsb(9, 0) && modes.isRhythm(9),
           "GM2 bank zero setup keeps channel 10 in rhythm mode");
    expect(modes.effectiveBankMsb(9, 0) == 120,
           "GM2 rhythm mode selects the GM2 drum bank");
    expect(modes.selectBankMsb(9, 121) && !modes.isRhythm(9),
           "GM2 melodic bank releases channel 10 from rhythm mode");
    expect(modes.selectBankMsb(5, 120) && modes.isRhythm(5),
           "GM2 drum bank enables rhythm on another channel");

    modes.reset(hybrid::MidiSystemReset::xg);
    expect(modes.selectBankMsb(9, 0) && !modes.isRhythm(9),
           "XG bank selection can still make channel 10 melodic");
    return failures == 0 ? 0 : 1;
}
