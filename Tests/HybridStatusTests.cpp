#include "HybridStatus.h"

#include <cassert>
#include <cstdint>

namespace {

std::uint32_t message(std::uint8_t operation, std::uint8_t channel,
                      std::uint8_t data1, std::uint8_t data2 = 0)
{
    return static_cast<std::uint32_t>(operation | channel)
        | (static_cast<std::uint32_t>(data1) << 8)
        | (static_cast<std::uint32_t>(data2) << 16);
}

} // namespace

int main()
{
    hybrid::HybridStatus status;
    status.setSupplementalEngineAvailable(true);
    assert(status.snapshot().supplementalEngineAvailable);
    auto snapshot = status.snapshot();
    assert(snapshot.channels[2].engine == hybrid::DisplayEngine::xg);
    assert(snapshot.channels[2].volume == 100);
    assert(snapshot.channels[2].pan == 64);
    assert(snapshot.channels[2].expression == 127);

    status.setAvailability(true, true);
    status.setEffectsBridgeAvailable(true);
    status.setSampleRate(48'000);
    status.observeShortMessage(message(0xb0, 2, 0, 33), true, false);
    status.observeShortMessage(message(0xb0, 2, 32, 4), true, false);
    status.observeShortMessage(message(0xc0, 2, 18), true, false);
    status.observeShortMessage(message(0xb0, 2, 2, 96), true, false);
    status.observeShortMessage(message(0xb0, 2, 11, 105), true, false);
    status.observeShortMessage(message(0xe0, 2, 127, 127), true, false);
    status.observeShortMessage(message(0x90, 2, 67, 110), true, false);
    snapshot = status.snapshot();
    assert(snapshot.vlAvailable);
    assert(snapshot.sgAvailable);
    assert(snapshot.effectsBridgeAvailable);
    assert(snapshot.sampleRate == 48'000);
    assert(snapshot.channels[2].engine == hybrid::DisplayEngine::vl);
    assert(snapshot.channels[2].bankMsb == 33);
    assert(snapshot.channels[2].bankLsb == 4);
    assert(snapshot.channels[2].program == 18);
    assert(snapshot.channels[2].breath == 96);
    assert(snapshot.channels[2].expression == 105);
    assert(snapshot.channels[2].pitchBend == 8191);
    assert(snapshot.channels[2].lastNote == 67);
    assert(snapshot.channels[2].velocity == 110);
    assert(snapshot.channels[2].activeNotes == 1);

    status.setSgState(true, false);
    status.setSgRouteMask(1u << 2);
    snapshot = status.snapshot();
    assert(snapshot.sgActive);
    assert(snapshot.channels[2].engine == hybrid::DisplayEngine::sg);

    status.configureVlWorker(3, 2);
    status.setVlWorkerState(3, hybrid::WorkerDisplayState::active);
    snapshot = status.snapshot();
    assert(snapshot.explicitVlAssignments);
    assert(snapshot.vlWorkers[3] == hybrid::WorkerDisplayState::active);
    assert(snapshot.vlWorkerChannels[3] == 2);

    status.observeShortMessage(message(0x80, 2, 67), true, true);
    snapshot = status.snapshot();
    assert(snapshot.channels[2].activeNotes == 0);
    assert(snapshot.channels[2].velocity == 0);

    status.reset(hybrid::ResetDisplayState::gm2);
    snapshot = status.snapshot();
    assert(snapshot.lastReset == hybrid::ResetDisplayState::gm2);
    assert(snapshot.sgRouteMask == 0);
    assert(!snapshot.explicitVlAssignments);
    assert(snapshot.channels[2].engine == hybrid::DisplayEngine::xg);
    assert(snapshot.channels[2].bankMsb == 0);
    assert(snapshot.channels[2].program == 0);
    assert(snapshot.channels[2].expression == 127);
    assert(snapshot.vlWorkers[3] == hybrid::WorkerDisplayState::idle);
    assert(snapshot.vlWorkerChannels[3] == 0xff);

    hybrid::HybridStatus playbackStatus;
    hybrid::HybridStatus detachedEditorStatus;
    playbackStatus.markMidiActivity();
    playbackStatus.observeShortMessage(message(0xb0, 5, 0, 33), true, false);
    playbackStatus.observeShortMessage(message(0xb0, 5, 32, 2), true, false);
    playbackStatus.observeShortMessage(message(0xc0, 5, 47), true, false);
    playbackStatus.observeShortMessage(message(0x90, 5, 72, 100), true, false);
    playbackStatus.publishShared();

    snapshot = detachedEditorStatus.displaySnapshot();
    assert(snapshot.latestPlaybackInstance);
    assert(snapshot.channels[5].engine == hybrid::DisplayEngine::vl);
    assert(snapshot.channels[5].bankMsb == 33);
    assert(snapshot.channels[5].bankLsb == 2);
    assert(snapshot.channels[5].program == 47);
    assert(snapshot.channels[5].lastNote == 72);
    assert(snapshot.channels[5].activeNotes == 1);

    snapshot = playbackStatus.displaySnapshot();
    assert(!snapshot.latestPlaybackInstance);
    assert(snapshot.channels[5].program == 47);

    hybrid::HybridStatus activeEditorStatus;
    activeEditorStatus.markMidiActivity();
    activeEditorStatus.observeShortMessage(
        message(0xc0, 5, 9), false, false);
    snapshot = activeEditorStatus.displaySnapshot();
    assert(!snapshot.latestPlaybackInstance);
    assert(snapshot.channels[5].program == 9);
    return 0;
}
