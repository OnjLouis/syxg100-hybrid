#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hybrid::ipc {

constexpr std::size_t maxSysexBytes = 65'536;
constexpr std::uint32_t maxFrames = 2048;
constexpr std::size_t planeCount = 4;
constexpr std::size_t maxStereoSamples = 2048 * 2;

enum class Command : LONG {
    none,
    sendShort,
    sendSysex,
    setSampleRate,
    prepare,
    render,
    shutdown,
};

struct SharedState {
    volatile LONG command {};
    volatile LONG result {};
    std::uint32_t argument {};
    std::uint32_t dataSize {};
    std::array<std::uint8_t, maxSysexBytes> data {};
    std::array<std::int16_t, maxStereoSamples * planeCount> audio {};
};

static_assert(std::is_standard_layout_v<SharedState>);

} // namespace hybrid::ipc
