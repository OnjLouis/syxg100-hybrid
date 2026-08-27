#pragma once

#include "NativeVlProtocol.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace hybrid {

class NativeSgEngine {
public:
    NativeSgEngine(const std::filesystem::path& vxdPath,
                   std::uint32_t sampleRate);
    ~NativeSgEngine();

    NativeSgEngine(const NativeSgEngine&) = delete;
    NativeSgEngine& operator=(const NativeSgEngine&) = delete;

    void sendShort(std::uint32_t packedMessage);
    void sendSysex(std::span<const std::uint8_t> bytes);
    void setSampleRate(std::uint32_t sampleRate);
    void render(std::uint32_t frames);
    [[nodiscard]] std::uint32_t routeMask() const;
    [[nodiscard]] std::span<const std::int16_t> plane(
        std::size_t index, std::uint32_t frames) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace hybrid
