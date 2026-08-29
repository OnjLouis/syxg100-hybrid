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

    hybrid::MidiRouter lsbFirstRouter;
    passed &= expect(
        lsbFirstRouter.routeShortMessage(message(0xb0, 32, 1))
            == hybrid::MidiDestination::both,
        "QWS bank LSB reaches both engines before the bank MSB");
    passed &= expect(
        lsbFirstRouter.routeShortMessage(message(0xb0, 0, 33))
            == hybrid::MidiDestination::both,
        "QWS bank MSB keeps both engines synchronized");
    passed &= expect(
        lsbFirstRouter.routeShortMessage(message(0xc0, 71))
            == hybrid::MidiDestination::vl,
        "QWS program change uses the selected VL bank");

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

    hybrid::MidiRouter releaseRouter;
    (void)releaseRouter.routeShortMessage(message(0xb0, 0, 33));
    (void)releaseRouter.routeShortMessage(message(0x90, 48, 90));
    (void)releaseRouter.routeShortMessage(message(0xb0, 0, 0));
    passed &= expect(releaseRouter.routeShortMessage(message(0x90, 48, 0))
                         == hybrid::MidiDestination::both,
                     "VL note release survives a bank change to XG");
    passed &= expect(releaseRouter.routeShortMessage(message(0x90, 48, 0))
                         == hybrid::MidiDestination::xg,
                     "released VL note no longer affects XG routing");

    hybrid::MidiRouter repeatedNoteRouter;
    (void)repeatedNoteRouter.routeShortMessage(message(0xb0, 0, 33));
    (void)repeatedNoteRouter.routeShortMessage(message(0x90, 60, 80));
    (void)repeatedNoteRouter.routeShortMessage(message(0x90, 60, 100));
    (void)repeatedNoteRouter.routeShortMessage(message(0xb0, 0, 0));
    passed &= expect(
        repeatedNoteRouter.routeShortMessage(message(0x80, 60, 0))
            == hybrid::MidiDestination::both,
        "first overlapping VL note release survives a bank change");
    passed &= expect(
        repeatedNoteRouter.routeShortMessage(message(0x80, 60, 0))
            == hybrid::MidiDestination::both,
        "second overlapping VL note release survives a bank change");
    passed &= expect(
        repeatedNoteRouter.routeShortMessage(message(0x80, 60, 0))
            == hybrid::MidiDestination::xg,
        "all overlapping VL note releases are eventually cleared");

    hybrid::MidiRouter sustainRouter;
    (void)sustainRouter.routeShortMessage(message(0xb0, 0, 33));
    (void)sustainRouter.routeShortMessage(message(0xb0, 64, 127));
    (void)sustainRouter.routeShortMessage(message(0xb0, 0, 0));
    passed &= expect(sustainRouter.routeShortMessage(message(0xb0, 64, 0))
                         == hybrid::MidiDestination::both,
                     "VL sustain release survives a bank change to XG");
    passed &= expect(sustainRouter.routeShortMessage(message(0xb0, 64, 0))
                         == hybrid::MidiDestination::xg,
                     "released VL sustain no longer affects XG routing");

    hybrid::MidiRouter allNotesOffRouter;
    (void)allNotesOffRouter.routeShortMessage(message(0xb0, 0, 33));
    (void)allNotesOffRouter.routeShortMessage(message(0x90, 60, 100));
    (void)allNotesOffRouter.routeShortMessage(message(0xb0, 0, 0));
    passed &= expect(
        allNotesOffRouter.routeShortMessage(message(0xb0, 123, 0))
            == hybrid::MidiDestination::both,
        "all-notes-off clears VL notes after a bank change");
    passed &= expect(
        allNotesOffRouter.routeShortMessage(message(0x80, 60, 0))
            == hybrid::MidiDestination::xg,
        "all-notes-off removes retained VL releases");

    hybrid::MidiRouter resetReleaseRouter;
    (void)resetReleaseRouter.routeShortMessage(message(0xb0, 0, 33));
    (void)resetReleaseRouter.routeShortMessage(message(0x90, 60, 100));
    (void)resetReleaseRouter.routeShortMessage(message(0xb0, 0, 0));
    resetReleaseRouter.reset();
    passed &= expect(
        resetReleaseRouter.routeShortMessage(message(0x80, 60, 0))
            == hybrid::MidiDestination::xg,
        "system reset clears retained VL releases");

    hybrid::MidiRouter xgToVlReleaseRouter;
    (void)xgToVlReleaseRouter.routeShortMessage(message(0x90, 64, 100));
    (void)xgToVlReleaseRouter.routeShortMessage(message(0xb0, 0, 33));
    passed &= expect(
        xgToVlReleaseRouter.routeShortMessage(message(0x80, 64, 0))
            == hybrid::MidiDestination::both,
        "XG note release survives a bank change to VL");
    passed &= expect(
        xgToVlReleaseRouter.routeShortMessage(message(0x80, 64, 0))
            == hybrid::MidiDestination::vl,
        "released XG note no longer affects VL routing");

    hybrid::MidiRouter xgToVlSustainRouter;
    (void)xgToVlSustainRouter.routeShortMessage(message(0xb0, 64, 127));
    (void)xgToVlSustainRouter.routeShortMessage(message(0xb0, 0, 33));
    passed &= expect(
        xgToVlSustainRouter.routeShortMessage(message(0xb0, 64, 0))
            == hybrid::MidiDestination::both,
        "XG sustain release survives a bank change to VL");

    hybrid::MidiRouter xgToVlAllNotesOffRouter;
    (void)xgToVlAllNotesOffRouter.routeShortMessage(message(0x90, 67, 100));
    (void)xgToVlAllNotesOffRouter.routeShortMessage(message(0xb0, 0, 33));
    passed &= expect(
        xgToVlAllNotesOffRouter.routeShortMessage(message(0xb0, 123, 0))
            == hybrid::MidiDestination::both,
        "all-notes-off clears XG notes after a bank change to VL");
    passed &= expect(
        xgToVlAllNotesOffRouter.routeShortMessage(message(0x80, 67, 0))
            == hybrid::MidiDestination::vl,
        "all-notes-off removes retained XG releases");
    return passed ? 0 : 1;
}
