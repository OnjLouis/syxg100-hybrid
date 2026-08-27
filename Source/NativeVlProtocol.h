#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hybrid::ipc {

constexpr std::size_t maxSysexBytes = 65'536;
constexpr std::size_t maxDiagnosticBytes = 512;
constexpr std::uint32_t maxFrames = 2048;
constexpr std::size_t planeCount = 4;
constexpr std::size_t maxStereoSamples = maxFrames * 2;
constexpr std::size_t maxTimedMidiEvents = 512;

struct TimedMidiEvent {
    std::uint32_t frameOffset {};
    std::uint32_t message {};
};

enum class Command : LONG {
    none,
    sendShort,
    sendSysex,
    setSampleRate,
    warmUp,
    prepare,
    render,
    renderTimed,
    getRouteMask,
    shutdown,
};

struct SharedState {
    volatile LONG command {};
    volatile LONG result {};
    std::uint32_t argument {};
    std::uint32_t dataSize {};
    std::uint32_t timedMidiEventCount {};
    std::uint32_t diagnosticSize {};
    std::array<char, maxDiagnosticBytes> diagnostic {};
    std::array<std::uint8_t, maxSysexBytes> data {};
    std::array<TimedMidiEvent, maxTimedMidiEvents> timedMidiEvents {};
    std::array<std::int16_t, maxStereoSamples * planeCount> audio {};
};

static_assert(std::is_standard_layout_v<SharedState>);

} // namespace hybrid::ipc
