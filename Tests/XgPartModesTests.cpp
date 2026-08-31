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
    return failures == 0 ? 0 : 1;
}
