#include "MasterVolumeTimeline.h"

#include <array>
#include <cassert>
#include <cmath>

int main()
{
    using hybrid::MasterVolumeTimeline;
    std::array<std::uint8_t, 9> volume {
        0xf0,0x43,0x10,0x4c,0,0,4,127,0xf7
    };
    const std::array<std::uint8_t, 9> reset {
        0xf0,0x43,0x10,0x4c,0,0,0x7e,0,0xf7
    };
    const std::array<std::uint8_t, 8> universal {
        0xf0,0x7f,0x7f,4,1,0x7f,0x7f,0xf7
    };
    assert(MasterVolumeTimeline::messageGain(volume) == 1.0f);
    assert(MasterVolumeTimeline::messageGain(universal) == 1.0f);
    volume[7] = 64;
    assert(std::abs(*MasterVolumeTimeline::messageGain(volume) - .254f) < .001f);
    volume[7] = 0;
    assert(MasterVolumeTimeline::messageGain(volume) == 0.0f);
    MasterVolumeTimeline timeline;
    assert(timeline.observe(volume, 128));
    assert(timeline.observe(reset, 256));
    assert(timeline.gainAt(127) == 1.0f);
    assert(timeline.gainAt(128) == 0.0f);
    assert(timeline.gainAt(255) == 0.0f);
    assert(timeline.gainAt(256) == 1.0f);
    volume[7] = 127;
    assert(timeline.observe(volume, 400));
    volume[7] = 0;
    assert(timeline.observe(volume, 350));
    assert(timeline.gainAt(350) == 0.0f);
    assert(timeline.gainAt(400) == 1.0f);
    volume[8] = 0;
    assert(!MasterVolumeTimeline::messageGain(volume));
    volume[8] = 0xf7;
    volume[7] = 128;
    assert(!MasterVolumeTimeline::messageGain(volume));
    volume[7] = 0;
    volume[2] = 0x20;
    assert(!MasterVolumeTimeline::messageGain(volume));
    volume[2] = 0x10;
    MasterVolumeTimeline bounded;
    for (std::size_t i = 0; i < MasterVolumeTimeline::capacity; ++i)
        assert(bounded.observe(volume, i));
    assert(!bounded.observe(volume, 5000));
    assert(bounded.gainAt(5000) == 0.0f);
    assert(bounded.observe(reset, 5001));
    assert(bounded.gainAt(5001) == 1.0f);
}
