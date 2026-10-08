#pragma once

#include "Vst2Abi.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace hybrid {

// MU Effect 2 has four independent insertions. Only algorithms shared with
// the legacy XG DSP are translated; unknown algorithms remain dry.
class MuExternalInsertions {
public:
    static constexpr std::size_t slotCount = 4;
    static constexpr std::size_t busesPerSlot = 8;

    struct Slot {
        std::uint8_t msb {}, lsb {}, part {127};
        std::array<std::uint8_t, 16> parameters {};
        std::array<bool, 16> parameterSeen {};
        std::array<std::uint8_t, 5> controlDepths {};
        std::uint64_t revision {};
        std::uint64_t typeRevision {};
        bool supported() const noexcept {
            return (msb == 0x49 || msb == 0x4a) && lsb == 0 && part < 16
                && controlDepths == std::array<std::uint8_t, 5>{};
        }
    };

    void reset() noexcept;
    bool observe(std::span<const std::uint8_t> bytes) noexcept;
    std::optional<std::size_t> target(std::uint8_t channel) const noexcept;
    std::optional<std::size_t> soleTarget(std::uint32_t mask) const noexcept;
    const Slot& slot(std::size_t index) const noexcept { return slots[index]; }

private:
    std::array<Slot, slotCount> slots {};
    std::uint64_t revision {};
};

// One voice-free XG instance per insertion, processed before the main
// effects hook. Its variation DSP is private, not the song's variation slot.
class XgExternalInsertion {
public:
    XgExternalInsertion(vst2::EntryPoint entry, vst2::HostCallback host,
                        float rate);
    ~XgExternalInsertion();
    XgExternalInsertion(const XgExternalInsertion&) = delete;
    XgExternalInsertion& operator=(const XgExternalInsertion&) = delete;
    void setSampleRate(float rate);
    void reset() noexcept;
    bool render(const MuExternalInsertions::Slot& slot,
                std::span<float* const> input, std::span<float* const> mix,
                std::size_t frames, float nativeGain);

private:
    static constexpr std::size_t quantum = 128;
    static void inject(void* context, float* buses,
                       std::uint32_t frames) noexcept;
    void configure(const MuExternalInsertions::Slot& slot);
    void message(std::uint8_t address, std::uint8_t value);
    void type(std::uint8_t msb, std::uint8_t lsb);
    void queue(std::span<const std::uint8_t> bytes);
    vst2::AEffect* effect {};
    std::array<std::array<std::uint8_t, 10>, 32> data {};
    std::array<vst2::SysexEvent, 32> sysex {};
    struct Events {
        std::int32_t numEvents {};
        vst2::IntPtr reserved {};
        std::array<vst2::Event*, 32> events {};
    } events;
    std::array<std::array<float, quantum>, 2> output {};
    std::array<float, 8> weights {};
    std::span<float* const> input;
    std::size_t position {};
    float gain {1.0f};
    std::uint64_t configuredRevision {};
    MuExternalInsertions::Slot configuredSlot {};
    bool configurationValid {};
};

} // namespace hybrid
