#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hybrid {

enum class DisplayEngine : std::uint8_t {
    xg,
    vl,
    sg,
};

enum class WorkerDisplayState : std::uint8_t {
    idle,
    loaded,
    active,
    failed,
};

enum class ResetDisplayState : std::uint8_t {
    none,
    gm1,
    gm2,
    gs,
    xg,
};

struct ChannelStatusSnapshot {
    DisplayEngine engine {DisplayEngine::xg};
    std::uint8_t bankMsb {};
    std::uint8_t bankLsb {};
    std::uint8_t program {};
    std::uint8_t lastNote {};
    std::uint8_t velocity {};
    std::uint8_t modulation {};
    std::uint8_t breath {};
    std::uint8_t volume {100};
    std::uint8_t pan {64};
    std::uint8_t expression {127};
    std::uint8_t reverb {};
    std::uint8_t chorus {};
    std::uint8_t variation {};
    std::int16_t pitchBend {};
    std::uint16_t activeNotes {};
    bool sustain {};
};

struct HybridStatusSnapshot {
    std::array<ChannelStatusSnapshot, 16> channels {};
    std::array<WorkerDisplayState, 8> vlWorkers {};
    std::array<std::uint8_t, 8> vlWorkerChannels {};
    bool vlAvailable {};
    bool sgAvailable {};
    bool sgActive {};
    bool sgFailed {};
    bool effectsBridgeAvailable {};
    bool supplementalEngineAvailable {};
    bool explicitVlAssignments {};
    bool latestPlaybackInstance {};
    std::uint32_t sgRouteMask {};
    std::uint32_t sampleRate {44'100};
    ResetDisplayState lastReset {ResetDisplayState::none};
};

class HybridStatus {
public:
    HybridStatus() noexcept;

    void observeShortMessage(std::uint32_t packedMessage, bool vlChannel,
                             bool sgChannel) noexcept;
    void reset(ResetDisplayState resetState) noexcept;

    void setAvailability(bool vlAvailable, bool sgAvailable) noexcept;
    void setEffectsBridgeAvailable(bool available) noexcept;
    void setSupplementalEngineAvailable(bool available) noexcept;
    void setSampleRate(std::uint32_t sampleRate) noexcept;
    void setSgState(bool active, bool failed) noexcept;
    void setSgRouteMask(std::uint32_t routeMask) noexcept;
    void configureVlWorker(std::size_t worker, std::uint8_t channel) noexcept;
    void setVlWorkerState(std::size_t worker,
                          WorkerDisplayState state) noexcept;
    void resetVlWorkers() noexcept;
    void markMidiActivity() noexcept;
    void publishShared() const noexcept;

    [[nodiscard]] HybridStatusSnapshot snapshot() const noexcept;
    [[nodiscard]] HybridStatusSnapshot displaySnapshot() const noexcept;

private:
    explicit HybridStatus(bool sharedInstance) noexcept;
    struct ChannelState {
        ChannelStatusSnapshot value {};
        std::array<std::uint16_t, 128> heldNotes {};
    };

    struct PublishedChannel {
        std::atomic<std::uint64_t> primary {};
        std::atomic<std::uint64_t> controllers {};
        std::atomic<std::uint64_t> activity {};
    };

    static DisplayEngine routeEngine(std::uint8_t bankMsb, bool vlChannel,
                                     bool sgChannel) noexcept;
    static HybridStatus& sharedStatus() noexcept;
    void applySnapshot(const HybridStatusSnapshot& snapshot) noexcept;
    void publishChannel(std::size_t channel) noexcept;
    void refreshEngineRoutes() noexcept;

    std::array<ChannelState, 16> channels {};
    std::array<PublishedChannel, 16> publishedChannels {};
    std::array<std::atomic<std::uint8_t>, 8> vlWorkers {};
    std::array<std::atomic<std::uint8_t>, 8> vlWorkerChannels {};
    std::atomic<std::uint32_t> runtimeFlags {};
    std::atomic<std::uint32_t> sgRouteMask {};
    std::atomic<std::uint32_t> sampleRate {44'100};
    std::atomic<std::uint8_t> lastReset {};
    std::atomic<bool> midiActivity {};
    std::atomic<std::uint64_t> publishVersion {};
    bool explicitVlAssignments {};
    bool sharedInstance {};
};

} // namespace hybrid
