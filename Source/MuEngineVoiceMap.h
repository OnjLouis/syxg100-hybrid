#pragma once

#include "Vst2Abi.h"
#include <array>
#include <cstdint>
#include <algorithm>
#include <span>

namespace hybrid {

inline bool applyMuEngineVoiceMap(vst2::AEffect* child,
                                  std::span<const std::uint8_t> message) noexcept
{
    constexpr std::int32_t getChunk = 23;
    constexpr std::int32_t setChunk = 24;
    constexpr std::size_t bankChunkSize = 36;
    if (child == nullptr || message.size() != 9 || message[0] != 0xf0
        || message[1] != 0x43 || (message[2] & 0xf0) != 0x10
        || message[3] != 0x49 || message[4] != 0 || message[5] != 0
        || message[6] != 0x12 || message[7] > 1 || message[8] != 0xf7)
        return false;

    void* data = nullptr;
    const auto size = child->dispatcher(child, getChunk, 0, 0, &data, 0);
    if (size != bankChunkSize || data == nullptr)
        return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (bytes[bankChunkSize - 3] != 'V' || bytes[bankChunkSize - 2] != 'M')
        return false;

    // SXG-Create's extended chunk uses 1 for Basic, unlike Yamaha's SysEx.
    // Copy its bounded state before setChunk, which may reuse the source buffer.
    std::array<std::uint8_t, bankChunkSize> state {};
    std::copy_n(bytes, state.size(), state.begin());
    state.back() = message[7] == 0 ? 1 : 0;
    child->dispatcher(child, setChunk, 0, state.size(), state.data(), 0);
    return true;
}

} // namespace hybrid
