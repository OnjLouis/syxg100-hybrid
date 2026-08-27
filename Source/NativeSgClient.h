#pragma once

#include "NativeVlProtocol.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace hybrid {

class NativeSgClient {
public:
    static constexpr std::uint32_t maxFrames = ipc::maxFrames;
    static constexpr std::size_t planeCount = ipc::planeCount;

    NativeSgClient(const std::filesystem::path& workerPath,
                   const std::filesystem::path& vxdPath,
                   std::uint32_t sampleRate);
    ~NativeSgClient();

    NativeSgClient(const NativeSgClient&) = delete;
    NativeSgClient& operator=(const NativeSgClient&) = delete;

    void sendShort(std::uint32_t packedMessage);
    void sendSysex(std::span<const std::uint8_t> bytes);
    void setSampleRate(std::uint32_t sampleRate);
    void render(std::uint32_t frames);
    [[nodiscard]] std::uint32_t routeMask();
    [[nodiscard]] std::span<const std::int16_t> plane(
        std::size_t index, std::uint32_t frames) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace hybrid
