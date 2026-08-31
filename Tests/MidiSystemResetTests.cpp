#include "MidiSystemReset.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {

void expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

} // namespace

int main()
{
    using hybrid::MidiSystemReset;
    using hybrid::classifySystemReset;

    constexpr std::array<std::uint8_t, 6> gm1 {
        0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7
    };
    constexpr std::array<std::uint8_t, 6> gm2 {
        0xf0, 0x7e, 0x7f, 0x09, 0x03, 0xf7
    };
    constexpr std::array<std::uint8_t, 11> gs {
        0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7
    };
    constexpr std::array<std::uint8_t, 9> xg {
        0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7
    };

    expect(classifySystemReset(gm1) == MidiSystemReset::gm1,
           "GM1 System On is recognized");
    expect(classifySystemReset(gm2) == MidiSystemReset::gm2,
           "GM2 System On is recognized");
    expect(classifySystemReset(gs) == MidiSystemReset::gs,
           "GS Reset is recognized");
    expect(classifySystemReset(xg) == MidiSystemReset::xg,
           "XG System On is recognized");

    auto addressedGm2 = gm2;
    addressedGm2[2] = 0x05;
    expect(classifySystemReset(addressedGm2) == MidiSystemReset::gm2,
           "addressed GM2 System On is recognized");
    auto addressedGs = gs;
    addressedGs[2] = 0x11;
    expect(classifySystemReset(addressedGs) == MidiSystemReset::gs,
           "addressed GS Reset is recognized");
    auto addressedXg = xg;
    addressedXg[2] = 0x1f;
    expect(classifySystemReset(addressedXg) == MidiSystemReset::xg,
           "addressed XG System On is recognized");

    auto malformedGm1 = gm1;
    malformedGm1.back() = 0;
    expect(classifySystemReset(malformedGm1) == MidiSystemReset::none,
           "GM1 reset without EOX is rejected");
    auto malformedGs = gs;
    malformedGs[9] = 0;
    expect(classifySystemReset(malformedGs) == MidiSystemReset::none,
           "GS reset with bad checksum is rejected");
    auto malformedXg = xg;
    malformedXg[2] = 0x20;
    expect(classifySystemReset(malformedXg) == MidiSystemReset::none,
           "XG reset with invalid device byte is rejected");
    expect(classifySystemReset(std::span {xg}.first(8))
               == MidiSystemReset::none,
           "truncated XG reset is rejected");
}
