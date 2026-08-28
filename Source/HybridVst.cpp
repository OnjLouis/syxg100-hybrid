#include "MidiRouter.h"
#include "MidiChannelSnapshot.h"
#include "NativeEventTimeline.h"
#include "NativeSgClient.h"
#include "NativeVlClient.h"
#include "OrderedSetupHistory.h"
#include "SgRouting.h"
#include "StreamingRateAdapter.h"
#include "VlPartRouter.h"
#include "VlVoiceAllocator.h"
#include "Vst2Abi.h"
#include "XgEffectsBridge.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t maxPendingVlEvents = 512;
constexpr std::size_t maxPendingSgEvents = 512;
constexpr std::size_t maxVlSetupEvents = 1024;
constexpr std::size_t maxVlSetupSysexBytes = 65'536;
constexpr std::size_t midiChannelCount = 16;
constexpr std::size_t maxVlVoices = 8;
// The legacy XG engine replaces, rather than appends, its pending VST event
// list. Keep dense setup bursts intact in one dispatcher call.
constexpr std::size_t childEventsPerBatch = 4096;
constexpr std::size_t maxChildEventBatches = 8;
constexpr float int16Scale = 1.0f / 32768.0f;
constexpr float defaultNativeOutputGain = 3.5f;
constexpr float xgInternalBusScale = 32768.0f;
constexpr std::uint32_t nativeSampleRate = 44'100;
constexpr std::size_t vlOutputBusCount = hybrid::ipc::planeCount * 2;
constexpr std::size_t xgCachedFramesOffset = 0x100;
constexpr std::int32_t hybridUniqueId = 0x53314859; // "S1HY"
constexpr std::int32_t hybridVendorVersion = 100;
constexpr char hybridEffectName[] = "S-YXG100 Hybrid";
constexpr char hybridVendorName[] = "Onj Research";

vst2::IntPtr writeVstString(void* destination, const char* text,
                            std::size_t capacity)
{
    if (destination == nullptr || capacity == 0)
        return 0;
    auto* output = static_cast<char*>(destination);
    std::strncpy(output, text, capacity - 1);
    output[capacity - 1] = '\0';
    return 1;
}

enum VlOutputBus : std::size_t {
    dryLeft,
    dryRight,
    reverbLeft,
    reverbRight,
    chorusLeft,
    chorusRight,
    variationLeft,
    variationRight,
};

enum class PendingSgKind : std::uint8_t {
    shortMessage,
    sysex,
};

struct PendingSgEvent {
    PendingSgKind kind {};
    std::uint64_t frame {};
    std::int32_t deltaFrames {};
    std::uint32_t value {};
    std::uint32_t dataOffset {};
    std::uint32_t dataSize {};
};

enum class VlSetupKind : std::uint8_t {
    shortMessage,
    sysex,
};

struct VlSetupEvent {
    VlSetupKind kind {};
    std::uint8_t channel {};
    std::uint32_t value {};
    std::uint32_t dataOffset {};
    std::uint32_t dataSize {};
};

struct SgSetupEvent {
    VlSetupKind kind {};
    std::uint32_t value {};
    std::uint32_t dataOffset {};
    std::uint32_t dataSize {};
    std::uint64_t absoluteFrame {};
};

struct VlVoiceState {
    std::unique_ptr<hybrid::NativeVlClient> client;
    std::array<hybrid::TimedNativeMidi, maxPendingVlEvents> pending {};
    std::size_t pendingCount {};
    std::uint64_t timelineFrame {};
    bool prepared {};
    bool started {};
    bool disabled {};
};

struct SgState {
    std::unique_ptr<hybrid::NativeSgClient> client;
    std::array<PendingSgEvent, maxPendingSgEvents> pending {};
    std::size_t pendingCount {};
    std::array<std::uint8_t, maxVlSetupSysexBytes> pendingSysex {};
    std::size_t pendingSysexSize {};
    std::uint32_t routeMask {};
    std::uint64_t timelineFrame {};
    bool started {};
    bool disabled {};
};

struct ChildEventBatch {
    std::int32_t numEvents {};
    vst2::IntPtr reserved {};
    std::array<vst2::Event*, childEventsPerBatch> events {};
};

struct WrapperState {
    HMODULE module {};
    vst2::AEffect* child {};
    std::filesystem::path vxdPath;
    std::filesystem::path workerPath;
    std::filesystem::path sgVxdPath;
    std::filesystem::path sgWorkerPath;
    std::array<VlVoiceState, maxVlVoices> vlVoices;
    std::array<hybrid::MidiChannelSnapshot, midiChannelCount>
        vlChannelSnapshots;
    hybrid::VlVoiceAllocator<maxVlVoices> vlVoiceAllocator;
    SgState sg;
    hybrid::MidiRouter router;
    std::array<VlSetupEvent, maxVlSetupEvents> vlSetupEvents {};
    std::size_t vlSetupEventCount {};
    std::array<std::uint8_t, maxVlSetupSysexBytes> vlSetupSysex {};
    std::size_t vlSetupSysexSize {};
    std::array<std::uint8_t, maxVlSetupSysexBytes> vlSysexScratch {};
    std::array<SgSetupEvent, maxVlSetupEvents> sgSetupEvents {};
    std::size_t sgSetupEventCount {};
    std::array<std::uint8_t, maxVlSetupSysexBytes> sgSetupSysex {};
    std::size_t sgSetupSysexSize {};
    std::uint64_t sgTimelineFrames {};
    std::uint64_t sgSetupBaseFrame {};
    std::array<ChildEventBatch, maxChildEventBatches> childBatches {};
    std::size_t childBatchCount {};
    std::array<hybrid::ipc::TimedMidiEvent, maxPendingVlEvents>
        vlTimedMidiScratch {};
    std::vector<float> vlOutputBuses;
    std::size_t vlOutputCapacityFrames {};
    std::vector<float> nativeOutputBuses;
    std::size_t nativeOutputCapacityFrames {};
    hybrid::StreamingRateAdapter nativeRateAdapter;
    std::size_t vlEffectsCursor {};
    float sampleRate {};
    float nativeOutputGain {defaultNativeOutputGain};
    bool vlAvailable {};
    bool sgAvailable {};
    bool vlSetupHistoryFrozen {};
    bool sgSetupHistoryFrozen {};
    bool xgEffectsBridgeAvailable {};
    bool vlRenderDiagnosticWritten {};
    bool vlHookDiagnosticWritten {};
    bool xgBusDiagnosticWritten {};
};

std::uint64_t nativeFrame(const WrapperState& wrapper,
                          std::uint64_t hostFrame) noexcept
{
    const auto hostRate = static_cast<std::uint32_t>(
        std::max(1.0f, std::round(wrapper.sampleRate)));
    return hybrid::hostFrameToNative(hostFrame, nativeSampleRate, hostRate);
}

void configureAudioBuffers(WrapperState& wrapper,
                           std::size_t requestedFrames)
{
    const auto quantum = static_cast<std::size_t>(
        hybrid::XgEffectsBridge::quantumFrames);
    wrapper.vlOutputCapacityFrames = (requestedFrames + quantum - 1)
        / quantum * quantum;
    wrapper.vlOutputBuses.assign(
        wrapper.vlOutputCapacityFrames * vlOutputBusCount, 0.0f);

    const auto hostRate = static_cast<std::uint32_t>(
        std::max(1.0f, std::round(wrapper.sampleRate)));
    wrapper.nativeRateAdapter.configure(
        nativeSampleRate, hostRate, vlOutputBusCount,
        wrapper.vlOutputCapacityFrames);
    wrapper.nativeOutputCapacityFrames = static_cast<std::size_t>(std::ceil(
        wrapper.vlOutputCapacityFrames
        * (static_cast<double>(nativeSampleRate) / hostRate))) + 4;
    wrapper.nativeOutputBuses.assign(
        wrapper.nativeOutputCapacityFrames * vlOutputBusCount, 0.0f);
}

WrapperState* state(vst2::AEffect* effect)
{
    return static_cast<WrapperState*>(effect->object);
}

float readNativeOutputGain() noexcept
{
    wchar_t text[32] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_NATIVE_GAIN", text, static_cast<DWORD>(std::size(text)));
    if (length == 0 || length >= std::size(text))
        return defaultNativeOutputGain;
    wchar_t* end {};
    const auto value = std::wcstof(text, &end);
    if (end == text || *end != L'\0' || !std::isfinite(value)
        || value < 0.1f || value > 8.0f) {
        return defaultNativeOutputGain;
    }
    return value;
}

void reportVlFailure(const char* context, const char* details = nullptr)
{
    std::string message = "S-YXG100 Hybrid: VL engine disabled after ";
    message += context;
    if (details != nullptr && details[0] != '\0') {
        message += ": ";
        message += details;
    }
    message += '\n';
    OutputDebugStringA(message.c_str());

    wchar_t logPath[MAX_PATH] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message.data(), static_cast<DWORD>(message.size()),
              &written, nullptr);
    CloseHandle(file);
}

void reportSgFailure(const char* context, const char* details = nullptr)
{
    std::string message = "S-YXG100 Hybrid: SG engine disabled after ";
    message += context;
    if (details != nullptr && details[0] != '\0') {
        message += ": ";
        message += details;
    }
    message += '\n';
    OutputDebugStringA(message.c_str());

    wchar_t logPath[MAX_PATH] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message.data(), static_cast<DWORD>(message.size()),
              &written, nullptr);
    CloseHandle(file);
}

void reportSgDiagnostic(const char* stage, bool available,
                        std::uint32_t routeMask)
{
    char message[160] {};
    const auto length = std::snprintf(
        message, sizeof(message),
        "S-YXG100 Hybrid SG: %s available=%u route-mask=0x%04lx\n",
        stage, available ? 1u : 0u, static_cast<unsigned long>(routeMask));
    if (length <= 0)
        return;
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written {};
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

void reportBridgeDiagnostic(const char* stage, std::int32_t frames,
                            std::int32_t cachedPrefix, float peak)
{
    char message[192] {};
    const auto length = std::snprintf(
        message, sizeof(message),
        "S-YXG100 Hybrid bridge: %s frames=%ld cached=%ld peak=%.8g\n",
        stage, static_cast<long>(frames), static_cast<long>(cachedPrefix), peak);
    if (length <= 0)
        return;
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written {};
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

void reportBusDiagnostic(const WrapperState& wrapper, std::int32_t frames)
{
    std::array<float, vlOutputBusCount> peaks {};
    for (std::size_t bus = 0; bus < peaks.size(); ++bus) {
        const auto* samples = wrapper.vlOutputBuses.data()
            + bus * wrapper.vlOutputCapacityFrames;
        for (std::int32_t frame = 0; frame < frames; ++frame)
            peaks[bus] = std::max(peaks[bus], std::abs(samples[frame]));
    }
    char message[320] {};
    const auto length = std::snprintf(
        message, sizeof(message),
        "S-YXG100 Hybrid native buses: dry=%.8g/%.8g reverb=%.8g/%.8g "
        "chorus=%.8g/%.8g variation=%.8g/%.8g\n",
        peaks[dryLeft], peaks[dryRight], peaks[reverbLeft],
        peaks[reverbRight], peaks[chorusLeft], peaks[chorusRight],
        peaks[variationLeft], peaks[variationRight]);
    if (length <= 0)
        return;
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written {};
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

void reportXgBusDiagnostic(const float* buses, std::uint32_t frames)
{
    std::array<float, hybrid::XgEffectsBridge::busCount> peaks {};
    for (std::size_t bus = 0; bus < peaks.size(); ++bus) {
        const auto* samples = buses
            + bus * hybrid::XgEffectsBridge::busStrideFrames;
        for (std::uint32_t frame = 0; frame < frames; ++frame)
            peaks[bus] = std::max(peaks[bus], std::abs(samples[frame]));
    }
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    char message[512] {};
    auto position = std::snprintf(message, sizeof(message),
                                  "S-YXG100 Hybrid XG buses:");
    for (std::size_t bus = 0; bus < peaks.size()
         && position > 0
         && static_cast<std::size_t>(position) < sizeof(message); ++bus) {
        position += std::snprintf(
            message + position, sizeof(message) - position,
            " %zu=%.8g", bus, static_cast<double>(peaks[bus]));
    }
    if (position > 0 && static_cast<std::size_t>(position) < sizeof(message)) {
        message[position++] = '\n';
        DWORD written {};
        WriteFile(file, message, static_cast<DWORD>(position), &written,
                  nullptr);
    }
    CloseHandle(file);
}

void clearVlVoice(VlVoiceState& voice)
{
    voice.pendingCount = 0;
    voice.timelineFrame = 0;
    voice.prepared = false;
    voice.started = false;
    voice.disabled = false;
    voice.client.reset();
}

void disableVlVoice(WrapperState& wrapper, std::uint8_t voice,
                    const char* context, const char* details = nullptr)
{
    reportVlFailure(context, details);
    clearVlVoice(wrapper.vlVoices[voice]);
    wrapper.vlVoices[voice].disabled = true;
    wrapper.vlVoiceAllocator.release(voice);
}

void resetVlVoices(WrapperState& wrapper)
{
    for (auto& voice : wrapper.vlVoices)
        clearVlVoice(voice);
    for (auto& snapshot : wrapper.vlChannelSnapshots)
        snapshot.reset();
    wrapper.vlVoiceAllocator.reset();
}

void clearSg(WrapperState& wrapper)
{
    wrapper.sg.pendingCount = 0;
    wrapper.sg.pendingSysexSize = 0;
    wrapper.sg.routeMask = 0;
    wrapper.sg.started = false;
    wrapper.sg.disabled = false;
    wrapper.sg.client.reset();
}

void disableSg(WrapperState& wrapper, const char* context,
               const char* details = nullptr)
{
    reportSgFailure(context, details);
    clearSg(wrapper);
    wrapper.sg.disabled = true;
}

void resetVlPlaybackState(WrapperState& wrapper)
{
    wrapper.nativeRateAdapter.reset();
    for (auto& voice : wrapper.vlVoices) {
        voice.pendingCount = 0;
        voice.timelineFrame = nativeFrame(wrapper, wrapper.sgTimelineFrames);
        voice.prepared = false;
        voice.started = false;
    }
    for (auto& snapshot : wrapper.vlChannelSnapshots)
        snapshot.reset();
    wrapper.vlVoiceAllocator.reset();
    wrapper.vlSetupHistoryFrozen = false;
    wrapper.sg.pendingCount = 0;
    wrapper.sg.pendingSysexSize = 0;
    wrapper.sg.routeMask = 0;
    wrapper.sg.timelineFrame = nativeFrame(wrapper, wrapper.sgTimelineFrames);
    if (wrapper.sg.client == nullptr)
        wrapper.sgSetupHistoryFrozen = false;
}

hybrid::NativeVlClient* ensureVlVoice(WrapperState& wrapper,
                                      std::uint8_t voice)
{
    if (!wrapper.vlAvailable || voice >= wrapper.vlVoices.size())
        return nullptr;
    auto& state = wrapper.vlVoices[voice];
    if (state.disabled)
        return nullptr;
    if (state.client != nullptr)
        return state.client.get();
    try {
        state.client = std::make_unique<hybrid::NativeVlClient>(
            wrapper.workerPath, wrapper.vxdPath, nativeSampleRate);
        return state.client.get();
    } catch (const std::exception& error) {
        disableVlVoice(wrapper, voice, "initialization failure", error.what());
    } catch (...) {
        disableVlVoice(wrapper, voice, "initialization failure");
    }
    return nullptr;
}

hybrid::NativeSgClient* ensureSg(WrapperState& wrapper)
{
    if (!wrapper.sgAvailable || wrapper.sg.disabled)
        return nullptr;
    if (wrapper.sg.client != nullptr)
        return wrapper.sg.client.get();
    try {
        wrapper.sg.client = std::make_unique<hybrid::NativeSgClient>(
            wrapper.sgWorkerPath, wrapper.sgVxdPath, nativeSampleRate);
        wrapper.sg.started = true;
        return wrapper.sg.client.get();
    } catch (const std::exception& error) {
        disableSg(wrapper, "initialization failure", error.what());
    } catch (...) {
        disableSg(wrapper, "initialization failure");
    }
    return nullptr;
}

void configureVl(WrapperState& wrapper, float sampleRate)
{
    if (sampleRate <= 0.0f)
        return;
    const auto changed = wrapper.sampleRate != sampleRate;
    wrapper.sampleRate = sampleRate;
    wrapper.vlAvailable = std::filesystem::is_regular_file(wrapper.vxdPath)
        && std::filesystem::is_regular_file(wrapper.workerPath);
    wrapper.sgAvailable = std::filesystem::is_regular_file(wrapper.sgVxdPath)
        && std::filesystem::is_regular_file(wrapper.sgWorkerPath);
    if (!wrapper.vlAvailable)
        resetVlVoices(wrapper);
    if (!wrapper.sgAvailable)
        clearSg(wrapper);
    if (!changed)
        return;
    if (wrapper.vlOutputCapacityFrames != 0) {
        try {
            configureAudioBuffers(wrapper, wrapper.vlOutputCapacityFrames);
        } catch (const std::exception& error) {
            wrapper.vlOutputCapacityFrames = 0;
            wrapper.vlOutputBuses.clear();
            wrapper.nativeOutputCapacityFrames = 0;
            wrapper.nativeOutputBuses.clear();
            reportVlFailure("sample-rate buffer allocation failure",
                            error.what());
        }
    }
    for (auto& voice : wrapper.vlVoices)
        voice.timelineFrame = nativeFrame(wrapper, wrapper.sgTimelineFrames);
    wrapper.sg.timelineFrame = nativeFrame(wrapper, wrapper.sgTimelineFrames);
}

std::uint32_t packedMessage(const vst2::MidiEvent& event)
{
    std::uint32_t packed = 0;
    std::memcpy(&packed, event.midiData, sizeof(event.midiData));
    return packed;
}

bool isNoteOn(std::uint32_t packed)
{
    const auto operation = static_cast<std::uint8_t>(packed & 0xf0);
    const auto velocity = static_cast<std::uint8_t>((packed >> 16) & 0x7f);
    return operation == 0x90 && velocity != 0;
}

bool isNoteOff(std::uint32_t packed)
{
    const auto operation = static_cast<std::uint8_t>(packed & 0xf0);
    const auto velocity = static_cast<std::uint8_t>((packed >> 16) & 0x7f);
    return operation == 0x80 || (operation == 0x90 && velocity == 0);
}

bool clearsHeldNotes(std::uint32_t packed)
{
    const auto operation = static_cast<std::uint8_t>(packed & 0xf0);
    const auto controller = static_cast<std::uint8_t>((packed >> 8) & 0x7f);
    return operation == 0xb0 && (controller == 120 || controller == 123);
}

std::uint8_t midiNote(std::uint32_t packed)
{
    return static_cast<std::uint8_t>((packed >> 8) & 0x7f);
}

std::uint32_t noteRelease(std::uint32_t packed)
{
    return packed & 0x0000ffff;
}

std::uint32_t warmUpNote(std::uint32_t packed)
{
    const auto note = static_cast<std::uint8_t>((packed >> 8) & 0x7f);
    const auto probeNote = static_cast<std::uint8_t>(note == 127 ? 126 : note + 1);
    return (packed & ~std::uint32_t { 0x0000ff00 })
        | (static_cast<std::uint32_t>(probeNote) << 8);
}

void clearVlSetup(WrapperState& wrapper)
{
    wrapper.vlSetupEventCount = 0;
    wrapper.vlSetupSysexSize = 0;
}

void clearSgSetup(WrapperState& wrapper)
{
    wrapper.sgSetupEventCount = 0;
    wrapper.sgSetupSysexSize = 0;
    wrapper.sgSetupBaseFrame = wrapper.sgTimelineFrames;
}

bool retainVlShort(WrapperState& wrapper, std::uint8_t channel,
                   std::uint32_t message)
{
    // RPN and NRPN values depend on the exact selector/data-entry order.
    // Replacing earlier messages by controller number corrupts that sequence.
    return hybrid::retainOrderedSetupEvent(
        wrapper.vlSetupEvents, wrapper.vlSetupEventCount, VlSetupEvent {
        VlSetupKind::shortMessage, channel, message, 0, 0
    });
}

bool retainVlSysex(WrapperState& wrapper,
                   std::span<const std::uint8_t> bytes)
{
    if (wrapper.vlSetupEventCount == wrapper.vlSetupEvents.size()
        || bytes.size() > wrapper.vlSetupSysex.size() - wrapper.vlSetupSysexSize)
        return false;
    const auto offset = wrapper.vlSetupSysexSize;
    std::copy(bytes.begin(), bytes.end(),
              wrapper.vlSetupSysex.begin() + offset);
    wrapper.vlSetupSysexSize += bytes.size();
    wrapper.vlSetupEvents[wrapper.vlSetupEventCount++] = {
        VlSetupKind::sysex, 0, 0, static_cast<std::uint32_t>(offset),
        static_cast<std::uint32_t>(bytes.size())
    };
    return true;
}

bool retainSgShort(WrapperState& wrapper, std::uint32_t message,
                   std::uint64_t absoluteFrame)
{
    if (wrapper.sgSetupEventCount == wrapper.sgSetupEvents.size())
        return false;
    wrapper.sgSetupEvents[wrapper.sgSetupEventCount++] = {
        VlSetupKind::shortMessage, message, 0, 0, absoluteFrame
    };
    return true;
}

bool retainSgSysex(WrapperState& wrapper,
                   std::span<const std::uint8_t> bytes,
                   std::uint64_t absoluteFrame)
{
    if (wrapper.sgSetupEventCount == wrapper.sgSetupEvents.size()
        || bytes.size() > wrapper.sgSetupSysex.size()
            - wrapper.sgSetupSysexSize) {
        return false;
    }
    const auto offset = wrapper.sgSetupSysexSize;
    std::copy(bytes.begin(), bytes.end(),
              wrapper.sgSetupSysex.begin() + offset);
    wrapper.sgSetupSysexSize += bytes.size();
    wrapper.sgSetupEvents[wrapper.sgSetupEventCount++] = {
        VlSetupKind::sysex, 0, static_cast<std::uint32_t>(offset),
        static_cast<std::uint32_t>(bytes.size()), absoluteFrame
    };
    return true;
}

void sendVlSysexForChannel(WrapperState& wrapper,
                           hybrid::NativeVlClient& client,
                           std::span<const std::uint8_t> bytes,
                           std::uint8_t sourceChannel,
                           std::uint8_t nativeChannel)
{
    const auto route = hybrid::routeVlSysex(bytes, sourceChannel);
    if (route == hybrid::VlSysexRoute::passThrough) {
        client.sendSysex(bytes);
        return;
    }
    if (route == hybrid::VlSysexRoute::drop)
        return;
    std::copy(bytes.begin(), bytes.end(), wrapper.vlSysexScratch.begin());
    std::span<std::uint8_t> remapped {
        wrapper.vlSysexScratch.data(), bytes.size()
    };
    hybrid::applyVlSysexRoute(remapped, route, nativeChannel);
    client.sendSysex(remapped);
}

void replayVlSetup(WrapperState& wrapper, std::uint8_t voice,
                   std::uint8_t channel, bool replaySnapshot)
{
    auto& client = wrapper.vlVoices[voice].client;
    if (client == nullptr)
        return;
    const auto nativeChannel = hybrid::nativeVlChannel(
        wrapper.vlVoiceAllocator.hasExplicitConfiguration(), channel);
    for (std::size_t index = 0; index < wrapper.vlSetupEventCount; ++index) {
        const auto& event = wrapper.vlSetupEvents[index];
        if (event.kind == VlSetupKind::shortMessage) {
            if (event.channel == channel) {
                const auto nativeMessage = hybrid::remapVlShortMessage(
                    event.value, nativeChannel);
                client->sendShort(nativeMessage);
            }
            continue;
        }
        const std::span<const std::uint8_t> bytes {
            wrapper.vlSetupSysex.data() + event.dataOffset, event.dataSize
        };
        sendVlSysexForChannel(wrapper, *client, bytes, channel,
                              nativeChannel);
    }
    if (replaySnapshot) {
        wrapper.vlChannelSnapshots[channel].replay(
            [&](std::uint32_t message) {
                const auto nativeMessage = hybrid::remapVlShortMessage(
                    message, nativeChannel);
                client->sendShort(nativeMessage);
            });
    }
}

void replayVlSysexSetup(WrapperState& wrapper, std::uint8_t voice,
                        std::uint8_t channel)
{
    auto& client = wrapper.vlVoices[voice].client;
    if (client == nullptr)
        return;
    const auto nativeChannel = hybrid::nativeVlChannel(
        wrapper.vlVoiceAllocator.hasExplicitConfiguration(), channel);
    for (std::size_t index = 0; index < wrapper.vlSetupEventCount; ++index) {
        const auto& event = wrapper.vlSetupEvents[index];
        if (event.kind != VlSetupKind::sysex)
            continue;
        sendVlSysexForChannel(
            wrapper, *client,
            { wrapper.vlSetupSysex.data() + event.dataOffset, event.dataSize },
            channel, nativeChannel);
    }
}

void renderSgSetupDelay(hybrid::NativeSgClient& client,
                        std::uint64_t frames)
{
    while (frames != 0) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(frames, hybrid::NativeSgClient::maxFrames));
        client.render(count);
        frames -= count;
    }
}

void replaySgSetup(WrapperState& wrapper, std::uint64_t triggerFrame)
{
    if (wrapper.sg.client == nullptr)
        return;
    const auto convertFrame = [&](std::uint64_t frame) {
        return wrapper.nativeRateAdapter.active()
            ? nativeFrame(wrapper, frame) : frame;
    };
    auto position = convertFrame(wrapper.sgSetupBaseFrame);
    for (std::size_t index = 0; index < wrapper.sgSetupEventCount; ++index) {
        const auto& event = wrapper.sgSetupEvents[index];
        const auto eventFrame = std::max(
            position, convertFrame(event.absoluteFrame));
        renderSgSetupDelay(*wrapper.sg.client, eventFrame - position);
        if (event.kind == VlSetupKind::shortMessage) {
            wrapper.sg.client->sendShort(event.value);
        } else {
            wrapper.sg.client->sendSysex({
                wrapper.sgSetupSysex.data() + event.dataOffset, event.dataSize
            });
        }
        position = eventFrame;
    }
    const auto nativeTrigger = convertFrame(triggerFrame);
    renderSgSetupDelay(*wrapper.sg.client,
                       std::max(position, nativeTrigger) - position);
    if (wrapper.nativeRateAdapter.active())
        wrapper.sg.timelineFrame = std::max(position, nativeTrigger);
}

bool isSystemReset(std::span<const std::uint8_t> bytes)
{
    const bool gmReset = bytes.size() >= 6 && bytes[0] == 0xf0
        && bytes[1] == 0x7e && bytes[3] == 0x09 && bytes[4] == 0x01;
    const bool xgReset = bytes.size() >= 9 && bytes[0] == 0xf0
        && bytes[1] == 0x43 && bytes[3] == 0x4c && bytes[4] == 0x00
        && bytes[5] == 0x00 && bytes[6] == 0x7e && bytes[7] == 0x00;
    return gmReset || xgReset;
}

void queueVl(WrapperState& wrapper, VlVoiceState& voice,
             std::int32_t deltaFrames, std::uint32_t message)
{
    const hybrid::TimedNativeMidi event {
        nativeFrame(wrapper, wrapper.sgTimelineFrames
            + static_cast<std::uint64_t>(std::max(0, deltaFrames)))
            + hybrid::nativeMidiLookaheadFrames,
        message,
    };
    if (!hybrid::queueNativeMidi(voice.pending, voice.pendingCount, event))
        throw std::runtime_error("native VL event queue is full");
}

void queueSgShort(WrapperState& wrapper, std::int32_t deltaFrames,
                   std::uint32_t message)
{
    auto& state = wrapper.sg;
    if (state.pendingCount == state.pending.size())
        throw std::runtime_error("native SG event queue is full");
    const PendingSgEvent event {
        PendingSgKind::shortMessage,
        nativeFrame(wrapper, wrapper.sgTimelineFrames
            + static_cast<std::uint64_t>(std::max(0, deltaFrames))),
        deltaFrames, message, 0, 0
    };
    state.pending[state.pendingCount++] = event;
}

void queueSgSysex(WrapperState& wrapper, std::int32_t deltaFrames,
                   std::span<const std::uint8_t> bytes)
{
    auto& state = wrapper.sg;
    if (state.pendingCount == state.pending.size()
        || bytes.size() > state.pendingSysex.size()
            - state.pendingSysexSize) {
        throw std::runtime_error("native SG SysEx queue is full");
    }
    const auto offset = state.pendingSysexSize;
    std::copy(bytes.begin(), bytes.end(),
              state.pendingSysex.begin() + offset);
    state.pendingSysexSize += bytes.size();
    const PendingSgEvent event {
        PendingSgKind::sysex,
        nativeFrame(wrapper, wrapper.sgTimelineFrames
            + static_cast<std::uint64_t>(std::max(0, deltaFrames))),
        deltaFrames, 0,
        static_cast<std::uint32_t>(offset),
        static_cast<std::uint32_t>(bytes.size())
    };
    state.pending[state.pendingCount++] = event;
}

void retainChildEvent(WrapperState& wrapper, vst2::Event* event, bool forceNew)
{
    if (forceNew || wrapper.childBatchCount == 0
        || wrapper.childBatches[wrapper.childBatchCount - 1].numEvents
            == childEventsPerBatch) {
        if (wrapper.childBatchCount == wrapper.childBatches.size())
            return;
        ++wrapper.childBatchCount;
    }
    auto& batch = wrapper.childBatches[wrapper.childBatchCount - 1];
    batch.events[batch.numEvents++] = event;
}

void clearChildEvents(WrapperState& wrapper)
{
    for (std::size_t index = 0; index < wrapper.childBatchCount; ++index)
        wrapper.childBatches[index].numEvents = 0;
    wrapper.childBatchCount = 0;
}

vst2::IntPtr processEvents(WrapperState& wrapper, const vst2::Events* events)
{
    if (events == nullptr)
        return 0;
    if (!wrapper.vlAvailable && !wrapper.sgAvailable) {
        return wrapper.child->dispatcher(wrapper.child, vst2::processEvents, 0,
                                         0, const_cast<vst2::Events*>(events),
                                         0.0f);
    }
    const auto firstNewBatch = wrapper.childBatchCount;
    bool firstChildEvent = true;
    vst2::IntPtr result = 0;
    try {
        for (std::int32_t index = 0; index < events->numEvents; ++index) {
            auto* event = events->events[index];
            bool sendToChild = true;
            if (event != nullptr && event->type == 1) {
                const auto* midi = reinterpret_cast<const vst2::MidiEvent*>(event);
                const auto packed = packedMessage(*midi);
                const auto channel = static_cast<std::uint8_t>(packed & 0x0f);
                const auto eventFrame = wrapper.sgTimelineFrames
                    + static_cast<std::uint64_t>(
                        std::max(0, midi->deltaFrames));
                const auto destination = wrapper.router.routeShortMessage(packed);
                sendToChild = destination != hybrid::MidiDestination::vl;
                if (wrapper.sg.client == nullptr
                    && !wrapper.sgSetupHistoryFrozen
                    && !hybrid::sgOwnsNote(packed, 0xffff)
                    && !retainSgShort(wrapper, packed, eventFrame)) {
                    wrapper.sgSetupHistoryFrozen = true;
                }
                if (destination != hybrid::MidiDestination::xg) {
                    try {
                        wrapper.vlChannelSnapshots[channel].observe(packed);
                        if (isNoteOn(packed)) {
                            const auto allocation = wrapper.vlVoiceAllocator.noteOn(
                                channel, midiNote(packed));
                            if (allocation.voice
                                != decltype(wrapper.vlVoiceAllocator)::noVoice) {
                                const auto voiceIndex = static_cast<std::uint8_t>(
                                    allocation.voice);
                                auto& voice = wrapper.vlVoices[voiceIndex];
                                auto* client = ensureVlVoice(wrapper, voiceIndex);
                                if (client == nullptr) {
                                    sendToChild = true;
                                } else {
                                const auto nativeChannel =
                                    hybrid::nativeVlChannel(
                                        wrapper.vlVoiceAllocator
                                            .hasExplicitConfiguration(),
                                        channel);
                                const auto nativePacked =
                                    hybrid::remapVlShortMessage(
                                        packed, nativeChannel);
                                if (!voice.prepared) {
                                    // The first worker already has the complete
                                    // ordered setup. Replaying bank/program after
                                    // custom VL SysEx can replace the uploaded voice.
                                    const bool replayCurrentSnapshot =
                                        wrapper.vlSetupHistoryFrozen;
                                    replayVlSetup(wrapper, voiceIndex, channel,
                                                  replayCurrentSnapshot);
                                    // Warm-up consumes the probe note. Release it
                                    // before restoring setup so the real note is
                                    // not rejected as a duplicate active note.
                                    const auto probeNote = warmUpNote(nativePacked);
                                    client->sendShort(probeNote);
                                    client->warmUp();
                                    client->sendShort(noteRelease(probeNote));
                                    replayVlSetup(wrapper, voiceIndex, channel,
                                                  replayCurrentSnapshot);
                                    client->sendShort(probeNote);
                                    client->render(512);
                                    client->sendShort(noteRelease(probeNote));
                                    client->prepare();
                                    voice.prepared = true;
                                    voice.started = true;
                                    voice.timelineFrame = nativeFrame(
                                        wrapper, wrapper.sgTimelineFrames);
                                    wrapper.vlSetupHistoryFrozen = true;
                                } else if (allocation.reassigned) {
                                    replayVlSysexSetup(wrapper, voiceIndex,
                                                       channel);
                                    queueVl(wrapper, voice, midi->deltaFrames,
                                            0x000078b0u | nativeChannel);
                                    wrapper.vlChannelSnapshots[channel].replay(
                                        [&](std::uint32_t message) {
                                            queueVl(wrapper, voice,
                                                    midi->deltaFrames,
                                                    hybrid::remapVlShortMessage(
                                                        message,
                                                        nativeChannel));
                                        });
                                }
                                queueVl(wrapper, voice, midi->deltaFrames,
                                        nativePacked);
                                }
                            }
                        } else if (isNoteOff(packed)) {
                            const auto voiceIndex = wrapper.vlVoiceAllocator.noteOff(
                                channel, midiNote(packed));
                            if (voiceIndex
                                != decltype(wrapper.vlVoiceAllocator)::noVoice) {
                                const auto nativeChannel =
                                    hybrid::nativeVlChannel(
                                        wrapper.vlVoiceAllocator
                                            .hasExplicitConfiguration(),
                                        channel);
                                queueVl(wrapper, wrapper.vlVoices[voiceIndex],
                                        midi->deltaFrames,
                                        hybrid::remapVlShortMessage(
                                            packed, nativeChannel));
                            }
                        } else {
                            bool delivered = false;
                            for (std::uint8_t voiceIndex = 0;
                                 voiceIndex < wrapper.vlVoices.size();
                                 ++voiceIndex) {
                                auto& voice = wrapper.vlVoices[voiceIndex];
                                if (voice.started
                                    && wrapper.vlVoiceAllocator.channel(voiceIndex)
                                        == channel) {
                                    const auto nativeChannel =
                                        hybrid::nativeVlChannel(
                                            wrapper.vlVoiceAllocator
                                                .hasExplicitConfiguration(),
                                            channel);
                                    queueVl(wrapper, voice, midi->deltaFrames,
                                            hybrid::remapVlShortMessage(
                                                packed, nativeChannel));
                                    delivered = true;
                                }
                            }
                            if (!delivered && !wrapper.vlSetupHistoryFrozen
                                && !retainVlShort(wrapper, channel,
                                                  packed)) {
                                wrapper.vlSetupHistoryFrozen = true;
                            }
                        }
                        if (clearsHeldNotes(packed))
                            wrapper.vlVoiceAllocator.releaseChannel(channel);
                    } catch (const std::exception& error) {
                        reportVlFailure("MIDI processing failure", error.what());
                        sendToChild = true;
                    } catch (...) {
                        reportVlFailure("MIDI processing failure");
                        sendToChild = true;
                    }
                }
                if (wrapper.sg.client != nullptr && wrapper.sg.started) {
                    try {
                        queueSgShort(wrapper, midi->deltaFrames, packed);
                        if (hybrid::sgOwnsNote(packed,
                                               wrapper.sg.routeMask)) {
                            sendToChild = false;
                        }
                    } catch (const std::exception& error) {
                        disableSg(wrapper, "MIDI processing failure",
                                  error.what());
                        sendToChild = destination
                            != hybrid::MidiDestination::vl;
                    } catch (...) {
                        disableSg(wrapper, "MIDI processing failure");
                        sendToChild = destination
                            != hybrid::MidiDestination::vl;
                    }
                }
            } else if (event != nullptr && event->type == 6) {
                const auto* sysex = reinterpret_cast<const vst2::SysexEvent*>(event);
                if (sysex->sysexDump != nullptr && sysex->dumpBytes > 0) {
                    const std::span<const std::uint8_t> bytes {
                        reinterpret_cast<const std::uint8_t*>(sysex->sysexDump),
                        static_cast<std::size_t>(sysex->dumpBytes),
                    };
                    const auto eventFrame = wrapper.sgTimelineFrames
                        + static_cast<std::uint64_t>(
                            std::max(0, sysex->deltaFrames));
                    const bool systemReset = isSystemReset(bytes);
                    if (systemReset) {
                        wrapper.router.reset();
                        resetVlPlaybackState(wrapper);
                        clearVlSetup(wrapper);
                        if (wrapper.sg.client == nullptr)
                            clearSgSetup(wrapper);
                    }
                    if (const auto assignment =
                            hybrid::vlVoiceAssignment(bytes)) {
                        const auto voiceIndex = assignment->voice;
                        auto& voice = wrapper.vlVoices[voiceIndex];
                        if (wrapper.vlVoiceAllocator.configureVoice(
                                voiceIndex, assignment->channel)
                            && voice.started) {
                            queueVl(wrapper, voice, sysex->deltaFrames,
                                    0x000078b2u);
                        }
                    }
                    try {
                        const bool createSg = hybrid::isSgConfiguration(bytes)
                            && wrapper.sg.client == nullptr;
                        if (createSg) {
                            reportSgDiagnostic("trigger", wrapper.sgAvailable,
                                               wrapper.sg.routeMask);
                        }
                        auto* sg = createSg ? ensureSg(wrapper)
                                            : wrapper.sg.client.get();
                        if (sg != nullptr) {
                            if (createSg) {
                                replaySgSetup(wrapper, eventFrame);
                                wrapper.sgSetupHistoryFrozen = true;
                            }
                            queueSgSysex(wrapper, sysex->deltaFrames, bytes);
                        }
                    } catch (const std::exception& error) {
                        disableSg(wrapper, "SysEx processing failure",
                                  error.what());
                    } catch (...) {
                        disableSg(wrapper, "SysEx processing failure");
                    }
                    if (wrapper.sg.client == nullptr
                        && !wrapper.sgSetupHistoryFrozen
                        && !retainSgSysex(wrapper, bytes, eventFrame)) {
                        wrapper.sgSetupHistoryFrozen = true;
                    }
                    for (std::uint8_t voiceIndex = 0;
                         voiceIndex < wrapper.vlVoices.size(); ++voiceIndex) {
                        auto& voice = wrapper.vlVoices[voiceIndex];
                        const auto channel = wrapper.vlVoiceAllocator.channel(
                            voiceIndex);
                        if (voice.client == nullptr
                            || channel
                                == decltype(wrapper.vlVoiceAllocator)::unassignedChannel)
                            continue;
                        try {
                            const auto nativeChannel =
                                hybrid::nativeVlChannel(
                                    wrapper.vlVoiceAllocator
                                        .hasExplicitConfiguration(),
                                    channel);
                            sendVlSysexForChannel(wrapper, *voice.client,
                                                  bytes,
                                                  channel, nativeChannel);
                        } catch (const std::exception& error) {
                            disableVlVoice(wrapper, voiceIndex,
                                           "SysEx processing failure",
                                           error.what());
                        } catch (...) {
                            disableVlVoice(wrapper, voiceIndex,
                                           "SysEx processing failure");
                        }
                    }
                    if (!retainVlSysex(wrapper, bytes)) {
                        wrapper.vlSetupHistoryFrozen = true;
                    }
                }
            }

            if (sendToChild && event != nullptr) {
                retainChildEvent(wrapper, event, firstChildEvent);
                firstChildEvent = false;
            }
        }
    } catch (const std::exception& error) {
        reportVlFailure("MIDI processing failure", error.what());
    } catch (...) {
        reportVlFailure("MIDI processing failure");
    }
    for (std::size_t index = firstNewBatch;
         index < wrapper.childBatchCount; ++index) {
        result |= wrapper.child->dispatcher(
            wrapper.child, vst2::processEvents, 0, 0,
            &wrapper.childBatches[index], 0.0f);
    }
    return result;
}

float* vlBus(WrapperState& wrapper, std::size_t bus)
{
    return wrapper.vlOutputBuses.data()
        + bus * wrapper.vlOutputCapacityFrames;
}

float* nativeBus(WrapperState& wrapper, std::size_t bus)
{
    return wrapper.nativeOutputBuses.data()
        + bus * wrapper.nativeOutputCapacityFrames;
}

void mixVlChannelBlock(WrapperState& wrapper, VlVoiceState& voice,
                       std::int32_t outputOffset, std::uint32_t frames)
{
    for (std::size_t plane = 0; plane < hybrid::ipc::planeCount; ++plane) {
        const auto stereo = voice.client->plane(plane, frames);
        auto* left = nativeBus(wrapper, plane * 2);
        auto* right = nativeBus(wrapper, plane * 2 + 1);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            left[outputOffset + frame] += stereo[frame * 2] * int16Scale;
            right[outputOffset + frame] += stereo[frame * 2 + 1]
                * int16Scale;
        }
    }
}

bool beginVlVoiceBlock(WrapperState& wrapper, std::uint8_t voiceIndex,
                       std::uint32_t frames)
{
    auto& voice = wrapper.vlVoices[voiceIndex];
    if (voice.client == nullptr || !voice.started) {
        voice.pendingCount = 0;
        return false;
    }
    std::size_t eventCount = 0;
    std::int32_t position = 0;
    hybrid::renderNativeMidiTimeline(
        voice.pending, voice.pendingCount, voice.timelineFrame,
        static_cast<std::int32_t>(frames),
        [&](std::int32_t offset, std::int32_t count) {
            position = offset + count;
        },
        [&](std::uint32_t message) {
            wrapper.vlTimedMidiScratch[eventCount++] = {
                static_cast<std::uint32_t>(position), message
            };
        });
    voice.client->beginTimedRender(
        frames, { wrapper.vlTimedMidiScratch.data(), eventCount });
    return true;
}

void renderSgSegment(WrapperState& wrapper, std::int32_t outputOffset,
                     std::int32_t frames)
{
    while (frames > 0) {
        const auto block = static_cast<std::uint32_t>(std::min<std::int32_t>(
            frames, hybrid::NativeSgClient::maxFrames));
        wrapper.sg.client->render(block);
        for (std::size_t plane = 0; plane < hybrid::ipc::planeCount; ++plane) {
            const auto stereo = wrapper.sg.client->plane(plane, block);
            auto* left = nativeBus(wrapper, plane * 2);
            auto* right = nativeBus(wrapper, plane * 2 + 1);
            for (std::uint32_t frame = 0; frame < block; ++frame) {
                left[outputOffset + frame] += stereo[frame * 2] * int16Scale;
                right[outputOffset + frame] += stereo[frame * 2 + 1]
                    * int16Scale;
            }
        }
        outputOffset += static_cast<std::int32_t>(block);
        frames -= static_cast<std::int32_t>(block);
    }
}

void renderSg(WrapperState& wrapper, std::int32_t frames,
              std::int32_t cachedPrefix)
{
    if (wrapper.sg.client == nullptr || !wrapper.sg.started || frames < 0) {
        wrapper.sg.pendingCount = 0;
        wrapper.sg.pendingSysexSize = 0;
        return;
    }
    try {
        if (!wrapper.nativeRateAdapter.active()) {
            std::int32_t position = 0;
            for (std::size_t index = 0;
                 index < wrapper.sg.pendingCount; ++index) {
                const auto eventPosition = std::clamp(
                    wrapper.sg.pending[index].deltaFrames - cachedPrefix,
                    position, frames);
                renderSgSegment(wrapper, position, eventPosition - position);
                const auto& event = wrapper.sg.pending[index];
                if (event.kind == PendingSgKind::shortMessage) {
                    wrapper.sg.client->sendShort(event.value);
                } else {
                    wrapper.sg.client->sendSysex({
                        wrapper.sg.pendingSysex.data() + event.dataOffset,
                        event.dataSize
                    });
                    const auto routeMask = wrapper.sg.client->routeMask();
                    if (routeMask != wrapper.sg.routeMask) {
                        wrapper.sg.routeMask = routeMask;
                        reportSgDiagnostic("configured", wrapper.sgAvailable,
                                           wrapper.sg.routeMask);
                    }
                }
                position = eventPosition;
            }
            renderSgSegment(wrapper, position, frames - position);
            wrapper.sg.pendingCount = 0;
            wrapper.sg.pendingSysexSize = 0;
            return;
        }

        const auto startFrame = wrapper.sg.timelineFrame;
        const auto endFrame = startFrame
            + static_cast<std::uint64_t>(std::max(0, frames));
        auto position = startFrame;
        std::size_t consumed = 0;
        while (consumed < wrapper.sg.pendingCount
               && wrapper.sg.pending[consumed].frame <= endFrame) {
            const auto eventPosition = std::clamp(
                wrapper.sg.pending[consumed].frame, position, endFrame);
            renderSgSegment(
                wrapper, static_cast<std::int32_t>(position - startFrame),
                static_cast<std::int32_t>(eventPosition - position));
            const auto& event = wrapper.sg.pending[consumed];
            if (event.kind == PendingSgKind::shortMessage) {
                wrapper.sg.client->sendShort(event.value);
            } else {
                wrapper.sg.client->sendSysex({
                    wrapper.sg.pendingSysex.data() + event.dataOffset,
                    event.dataSize
                });
                const auto routeMask = wrapper.sg.client->routeMask();
                if (routeMask != wrapper.sg.routeMask) {
                    wrapper.sg.routeMask = routeMask;
                    reportSgDiagnostic("configured", wrapper.sgAvailable,
                                       wrapper.sg.routeMask);
                }
            }
            position = eventPosition;
            ++consumed;
        }
        renderSgSegment(
            wrapper, static_cast<std::int32_t>(position - startFrame),
            static_cast<std::int32_t>(endFrame - position));
        wrapper.sg.timelineFrame = endFrame;
        if (consumed != 0) {
            std::move(wrapper.sg.pending.begin() + consumed,
                      wrapper.sg.pending.begin() + wrapper.sg.pendingCount,
                      wrapper.sg.pending.begin());
            wrapper.sg.pendingCount -= consumed;
        }
        if (wrapper.sg.pendingCount == 0)
            wrapper.sg.pendingSysexSize = 0;
    } catch (const std::exception& error) {
        disableSg(wrapper, "audio rendering failure", error.what());
    } catch (...) {
        disableSg(wrapper, "audio rendering failure");
    }
}

bool renderNativeAudio(WrapperState& wrapper, std::int32_t frames,
                       std::int32_t cachedPrefix)
{
    if (frames < 0
        || static_cast<std::size_t>(frames)
            > wrapper.nativeOutputCapacityFrames) {
        return false;
    }
    for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus) {
        std::fill_n(nativeBus(wrapper, bus), frames, 0.0f);
    }
    std::int32_t outputOffset = 0;
    while (outputOffset < frames) {
        const auto block = static_cast<std::uint32_t>(
            std::min<std::int32_t>(frames - outputOffset,
                                   hybrid::NativeVlClient::maxFrames));
        std::array<bool, maxVlVoices> inFlight {};
        for (std::uint8_t voice = 0; voice < wrapper.vlVoices.size(); ++voice) {
            try {
                inFlight[voice] = beginVlVoiceBlock(wrapper, voice, block);
            } catch (const std::exception& error) {
                disableVlVoice(wrapper, voice, "audio render dispatch failure",
                               error.what());
            } catch (...) {
                disableVlVoice(wrapper, voice, "audio render dispatch failure");
            }
        }
        for (std::uint8_t voice = 0; voice < wrapper.vlVoices.size(); ++voice) {
            if (!inFlight[voice])
                continue;
            auto& state = wrapper.vlVoices[voice];
            try {
                state.client->finishTimedRender();
                mixVlChannelBlock(wrapper, state, outputOffset, block);
            } catch (const std::exception& error) {
                disableVlVoice(wrapper, voice, "audio rendering failure",
                               error.what());
            } catch (...) {
                disableVlVoice(wrapper, voice, "audio rendering failure");
            }
        }
        outputOffset += static_cast<std::int32_t>(block);
    }
    renderSg(wrapper, frames, cachedPrefix);
    return true;
}

bool renderVl(WrapperState& wrapper, std::int32_t frames,
              std::int32_t cachedPrefix)
{
    if (frames < 0
        || static_cast<std::size_t>(frames) > wrapper.vlOutputCapacityFrames) {
        return false;
    }
    for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus)
        std::fill_n(vlBus(wrapper, bus), frames, 0.0f);

    if (!wrapper.nativeRateAdapter.active()) {
        if (!renderNativeAudio(wrapper, frames, cachedPrefix))
            return false;
        for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus) {
            std::copy_n(nativeBus(wrapper, bus), frames, vlBus(wrapper, bus));
        }
    } else {
        const auto needed = wrapper.nativeRateAdapter.inputFramesNeeded(
            static_cast<std::size_t>(frames));
        if (!renderNativeAudio(wrapper, static_cast<std::int32_t>(needed), 0))
            return false;
        std::array<const float*, vlOutputBusCount> input {};
        std::array<float*, vlOutputBusCount> output {};
        for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus) {
            input[bus] = nativeBus(wrapper, bus);
            output[bus] = vlBus(wrapper, bus);
        }
        wrapper.nativeRateAdapter.append(input, needed);
        wrapper.nativeRateAdapter.process(
            output, static_cast<std::size_t>(frames));
    }
    if (!wrapper.vlRenderDiagnosticWritten) {
        float peak = 0.0f;
        for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus) {
            const auto* samples = vlBus(wrapper, bus);
            for (std::int32_t frame = 0; frame < frames; ++frame)
                peak = std::max(peak, std::abs(samples[frame]));
        }
        if (peak > 0.0f) {
            reportBridgeDiagnostic("render", frames, 0, peak);
            reportBusDiagnostic(wrapper, frames);
            wrapper.vlRenderDiagnosticWritten = true;
        }
    }
    return true;
}

bool xgRenderWindow(const WrapperState& wrapper, std::int32_t frames,
                    std::int32_t& cachedPrefix,
                    std::int32_t& generatedFrames) noexcept
{
    if (wrapper.child == nullptr || wrapper.child->object == nullptr
        || frames < 0) {
        return false;
    }
    const auto* object = static_cast<const std::byte*>(wrapper.child->object);
    cachedPrefix = *reinterpret_cast<const std::int32_t*>(
        object + xgCachedFramesOffset);
    if (cachedPrefix < 0
        || cachedPrefix > static_cast<std::int32_t>(
            hybrid::XgEffectsBridge::quantumFrames)) {
        return false;
    }
    const auto needed = std::max(0, frames - cachedPrefix);
    const auto quantum = static_cast<std::int32_t>(
        hybrid::XgEffectsBridge::quantumFrames);
    generatedFrames = ((needed + quantum - 1) / quantum) * quantum;
    return static_cast<std::size_t>(generatedFrames)
        <= wrapper.vlOutputCapacityFrames;
}

void mixVlDry(WrapperState& wrapper, float** outputs, std::int32_t frames)
{
    if (outputs == nullptr || outputs[0] == nullptr || outputs[1] == nullptr)
        return;
    const auto* left = vlBus(wrapper, dryLeft);
    const auto* right = vlBus(wrapper, dryRight);
    for (std::int32_t frame = 0; frame < frames; ++frame) {
        outputs[0][frame] += left[frame] * wrapper.nativeOutputGain;
        outputs[1][frame] += right[frame] * wrapper.nativeOutputGain;
    }
}

void injectVlBuses(void* context, float* buses,
                   std::uint32_t frames) noexcept
{
    auto& wrapper = *static_cast<WrapperState*>(context);
    if (!wrapper.xgBusDiagnosticWritten) {
        float xgPeak = 0.0f;
        for (std::size_t bus = 0;
             bus < hybrid::XgEffectsBridge::busCount; ++bus) {
            const auto* samples = buses
                + bus * hybrid::XgEffectsBridge::busStrideFrames;
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                xgPeak = std::max(xgPeak, std::abs(samples[frame]));
        }
        if (xgPeak > 0.0f) {
            reportXgBusDiagnostic(buses, frames);
            wrapper.xgBusDiagnosticWritten = true;
        }
    }
    const auto available = wrapper.vlOutputCapacityFrames
        - std::min(wrapper.vlEffectsCursor, wrapper.vlOutputCapacityFrames);
    const auto count = std::min<std::size_t>(frames, available);
    const auto sourceOffset = wrapper.vlEffectsCursor;
    if (!wrapper.vlHookDiagnosticWritten) {
        float peak = 0.0f;
        for (std::size_t bus = 0; bus < vlOutputBusCount; ++bus) {
            const auto* source = vlBus(wrapper, bus) + sourceOffset;
            for (std::size_t frame = 0; frame < count; ++frame)
                peak = std::max(peak, std::abs(source[frame]));
        }
        if (peak > 0.0f) {
            reportBridgeDiagnostic("hook", static_cast<std::int32_t>(frames),
                                   static_cast<std::int32_t>(sourceOffset), peak);
            wrapper.vlHookDiagnosticWritten = true;
        }
    }
    const auto mixBus = [&](std::size_t sourceBus,
                            std::size_t destinationBus) noexcept {
        const auto* source = vlBus(wrapper, sourceBus) + sourceOffset;
        auto* destination = buses
            + destinationBus * hybrid::XgEffectsBridge::busStrideFrames;
        for (std::size_t frame = 0; frame < count; ++frame)
            destination[frame] += source[frame] * xgInternalBusScale
                * wrapper.nativeOutputGain;
    };

    mixBus(dryLeft, 0);
    mixBus(dryRight, 1);
    mixBus(reverbLeft, 2);
    mixBus(reverbRight, 3);
    mixBus(chorusLeft, 4);
    mixBus(chorusRight, 5);
    mixBus(variationLeft, 6);
    mixBus(variationRight, 7);
    wrapper.vlEffectsCursor += frames;
}

vst2::IntPtr dispatch(vst2::AEffect* effect, std::int32_t opcode,
                      std::int32_t index, vst2::IntPtr value, void* data,
                      float option)
{
    auto* wrapper = state(effect);
    if (wrapper == nullptr || wrapper->child == nullptr)
        return 0;
    if (opcode == vst2::getEffectName)
        return writeVstString(data, hybridEffectName, 32);
    if (opcode == vst2::getVendorString)
        return writeVstString(data, hybridVendorName, 64);
    if (opcode == vst2::getProductString)
        return writeVstString(data, hybridEffectName, 64);
    if (opcode == vst2::getVendorVersion)
        return hybridVendorVersion;
    if (opcode == vst2::processEvents)
        return processEvents(*wrapper, static_cast<const vst2::Events*>(data));
    if (opcode != vst2::close) {
        const auto result = wrapper->child->dispatcher(
            wrapper->child, opcode, index, value, data, option);
        if (opcode == vst2::setSampleRate)
            configureVl(*wrapper, option);
        if (opcode == vst2::setBlockSize && value > 0) {
            try {
                configureAudioBuffers(*wrapper,
                                      static_cast<std::size_t>(value));
            } catch (const std::exception& error) {
                wrapper->vlOutputCapacityFrames = 0;
                wrapper->vlOutputBuses.clear();
                wrapper->nativeOutputCapacityFrames = 0;
                wrapper->nativeOutputBuses.clear();
                reportVlFailure("block buffer allocation failure", error.what());
            }
        }
        return result;
    }

    const auto result = wrapper->child->dispatcher(wrapper->child, opcode, index,
                                                   value, data, option);
    resetVlVoices(*wrapper);
    clearSg(*wrapper);
    if (wrapper->module != nullptr) {
        if (wrapper->xgEffectsBridgeAvailable)
            hybrid::XgEffectsBridge::release(wrapper->module);
        FreeLibrary(wrapper->module);
    }
    delete wrapper;
    delete effect;
    return result;
}

void process(vst2::AEffect* effect, float** inputs, float** outputs,
             std::int32_t frames)
{
    auto& wrapper = *state(effect);
    std::int32_t cachedPrefix {};
    std::int32_t generatedFrames {};
    const auto useEffects = wrapper.xgEffectsBridgeAvailable
        && xgRenderWindow(wrapper, frames, cachedPrefix, generatedFrames);
    const auto renderedVl = useEffects
        ? renderVl(wrapper, generatedFrames, cachedPrefix)
        : renderVl(wrapper, frames, 0);
    wrapper.vlEffectsCursor = 0;
    if (useEffects && generatedFrames != 0)
        hybrid::XgEffectsBridge::beginBlock(&wrapper, injectVlBuses);
    if (wrapper.child->process != nullptr)
        wrapper.child->process(wrapper.child, inputs, outputs, frames);
    if (useEffects && generatedFrames != 0)
        hybrid::XgEffectsBridge::endBlock();
    clearChildEvents(wrapper);
    if (renderedVl && !useEffects)
        mixVlDry(wrapper, outputs, frames);
    wrapper.sgTimelineFrames += static_cast<std::uint64_t>(
        std::max(0, frames));
}

void processReplacing(vst2::AEffect* effect, float** inputs, float** outputs,
                      std::int32_t frames)
{
    auto& wrapper = *state(effect);
    std::int32_t cachedPrefix {};
    std::int32_t generatedFrames {};
    const auto useEffects = wrapper.xgEffectsBridgeAvailable
        && xgRenderWindow(wrapper, frames, cachedPrefix, generatedFrames);
    const auto renderedVl = useEffects
        ? renderVl(wrapper, generatedFrames, cachedPrefix)
        : renderVl(wrapper, frames, 0);
    wrapper.vlEffectsCursor = 0;
    if (useEffects && generatedFrames != 0)
        hybrid::XgEffectsBridge::beginBlock(&wrapper, injectVlBuses);
    if (wrapper.child->processReplacing != nullptr)
        wrapper.child->processReplacing(wrapper.child, inputs, outputs, frames);
    else if (wrapper.child->process != nullptr)
        wrapper.child->process(wrapper.child, inputs, outputs, frames);
    if (useEffects && generatedFrames != 0)
        hybrid::XgEffectsBridge::endBlock();
    clearChildEvents(wrapper);
    if (renderedVl && !useEffects)
        mixVlDry(wrapper, outputs, frames);
    wrapper.sgTimelineFrames += static_cast<std::uint64_t>(
        std::max(0, frames));
}

void setParameter(vst2::AEffect* effect, std::int32_t index, float value)
{
    auto* child = state(effect)->child;
    child->setParameter(child, index, value);
}

float getParameter(vst2::AEffect* effect, std::int32_t index)
{
    auto* child = state(effect)->child;
    return child->getParameter(child, index);
}

} // namespace

extern "C" __declspec(dllexport) vst2::AEffect* VSTPluginMain(
    vst2::HostCallback host)
{
    wchar_t wrapperPath[MAX_PATH] {};
    HMODULE self {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&VSTPluginMain), &self)
        || GetModuleFileNameW(self, wrapperPath, MAX_PATH) == 0) {
        return nullptr;
    }

    const std::filesystem::path directory =
        std::filesystem::path(wrapperPath).parent_path();
    const auto childPath = directory / L"syxg50-engine.bin";
    const auto module = LoadLibraryW(childPath.c_str());
    if (module == nullptr)
        return nullptr;
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "main"));
    if (entry == nullptr) {
        FreeLibrary(module);
        return nullptr;
    }

    const auto vxdPath = directory / L"Sxgpvknl.vxd";
    const auto sgVxdPath = directory / L"sxgsgknl.vxd";
    const auto reportedRate = static_cast<float>(
        host(nullptr, vst2::hostGetSampleRate, 0, 0, nullptr, 0.0f));
    const auto initialRate = reportedRate > 0.0f ? reportedRate : 44'100.0f;
    const auto workerPath = directory / L"syxg100-vl-worker.exe";
    const auto sgWorkerPath = directory / L"syxg100-sg-worker.exe";

    auto* child = entry(host);
    if (child == nullptr || child->magic != vst2::effectMagic) {
        FreeLibrary(module);
        return nullptr;
    }

    auto* wrapperState = new (std::nothrow) WrapperState;
    auto* effect = new (std::nothrow) vst2::AEffect {};
    if (wrapperState == nullptr || effect == nullptr) {
        delete wrapperState;
        delete effect;
        child->dispatcher(child, vst2::close, 0, 0, nullptr, 0.0f);
        FreeLibrary(module);
        return nullptr;
    }
    wrapperState->module = module;
    wrapperState->child = child;
    wrapperState->vxdPath = vxdPath;
    wrapperState->workerPath = workerPath;
    wrapperState->sgVxdPath = sgVxdPath;
    wrapperState->sgWorkerPath = sgWorkerPath;
    wrapperState->sampleRate = initialRate;
    wrapperState->nativeOutputGain = readNativeOutputGain();
    wrapperState->vlAvailable = std::filesystem::is_regular_file(vxdPath)
        && std::filesystem::is_regular_file(workerPath);
    wrapperState->sgAvailable = std::filesystem::is_regular_file(sgVxdPath)
        && std::filesystem::is_regular_file(sgWorkerPath);
    try {
        configureAudioBuffers(*wrapperState,
                              hybrid::NativeVlClient::maxFrames);
    } catch (...) {
        wrapperState->vlOutputCapacityFrames = 0;
        wrapperState->vlOutputBuses.clear();
        wrapperState->nativeOutputCapacityFrames = 0;
        wrapperState->nativeOutputBuses.clear();
    }
    wchar_t disableEffects[2] {};
    const auto effectsDisabled = GetEnvironmentVariableW(
        L"SYXG100_DISABLE_XG_EFFECTS", disableEffects,
        static_cast<DWORD>(std::size(disableEffects))) != 0;
    wrapperState->xgEffectsBridgeAvailable = !effectsDisabled
        && hybrid::XgEffectsBridge::acquire(module);
    *effect = *child;
    effect->dispatcher = dispatch;
    effect->process = process;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->object = wrapperState;
    effect->processReplacing = processReplacing;
    effect->uniqueId = hybridUniqueId;
    return effect;
}
