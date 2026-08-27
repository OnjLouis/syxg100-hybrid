#include "VlPartRouter.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    using hybrid::VlSysexRoute;

    assert(hybrid::remapVlShortMessage(0x00643c94) == 0x00643c92);
    assert(hybrid::remapVlShortMessage(0x00643c94, 0) == 0x00643c90);
    assert(hybrid::remapVlShortMessage(0x000000f8) == 0x000000f8);
    assert(hybrid::nativeVlChannel(false, 0) == 0);
    assert(hybrid::nativeVlChannel(true, 0)
           == hybrid::canonicalVlChannel);

    const std::array<std::uint8_t, 9> sourcePart {
        0xf0, 0x43, 0x10, 0x4c, 0x09, 0x03, 0x03, 0x61, 0xf7
    };
    assert(hybrid::routeVlSysex(sourcePart, 3)
           == VlSysexRoute::remapPart);
    assert(hybrid::routeVlSysex(sourcePart, 4) == VlSysexRoute::drop);

    const std::array<std::uint8_t, 9> globalReset {
        0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7
    };
    assert(hybrid::routeVlSysex(globalReset, 3)
           == VlSysexRoute::passThrough);

    const std::array<std::uint8_t, 9> sourceVoiceAssignment {
        0xf0, 0x43, 0x10, 0x4c, 0x70, 0x00, 0x01, 0x01, 0xf7
    };
    assert(hybrid::routeVlSysex(sourceVoiceAssignment, 1)
           == VlSysexRoute::remapVoiceAssignment);
    assert(hybrid::routeVlSysex(sourceVoiceAssignment, 0)
           == VlSysexRoute::drop);
    const auto assignment = hybrid::vlVoiceAssignment(sourceVoiceAssignment);
    assert(assignment.has_value());
    assert(assignment->voice == 1);
    assert(assignment->channel == 1);
    assert(!hybrid::vlVoiceAssignment(globalReset).has_value());

    auto remappedVoiceAssignment = sourceVoiceAssignment;
    hybrid::applyVlSysexRoute(remappedVoiceAssignment,
                              VlSysexRoute::remapVoiceAssignment);
    assert(remappedVoiceAssignment[6] == 0);
    assert(remappedVoiceAssignment[7] == hybrid::canonicalVlChannel);

    auto legacyPart = sourcePart;
    hybrid::applyVlSysexRoute(legacyPart, VlSysexRoute::remapPart, 0);
    assert(legacyPart[5] == 0);
}
