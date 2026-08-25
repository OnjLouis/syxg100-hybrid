#include "MidiRouter.h"

#include <cstdint>
#include <cstdio>

namespace {

std::uint32_t message(std::uint8_t status, std::uint8_t data1,
                      std::uint8_t data2 = 0)
{
    return status | (static_cast<std::uint32_t>(data1) << 8)
         | (static_cast<std::uint32_t>(data2) << 16);
}

bool expect(bool condition, const char* description)
{
    if (!condition)
        std::fprintf(stderr, "FAILED: %s\n", description);
    return condition;
}

} // namespace

int main()
{
    hybrid::MidiRouter router;
    bool passed = true;
    passed &= expect(router.routeShortMessage(message(0x90, 60, 100))
                         == hybrid::MidiDestination::xg,
                     "ordinary note is routed to XG");

    passed &= expect(router.observeShortMessage(message(0xb1, 0, 81)),
                     "VL MSB enters routing on channel 2");
    passed &= expect(router.observeShortMessage(message(0xb1, 32, 112)),
                     "VL LSB is routed");
    passed &= expect(router.observeShortMessage(message(0xc1, 42)),
                     "VL program is routed");
    passed &= expect(router.observeShortMessage(message(0x91, 67, 110)),
                     "VL note is routed");
    passed &= expect(!router.observeShortMessage(message(0x92, 67, 110)),
                     "other channels remain ordinary XG");

    passed &= expect(router.routeShortMessage(message(0xb1, 0, 0))
                         == hybrid::MidiDestination::both,
                     "leaving VL clears VL and reaches XG");
    passed &= expect(!router.observeShortMessage(message(0x91, 67, 110)),
                     "channel returns to ordinary XG");

    passed &= expect(router.observeShortMessage(message(0xb2, 0, 97)),
                     "channel enters VL before reset");
    router.reset();
    passed &= expect(!router.isVlChannel(2),
                     "system reset clears remembered VL banks");

    for (const auto bank : { 33, 81, 97 }) {
        hybrid::MidiRouter bankRouter;
        passed &= expect(bankRouter.observeShortMessage(message(0xb0, 0, bank)),
                         "documented VL/PVL bank is routed");
    }
    return passed ? 0 : 1;
}
