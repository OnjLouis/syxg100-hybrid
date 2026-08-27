#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace hybrid {

class XgEffectsBridge {
public:
    using MixCallback = void (*)(void* context, float* buses,
                                 std::uint32_t frames) noexcept;

    static constexpr std::uint32_t quantumFrames = 128;
    static constexpr std::uint32_t busStrideFrames = 128;
    static constexpr std::size_t busCount = 10;

    static bool acquire(HMODULE module) noexcept;
    static void release(HMODULE module) noexcept;
    static void beginBlock(void* context, MixCallback callback) noexcept;
    static void endBlock() noexcept;
};

} // namespace hybrid
