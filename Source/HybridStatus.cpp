#include "HybridStatus.h"

#include <algorithm>

namespace hybrid {
namespace {

constexpr std::uint32_t vlAvailableFlag = 1u << 0;
constexpr std::uint32_t sgAvailableFlag = 1u << 1;
constexpr std::uint32_t sgActiveFlag = 1u << 2;
constexpr std::uint32_t sgFailedFlag = 1u << 3;
constexpr std::uint32_t effectsBridgeFlag = 1u << 4;
constexpr std::uint32_t explicitVlAssignmentsFlag = 1u << 5;
constexpr std::uint32_t supplementalEngineFlag = 1u << 6;
constexpr std::uint8_t unassignedChannel = 0xff;
std::atomic_flag sharedStatusWriter = ATOMIC_FLAG_INIT;

constexpr std::uint64_t byte(std::uint8_t value, unsigned shift) noexcept
{
    return static_cast<std::uint64_t>(value) << shift;
}

void setFlag(std::atomic<std::uint32_t>& flags, std::uint32_t flag,
             bool enabled) noexcept
{
    if (enabled)
        flags.fetch_or(flag, std::memory_order_release);
    else
        flags.fetch_and(~flag, std::memory_order_release);
}

} // namespace

HybridStatus::HybridStatus() noexcept : HybridStatus(false)
{
}

HybridStatus::HybridStatus(bool isSharedInstance) noexcept
    : sharedInstance(isSharedInstance)
{
    for (auto& channel : channels) {
        channel.value.volume = 100;
        channel.value.pan = 64;
        channel.value.expression = 127;
    }
    for (auto& channel : publishedChannels) {
        channel.primary.store(0, std::memory_order_relaxed);
        channel.controllers.store(byte(100, 16) | byte(64, 24)
                                      | byte(127, 32),
                                  std::memory_order_relaxed);
        channel.activity.store(0, std::memory_order_relaxed);
    }
    for (auto& channel : vlWorkerChannels)
        channel.store(unassignedChannel, std::memory_order_relaxed);
}

HybridStatus& HybridStatus::sharedStatus() noexcept
{
    static HybridStatus shared(true);
    return shared;
}

DisplayEngine HybridStatus::routeEngine(std::uint8_t bankMsb,
                                        bool vlChannel,
                                        bool sgChannel) noexcept
{
    if (sgChannel)
        return DisplayEngine::sg;
    if (vlChannel || bankMsb == 33 || bankMsb == 81 || bankMsb == 97)
        return DisplayEngine::vl;
    return DisplayEngine::xg;
}

void HybridStatus::observeShortMessage(std::uint32_t packedMessage,
                                       bool vlChannel,
                                       bool sgChannel) noexcept
{
    const auto status = static_cast<std::uint8_t>(packedMessage & 0xff);
    if (status < 0x80 || status >= 0xf0)
        return;
    const auto channelIndex = static_cast<std::size_t>(status & 0x0f);
    const auto operation = static_cast<std::uint8_t>(status & 0xf0);
    const auto data1 = static_cast<std::uint8_t>((packedMessage >> 8) & 0x7f);
    const auto data2 = static_cast<std::uint8_t>((packedMessage >> 16) & 0x7f);
    auto& channel = channels[channelIndex];

    if (operation == 0xb0) {
        switch (data1) {
        case 0: channel.value.bankMsb = data2; break;
        case 1: channel.value.modulation = data2; break;
        case 2: channel.value.breath = data2; break;
        case 7: channel.value.volume = data2; break;
        case 10: channel.value.pan = data2; break;
        case 11: channel.value.expression = data2; break;
        case 32: channel.value.bankLsb = data2; break;
        case 64: channel.value.sustain = data2 >= 64; break;
        case 91: channel.value.reverb = data2; break;
        case 93: channel.value.chorus = data2; break;
        case 94: channel.value.variation = data2; break;
        case 120:
        case 123:
            channel.heldNotes.fill(0);
            channel.value.activeNotes = 0;
            channel.value.velocity = 0;
            break;
        default: break;
        }
    } else if (operation == 0xc0) {
        channel.value.program = data1;
    } else if (operation == 0xe0) {
        const auto value = static_cast<std::uint16_t>(data1)
            | (static_cast<std::uint16_t>(data2) << 7);
        channel.value.pitchBend = static_cast<std::int16_t>(value) - 8192;
    } else if (operation == 0x90 && data2 != 0) {
        if (channel.heldNotes[data1] != 0xffff)
            ++channel.heldNotes[data1];
        if (channel.value.activeNotes != 0xffff)
            ++channel.value.activeNotes;
        channel.value.lastNote = data1;
        channel.value.velocity = data2;
    } else if (operation == 0x80 || (operation == 0x90 && data2 == 0)) {
        if (channel.heldNotes[data1] != 0) {
            --channel.heldNotes[data1];
            if (channel.value.activeNotes != 0)
                --channel.value.activeNotes;
        }
        if (channel.value.activeNotes == 0)
            channel.value.velocity = 0;
    }

    channel.value.engine = routeEngine(channel.value.bankMsb, vlChannel,
                                       sgChannel);
    publishChannel(channelIndex);
}

void HybridStatus::reset(ResetDisplayState resetState) noexcept
{
    for (std::size_t index = 0; index < channels.size(); ++index) {
        channels[index] = {};
        channels[index].value.volume = 100;
        channels[index].value.pan = 64;
        channels[index].value.expression = 127;
        publishChannel(index);
    }
    explicitVlAssignments = false;
    runtimeFlags.fetch_and(~explicitVlAssignmentsFlag,
                           std::memory_order_release);
    sgRouteMask.store(0, std::memory_order_release);
    lastReset.store(static_cast<std::uint8_t>(resetState),
                    std::memory_order_release);
    resetVlWorkers();
}

void HybridStatus::setAvailability(bool vlAvailable,
                                   bool sgAvailable) noexcept
{
    setFlag(runtimeFlags, vlAvailableFlag, vlAvailable);
    setFlag(runtimeFlags, sgAvailableFlag, sgAvailable);
}

void HybridStatus::setEffectsBridgeAvailable(bool available) noexcept
{
    setFlag(runtimeFlags, effectsBridgeFlag, available);
}

void HybridStatus::setSupplementalEngineAvailable(bool available) noexcept
{
    setFlag(runtimeFlags, supplementalEngineFlag, available);
}

void HybridStatus::setSampleRate(std::uint32_t value) noexcept
{
    sampleRate.store(std::max<std::uint32_t>(1, value),
                     std::memory_order_release);
}

void HybridStatus::setSgState(bool active, bool failed) noexcept
{
    setFlag(runtimeFlags, sgActiveFlag, active);
    setFlag(runtimeFlags, sgFailedFlag, failed);
}

void HybridStatus::setSgRouteMask(std::uint32_t value) noexcept
{
    sgRouteMask.store(value, std::memory_order_release);
    refreshEngineRoutes();
}

void HybridStatus::configureVlWorker(std::size_t worker,
                                     std::uint8_t channel) noexcept
{
    if (worker >= vlWorkerChannels.size() || channel >= channels.size())
        return;
    vlWorkerChannels[worker].store(channel, std::memory_order_release);
    explicitVlAssignments = true;
    runtimeFlags.fetch_or(explicitVlAssignmentsFlag,
                          std::memory_order_release);
}

void HybridStatus::setVlWorkerState(std::size_t worker,
                                    WorkerDisplayState state) noexcept
{
    if (worker < vlWorkers.size()) {
        vlWorkers[worker].store(static_cast<std::uint8_t>(state),
                                std::memory_order_release);
    }
}

void HybridStatus::resetVlWorkers() noexcept
{
    for (auto& worker : vlWorkers)
        worker.store(static_cast<std::uint8_t>(WorkerDisplayState::idle),
                     std::memory_order_release);
    for (auto& channel : vlWorkerChannels)
        channel.store(unassignedChannel, std::memory_order_release);
}

HybridStatusSnapshot HybridStatus::snapshot() const noexcept
{
    HybridStatusSnapshot result;
    std::uint64_t versionBefore {};
    std::uint64_t versionAfter {};
    do {
        versionBefore = publishVersion.load(std::memory_order_acquire);
        if ((versionBefore & 1u) != 0)
            continue;
        for (std::size_t index = 0; index < result.channels.size(); ++index) {
            const auto primary = publishedChannels[index].primary.load(
                std::memory_order_acquire);
            const auto controllers = publishedChannels[index].controllers.load(
                std::memory_order_acquire);
            const auto activity = publishedChannels[index].activity.load(
                std::memory_order_acquire);
            auto& channel = result.channels[index];
            channel.engine = static_cast<DisplayEngine>(primary & 0xff);
            channel.bankMsb = static_cast<std::uint8_t>((primary >> 8) & 0xff);
            channel.bankLsb = static_cast<std::uint8_t>((primary >> 16) & 0xff);
            channel.program = static_cast<std::uint8_t>((primary >> 24) & 0xff);
            channel.modulation = static_cast<std::uint8_t>(controllers & 0xff);
            channel.breath = static_cast<std::uint8_t>((controllers >> 8) & 0xff);
            channel.volume = static_cast<std::uint8_t>((controllers >> 16) & 0xff);
            channel.pan = static_cast<std::uint8_t>((controllers >> 24) & 0xff);
            channel.expression = static_cast<std::uint8_t>((controllers >> 32) & 0xff);
            channel.reverb = static_cast<std::uint8_t>((controllers >> 40) & 0xff);
            channel.chorus = static_cast<std::uint8_t>((controllers >> 48) & 0xff);
            channel.variation = static_cast<std::uint8_t>((controllers >> 56) & 0xff);
            channel.lastNote = static_cast<std::uint8_t>(activity & 0xff);
            channel.velocity = static_cast<std::uint8_t>((activity >> 8) & 0xff);
            channel.pitchBend = static_cast<std::int16_t>(
                static_cast<std::uint16_t>((activity >> 16) & 0xffff));
            channel.activeNotes = static_cast<std::uint16_t>(
                (activity >> 32) & 0xffff);
            channel.sustain = ((activity >> 48) & 1) != 0;
        }

        const auto flags = runtimeFlags.load(std::memory_order_acquire);
        result.vlAvailable = (flags & vlAvailableFlag) != 0;
        result.sgAvailable = (flags & sgAvailableFlag) != 0;
        result.sgActive = (flags & sgActiveFlag) != 0;
        result.sgFailed = (flags & sgFailedFlag) != 0;
        result.effectsBridgeAvailable = (flags & effectsBridgeFlag) != 0;
        result.supplementalEngineAvailable =
            (flags & supplementalEngineFlag) != 0;
        result.explicitVlAssignments =
            (flags & explicitVlAssignmentsFlag) != 0;
        result.sgRouteMask = sgRouteMask.load(std::memory_order_acquire);
        result.sampleRate = sampleRate.load(std::memory_order_acquire);
        result.lastReset = static_cast<ResetDisplayState>(
            lastReset.load(std::memory_order_acquire));
        for (std::size_t index = 0; index < result.vlWorkers.size(); ++index) {
            result.vlWorkers[index] = static_cast<WorkerDisplayState>(
                vlWorkers[index].load(std::memory_order_acquire));
            result.vlWorkerChannels[index] = vlWorkerChannels[index].load(
                std::memory_order_acquire);
        }
        versionAfter = publishVersion.load(std::memory_order_acquire);
    } while (versionBefore != versionAfter || (versionAfter & 1u) != 0);
    return result;
}

HybridStatusSnapshot HybridStatus::displaySnapshot() const noexcept
{
    if (midiActivity.load(std::memory_order_acquire) || sharedInstance)
        return snapshot();
    const auto& shared = sharedStatus();
    if (shared.midiActivity.load(std::memory_order_acquire)) {
        auto result = shared.snapshot();
        result.latestPlaybackInstance = true;
        return result;
    }
    return snapshot();
}

void HybridStatus::markMidiActivity() noexcept
{
    midiActivity.store(true, std::memory_order_release);
}

void HybridStatus::publishShared() const noexcept
{
    if (sharedInstance || !midiActivity.load(std::memory_order_acquire))
        return;
    if (sharedStatusWriter.test_and_set(std::memory_order_acquire))
        return;
    sharedStatus().applySnapshot(snapshot());
    sharedStatusWriter.clear(std::memory_order_release);
}

void HybridStatus::applySnapshot(
    const HybridStatusSnapshot& value) noexcept
{
    publishVersion.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < value.channels.size(); ++index) {
        const auto& channel = value.channels[index];
        const auto primary = byte(static_cast<std::uint8_t>(channel.engine), 0)
            | byte(channel.bankMsb, 8) | byte(channel.bankLsb, 16)
            | byte(channel.program, 24);
        const auto controllers = byte(channel.modulation, 0)
            | byte(channel.breath, 8) | byte(channel.volume, 16)
            | byte(channel.pan, 24) | byte(channel.expression, 32)
            | byte(channel.reverb, 40) | byte(channel.chorus, 48)
            | byte(channel.variation, 56);
        const auto activity = byte(channel.lastNote, 0)
            | byte(channel.velocity, 8)
            | (static_cast<std::uint64_t>(
                   static_cast<std::uint16_t>(channel.pitchBend)) << 16)
            | (static_cast<std::uint64_t>(channel.activeNotes) << 32)
            | (static_cast<std::uint64_t>(channel.sustain ? 1 : 0) << 48);
        publishedChannels[index].primary.store(primary,
                                               std::memory_order_relaxed);
        publishedChannels[index].controllers.store(
            controllers, std::memory_order_relaxed);
        publishedChannels[index].activity.store(activity,
                                                std::memory_order_relaxed);
    }

    std::uint32_t flags {};
    flags |= value.vlAvailable ? vlAvailableFlag : 0;
    flags |= value.sgAvailable ? sgAvailableFlag : 0;
    flags |= value.sgActive ? sgActiveFlag : 0;
    flags |= value.sgFailed ? sgFailedFlag : 0;
    flags |= value.effectsBridgeAvailable ? effectsBridgeFlag : 0;
    flags |= value.explicitVlAssignments ? explicitVlAssignmentsFlag : 0;
    flags |= value.supplementalEngineAvailable ? supplementalEngineFlag : 0;
    runtimeFlags.store(flags, std::memory_order_relaxed);
    sgRouteMask.store(value.sgRouteMask, std::memory_order_relaxed);
    sampleRate.store(value.sampleRate, std::memory_order_relaxed);
    lastReset.store(static_cast<std::uint8_t>(value.lastReset),
                    std::memory_order_relaxed);
    for (std::size_t index = 0; index < value.vlWorkers.size(); ++index) {
        vlWorkers[index].store(static_cast<std::uint8_t>(
            value.vlWorkers[index]), std::memory_order_relaxed);
        vlWorkerChannels[index].store(value.vlWorkerChannels[index],
                                      std::memory_order_relaxed);
    }
    midiActivity.store(true, std::memory_order_relaxed);
    publishVersion.fetch_add(1, std::memory_order_release);
}

void HybridStatus::publishChannel(std::size_t index) noexcept
{
    const auto& value = channels[index].value;
    const auto primary = byte(static_cast<std::uint8_t>(value.engine), 0)
        | byte(value.bankMsb, 8) | byte(value.bankLsb, 16)
        | byte(value.program, 24);
    const auto controllers = byte(value.modulation, 0) | byte(value.breath, 8)
        | byte(value.volume, 16) | byte(value.pan, 24)
        | byte(value.expression, 32) | byte(value.reverb, 40)
        | byte(value.chorus, 48) | byte(value.variation, 56);
    const auto activity = byte(value.lastNote, 0) | byte(value.velocity, 8)
        | (static_cast<std::uint64_t>(
               static_cast<std::uint16_t>(value.pitchBend))
           << 16)
        | (static_cast<std::uint64_t>(value.activeNotes) << 32)
        | (static_cast<std::uint64_t>(value.sustain ? 1 : 0) << 48);
    publishedChannels[index].primary.store(primary, std::memory_order_release);
    publishedChannels[index].controllers.store(controllers,
                                               std::memory_order_release);
    publishedChannels[index].activity.store(activity,
                                            std::memory_order_release);
}

void HybridStatus::refreshEngineRoutes() noexcept
{
    const auto mask = sgRouteMask.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const bool sgChannel = (mask & (std::uint32_t {1} << index)) != 0;
        const auto bank = channels[index].value.bankMsb;
        channels[index].value.engine = routeEngine(
            bank, bank == 33 || bank == 81 || bank == 97, sgChannel);
        publishChannel(index);
    }
}

} // namespace hybrid
