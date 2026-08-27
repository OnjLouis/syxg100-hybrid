#include "MidiChannelSnapshot.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

std::uint32_t midi(std::uint8_t status, std::uint8_t data1,
                   std::uint8_t data2 = 0)
{
    return status | (static_cast<std::uint32_t>(data1) << 8)
        | (static_cast<std::uint32_t>(data2) << 16);
}

} // namespace

int main()
{
    hybrid::MidiChannelSnapshot snapshot;
    snapshot.observe(midi(0xb2, 0, 97));
    snapshot.observe(midi(0xb2, 32, 112));
    snapshot.observe(midi(0xc2, 35));
    snapshot.observe(midi(0xb2, 7, 90));
    snapshot.observe(midi(0xb2, 101, 0));
    snapshot.observe(midi(0xb2, 100, 0));
    snapshot.observe(midi(0xb2, 6, 12));
    snapshot.observe(midi(0xb2, 101, 127));
    snapshot.observe(midi(0xb2, 100, 127));
    snapshot.observe(midi(0xe2, 0, 96));

    std::vector<std::uint32_t> replayed;
    snapshot.replay([&](std::uint32_t message) {
        replayed.push_back(message);
    });
    const std::vector<std::uint32_t> expected {
        midi(0xb2, 0, 97), midi(0xb2, 32, 112), midi(0xc2, 35),
        midi(0xb2, 7, 90), midi(0xb2, 101, 0), midi(0xb2, 100, 0),
        midi(0xb2, 6, 12), midi(0xb2, 101, 127), midi(0xb2, 100, 127),
        midi(0xe2, 0, 96),
    };
    assert(replayed == expected);
}
