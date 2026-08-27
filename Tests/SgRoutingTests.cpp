#include "SgRouting.h"

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
    const std::array<std::uint8_t, 9> sg {
        0xf0, 0x43, 0x10, 0x5d, 0x50, 0x00, 0x42, 0x00, 0xf7
    };
    const std::array<std::uint8_t, 9> xg {
        0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7
    };
    const std::array<std::uint8_t, 4> truncated {
        0xf0, 0x43, 0x10, 0x5d
    };
    expect(hybrid::isSgConfiguration(sg), "SG model SysEx is detected");
    expect(!hybrid::isSgConfiguration(xg), "ordinary XG SysEx is ignored");
    expect(!hybrid::isSgConfiguration(truncated),
           "truncated SG SysEx is ignored");

    expect(hybrid::sgOwnsNote(0x00643c90, 0x0001),
           "routed channel note-on belongs to SG");
    expect(hybrid::sgOwnsNote(0x00003c80, 0x0001),
           "routed channel note-off belongs to SG");
    expect(hybrid::sgOwnsNote(0x00003c90, 0x0001),
           "zero-velocity routed note-on belongs to SG");
    expect(!hybrid::sgOwnsNote(0x00643c91, 0x0001),
           "unrouted channel note remains in XG");
    expect(!hybrid::sgOwnsNote(0x00075bb0, 0x0001),
           "controllers continue to XG");
    return failures == 0 ? 0 : 1;
}
