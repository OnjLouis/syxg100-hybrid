#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace hybrid {

class NativeVlEngine {
public:
    static constexpr std::uint32_t maxFrames = 2048;
    static constexpr std::size_t planeCount = 4;

    NativeVlEngine(const std::filesystem::path& vxdPath,
                   std::uint32_t sampleRate);
    ~NativeVlEngine();

    NativeVlEngine(const NativeVlEngine&) = delete;
    NativeVlEngine& operator=(const NativeVlEngine&) = delete;

    void sendShort(std::uint32_t packedMessage);
    void sendSysex(std::span<const std::uint8_t> bytes);
    void setSampleRate(std::uint32_t sampleRate);
    void prepare();
    void render(std::uint32_t frames);
    [[nodiscard]] std::span<const std::int16_t> plane(
        std::size_t index, std::uint32_t frames) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace hybrid
