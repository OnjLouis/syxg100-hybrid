#include "../Source/LeImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uintptr_t imageBase = 0x20000000;
constexpr std::uintptr_t controlDispatcherOffset = 0x21e;
constexpr std::uintptr_t sgDispatcherOffset = 0x1419f8;
constexpr std::uintptr_t vfmOpenOffset = 0x143699;
constexpr std::uintptr_t vfmCloseOffset = 0x1437d5;
constexpr std::uintptr_t vfmTickOffset = 0x1441e8;
constexpr std::uintptr_t processEventsOffset = 0x142109;
constexpr std::uintptr_t midiParserPointerOffset = 0x1218a4;
constexpr std::uintptr_t voiceActivityOffset = 0x1210a4;
constexpr std::uintptr_t renderActivityMaskOffset = 0x1210c0;
constexpr std::uintptr_t renderModeOffset = 0x12109c;
constexpr std::size_t voiceActivityBytes = 20;
constexpr std::uintptr_t acceptsMidiChannelOffset = 0x15064e;
constexpr std::uintptr_t noteAllocatorOffset = 0x14fec3;
constexpr std::uintptr_t voiceActivationOffset = 0x150060;
constexpr std::uintptr_t voicePreparationCompleteOffset = 0x15255d;
constexpr std::uintptr_t sgEnableOffset = 0x14911d;
constexpr std::uintptr_t laneCoefficientOffset = 0x143e7f;
constexpr std::uintptr_t laneStartOffset = 0x143cae;
constexpr std::uintptr_t renderStatePointerOperandOffset = 0x1460aa;
constexpr std::uintptr_t renderHostBridgeOffset = 0x1460f9;
constexpr std::uintptr_t outputPlaneSelectorOffset = 0x153bfd;
constexpr std::size_t outputPlaneStrideSamples = 0x1000;
constexpr std::size_t outputPlaneCount = 4;
constexpr std::size_t sgVoiceCount = 20;
constexpr std::size_t sgVoiceRecordSize = 0x5c;
constexpr std::size_t rendererLaneCount = 18;
constexpr std::size_t rendererLaneRecordSize = 0xac;
constexpr std::uintptr_t fpuGuardStartOffset = 0x143573;
constexpr std::uintptr_t fpuGuardEndOffset = 0x1435df;
constexpr std::uintptr_t ringCliOffset = 0x1422ef;
constexpr std::uintptr_t ringStiOffset = 0x14230e;
constexpr std::uint32_t vmmHeapAllocate = 0x0001804f;
constexpr std::uint32_t vmmHeapFree = 0x00018051;
std::size_t allocationCount {};
std::size_t allocatedBytes {};
std::size_t freeCount {};
std::size_t noteAllocatorEntries {};
std::size_t voiceActivations {};
std::size_t voicePreparationsCompleted {};
std::size_t sgEnableCalls {};
std::size_t sgDisableCalls {};
std::size_t laneCoefficientCalls {};
std::size_t nonzeroLaneCoefficients {};
float maximumLaneCoefficient {};
std::size_t laneStarts {};
std::size_t enabledLaneStarts {};
std::array<std::uint8_t, 8> firstLaneStartDescriptor {};
std::uintptr_t voiceStateBase {};
std::array<std::size_t, sgVoiceCount> voiceActivationCounts {};
std::size_t maximumPendingRenderCommands {};
std::size_t maximumPendingCommandsAfterAcceptedNote {};
std::array<std::uint8_t, rendererLaneCount> firstAcceptedNoteCommandFlags {};
std::size_t hostRenderCalls {};
std::size_t hostRenderNonzeroSamples {};
std::size_t completedHostRenders {};
std::uint32_t observedRenderActivityMask {};
std::size_t maximumActivityBeforeHostRender {};
std::size_t maximumActivityAfterHostRender {};

LONG WINAPI reportException(EXCEPTION_POINTERS* details)
{
    const auto* record = details->ExceptionRecord;
    auto* context = details->ContextRecord;
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(context->Eip);
    const auto inFpuGuard = context->Eip >= imageBase + fpuGuardStartOffset
        && context->Eip < imageBase + fpuGuardEndOffset;
    const auto isRingInterruptGuard = context->Eip == imageBase + ringCliOffset
        || context->Eip == imageBase + ringStiOffset;
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + noteAllocatorOffset) {
        // The probe replaces PUSH EBP with INT3; reproduce it before resuming.
        context->Esp -= sizeof(std::uint32_t);
        *reinterpret_cast<std::uint32_t*>(context->Esp) = context->Ebp;
        ++context->Eip;
        ++noteAllocatorEntries;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + voiceActivationOffset) {
        // Reproduce MOV byte ptr [ESI + EAX], 1 (C6 04 06 01).
        *reinterpret_cast<std::uint8_t*>(context->Esi + context->Eax) = 1;
        voiceStateBase = context->Eax;
        const auto voiceIndex = context->Esi / sgVoiceRecordSize;
        if (voiceIndex < voiceActivationCounts.size())
            ++voiceActivationCounts[voiceIndex];
        context->Eip += 4;
        ++voiceActivations;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + voicePreparationCompleteOffset) {
        // Reproduce AND byte ptr [ECX + EAX], 0 (80 24 01 00).
        *reinterpret_cast<std::uint8_t*>(context->Ecx + context->Eax) = 0;
        context->Eip += 4;
        ++voicePreparationsCompleted;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + sgEnableOffset) {
        const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
        if ((stack[1] & 0xff) == 1)
            ++sgEnableCalls;
        else if ((stack[1] & 0xff) == 0)
            ++sgDisableCalls;
        context->Esp -= sizeof(std::uint32_t);
        *reinterpret_cast<std::uint32_t*>(context->Esp) = context->Ebp;
        ++context->Eip;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + laneCoefficientOffset) {
        const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
        float coefficient = 0.0f;
        std::memcpy(&coefficient, stack + 2, sizeof(coefficient));
        ++laneCoefficientCalls;
        nonzeroLaneCoefficients += coefficient != 0.0f;
        maximumLaneCoefficient = std::max(maximumLaneCoefficient,
                                          std::abs(coefficient));
        context->Esp -= sizeof(std::uint32_t);
        *reinterpret_cast<std::uint32_t*>(context->Esp) = context->Ebp;
        ++context->Eip;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT
        && context->Eip == imageBase + laneStartOffset) {
        const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
        const auto* descriptor = reinterpret_cast<const std::uint8_t*>(stack[1]);
        if (laneStarts == 0)
            std::copy_n(descriptor, firstLaneStartDescriptor.size(),
                        firstLaneStartDescriptor.begin());
        ++laneStarts;
        enabledLaneStarts += descriptor[5] != 0;
        context->Esp -= sizeof(std::uint32_t);
        *reinterpret_cast<std::uint32_t*>(context->Esp) = context->Ebp;
        ++context->Eip;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION
        && (inFpuGuard || isRingInterruptGuard)) {
        if (instruction[0] == 0xfa || instruction[0] == 0xfb) {
            ++context->Eip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x20
            && instruction[2] == 0xc0) {
            context->Eax = 0;
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x06) {
            context->Eip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x22
            && instruction[2] == 0xc0) {
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && instruction[0] == 0xcd && instruction[1] == 0x20) {
        std::uint32_t service = 0;
        std::memcpy(&service, instruction + 2, sizeof(service));
        const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
        if (service == vmmHeapAllocate) {
            const auto size = stack[1];
            auto* memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
            context->Eax = static_cast<DWORD>(
                reinterpret_cast<std::uintptr_t>(memory));
            allocationCount += memory != nullptr;
            allocatedBytes += memory != nullptr ? size : 0;
            context->Eip = stack[0];
            context->Esp += sizeof(std::uint32_t);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (service == vmmHeapFree) {
            context->Eax = HeapFree(GetProcessHeap(), 0,
                                    reinterpret_cast<void*>(stack[1]));
            freeCount += context->Eax != 0;
            context->Eip = stack[0];
            context->Esp += sizeof(std::uint32_t);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    std::fprintf(stderr,
                 "exception=0x%08lx address=%p eip=0x%08lx eax=0x%08lx "
                 "ebx=0x%08lx ecx=0x%08lx edx=0x%08lx esi=0x%08lx "
                 "edi=0x%08lx esp=0x%08lx parser=0x%08x queue=%u\n",
                 record->ExceptionCode, record->ExceptionAddress,
                 context->Eip, context->Eax, context->Ebx, context->Ecx,
                 context->Edx, context->Esi, context->Edi, context->Esp,
                 *reinterpret_cast<const std::uint32_t*>(
                     imageBase + midiParserPointerOffset),
                 *reinterpret_cast<const std::uint16_t*>(imageBase + 0x102848));
    std::fflush(stderr);
    ExitProcess(2);
}

std::uint32_t callControl(std::uintptr_t dispatcher, std::uint32_t operation)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>(
            dispatcher)(operation, 0, 0, 0);
}

std::uint32_t callSg(std::uintptr_t dispatcher, std::uint32_t operation,
                     std::uint32_t parameter1, std::uint32_t parameter2)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t)>(dispatcher)(
            operation, parameter1, parameter2);
}

std::uint32_t callUnary(std::uintptr_t address, std::uint32_t value)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(std::uint32_t)>(address)(
        value);
}

std::uint32_t callNoArguments(std::uintptr_t address)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)()>(address)();
}

std::uint32_t callTick(std::uintptr_t address, std::int16_t* output,
                       std::uint32_t frames, float scale)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::int16_t*, std::uint32_t, float)>(address)(output, frames, scale);
}

std::uint32_t callHostRender(std::int16_t* output, std::uint32_t frames)
{
    struct HostState {
        std::uint32_t reserved {};
        std::uint32_t active {1};
    } state;
    struct RenderRequest {
        std::uint32_t reserved {};
        std::uint32_t frames {};
        std::uint32_t reserved2 {};
        std::int16_t* output {};
        std::uint32_t completed {};
    } request {0, frames, 0, output, 0};
    ++hostRenderCalls;
    const auto result = reinterpret_cast<std::uint32_t(__cdecl*)(
        HostState*, RenderRequest*)>(
        imageBase + renderHostBridgeOffset)(&state, &request);
    completedHostRenders += request.completed != 0;
    return result;
}

std::size_t countActiveRenderLanes()
{
    const auto* activity = reinterpret_cast<const std::uint8_t*>(
        imageBase + voiceActivityOffset);
    return static_cast<std::size_t>(std::count_if(
        activity, activity + voiceActivityBytes,
        [](auto value) { return value != 0; }));
}

std::size_t countPendingRenderCommands()
{
    const auto pointerAddress = *reinterpret_cast<const std::uint32_t*>(
        imageBase + renderStatePointerOperandOffset);
    const auto state = *reinterpret_cast<const std::uint32_t*>(pointerAddress);
    if (state == 0)
        return 0;
    std::size_t pending = 0;
    for (std::size_t index = 0; index < rendererLaneCount; ++index) {
        pending += *reinterpret_cast<const std::uint8_t*>(
            state + 0x18 + index * rendererLaneRecordSize) != 0;
    }
    return pending;
}

std::array<std::uint8_t, rendererLaneCount> snapshotRenderCommandFlags()
{
    std::array<std::uint8_t, rendererLaneCount> flags {};
    const auto pointerAddress = *reinterpret_cast<const std::uint32_t*>(
        imageBase + renderStatePointerOperandOffset);
    const auto state = *reinterpret_cast<const std::uint32_t*>(pointerAddress);
    if (state == 0)
        return flags;
    for (std::size_t index = 0; index < flags.size(); ++index) {
        flags[index] = *reinterpret_cast<const std::uint8_t*>(
            state + 0x18 + index * rendererLaneRecordSize);
    }
    return flags;
}

std::vector<std::uint8_t> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
        throw std::runtime_error("odd hexadecimal event length");
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            std::stoul(std::string(text.substr(index * 2, 2)), nullptr, 16));
    }
    return bytes;
}

void renderFrames(std::uint32_t frames, std::vector<std::int16_t>& rendered)
{
    // The original SG renderer's fixed scratch area permits at most 388
    // frames; exceeding it overwrites adjacent kernel globals.
    constexpr std::uint32_t blockSize = 256;
    std::vector<std::int16_t> block(blockSize);
    std::vector<std::int16_t> planes(outputPlaneStrideSamples * outputPlaneCount);
    while (frames != 0) {
        const auto count = std::min(frames, blockSize);
        callNoArguments(imageBase + processEventsOffset);
        observedRenderActivityMask |= *reinterpret_cast<const std::uint32_t*>(
            imageBase + renderActivityMaskOffset);
        maximumActivityBeforeHostRender = std::max(
            maximumActivityBeforeHostRender, countActiveRenderLanes());
        maximumPendingRenderCommands = std::max(
            maximumPendingRenderCommands, countPendingRenderCommands());
        std::fill(planes.begin(), planes.end(), 0);
        callHostRender(planes.data(), count);
        observedRenderActivityMask |= *reinterpret_cast<const std::uint32_t*>(
            imageBase + renderActivityMaskOffset);
        maximumActivityAfterHostRender = std::max(
            maximumActivityAfterHostRender, countActiveRenderLanes());
        hostRenderNonzeroSamples += std::count_if(
            planes.begin(), planes.end(), [](auto sample) { return sample != 0; });
        for (std::size_t frame = 0; frame < count; ++frame) {
            int mixed = 0;
            for (std::size_t plane = 0; plane < outputPlaneCount; ++plane)
                mixed += planes[plane * outputPlaneStrideSamples + frame];
            block[frame] = static_cast<std::int16_t>(std::clamp(
                mixed, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
        }
        rendered.insert(rendered.end(), block.begin(), block.begin() + count);
        frames -= count;
    }
}

void replayTrace(std::uintptr_t dispatcher, const std::filesystem::path& path,
                 std::vector<std::int16_t>& rendered)
{
    struct LongMessage {
        const std::uint8_t* data;
        std::uint32_t size;
    };

    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "SGTE 1")
        throw std::runtime_error("unsupported SG event trace");
    std::size_t events = 0;
    std::size_t maximumActiveVoices = 0;
    std::size_t maximumQueuedVoicesAfterPump = 0;
    std::size_t noteOns = 0;
    std::size_t acceptedNoteOns = 0;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::uint32_t delta = 0;
        char kind = 0;
        std::string hexadecimal;
        if (!(fields >> delta >> kind >> hexadecimal))
            continue;
        renderFrames(delta, rendered);
        const auto bytes = parseHex(hexadecimal);
        bool acceptedNoteEvent = false;
        if (kind == 'M') {
            std::uint32_t packed = 0;
            if (bytes.empty() || bytes.size() > sizeof(packed))
                throw std::runtime_error("invalid short MIDI message");
            std::copy(bytes.begin(), bytes.end(),
                      reinterpret_cast<std::uint8_t*>(&packed));
            const auto status = bytes.front();
            if ((status & 0xf0) == 0x90 && bytes.size() == 3
                && bytes[2] != 0) {
                ++noteOns;
                const auto routeAccepted = callUnary(
                    imageBase + acceptsMidiChannelOffset,
                    status & 0x0f) == 0;
                acceptedNoteOns += routeAccepted;
                acceptedNoteEvent = routeAccepted;
            }
            callSg(dispatcher, 17, 0x60, packed);
        } else if (kind == 'S') {
            LongMessage message {bytes.data(),
                                 static_cast<std::uint32_t>(bytes.size())};
            callSg(dispatcher, 17, 0x61,
                   static_cast<std::uint32_t>(
                       reinterpret_cast<std::uintptr_t>(&message)));
        } else {
            throw std::runtime_error("unknown SG event type");
        }
        // Long messages are queued by pointer, so consume them before the
        // temporary descriptor and byte buffer leave scope.
        callNoArguments(imageBase + processEventsOffset);
        if (acceptedNoteEvent) {
            const auto flags = snapshotRenderCommandFlags();
            const auto pending = static_cast<std::size_t>(std::count_if(
                flags.begin(), flags.end(), [](auto value) {
                    return value != 0;
                }));
            if (maximumPendingCommandsAfterAcceptedNote == 0 && pending != 0)
                firstAcceptedNoteCommandFlags = flags;
            maximumPendingCommandsAfterAcceptedNote = std::max(
                maximumPendingCommandsAfterAcceptedNote, pending);
        }
        const auto* activity = reinterpret_cast<const std::uint8_t*>(
            imageBase + voiceActivityOffset);
        maximumActiveVoices = std::max(
            maximumActiveVoices,
            static_cast<std::size_t>(std::count_if(
                activity, activity + voiceActivityBytes,
                [](auto value) { return value != 0; })));
        if (voiceStateBase != 0) {
            std::size_t activeAllocatedVoices = 0;
            for (std::size_t index = 0; index < sgVoiceCount; ++index) {
                activeAllocatedVoices += voiceActivationCounts[index] != 0 &&
                    *reinterpret_cast<const std::uint8_t*>(
                        voiceStateBase + index * sgVoiceRecordSize) != 0;
            }
            maximumQueuedVoicesAfterPump = std::max(
                maximumQueuedVoicesAfterPump, activeAllocatedVoices);
        }
        ++events;
    }
    renderFrames(44'100, rendered);
    std::printf("replayed SG events=%zu rendered frames=%zu "
                "note-ons=%zu accepted note-ons=%zu "
                "maximum legacy flags=%zu "
                "maximum queued voices after pump=%zu\n",
                events, rendered.size(), noteOns, acceptedNoteOns,
                maximumActiveVoices, maximumQueuedVoicesAfterPump);
}

void writeWave(const std::filesystem::path& path,
               const std::vector<std::int16_t>& samples)
{
    const std::uint32_t dataSize = static_cast<std::uint32_t>(
        samples.size() * sizeof(samples.front()));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t sampleRate = 44'100;
    const std::uint32_t byteRate = sampleRate * sizeof(std::int16_t);
    const std::uint16_t format = 1;
    const std::uint16_t channels = 1;
    const std::uint16_t blockAlign = sizeof(std::int16_t);
    const std::uint16_t bits = 16;
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    output.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    output.write("WAVEfmt ", 8);
    const std::uint32_t formatSize = 16;
    output.write(reinterpret_cast<const char*>(&formatSize), sizeof(formatSize));
    output.write(reinterpret_cast<const char*>(&format), sizeof(format));
    output.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    output.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    output.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    output.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    output.write(reinterpret_cast<const char*>(&bits), sizeof(bits));
    output.write("data", 4);
    output.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    output.write(reinterpret_cast<const char*>(samples.data()), dataSize);
    if (!output)
        throw std::runtime_error("cannot write SG wave output");
}

void reportVoiceActivity()
{
    const auto* activity = reinterpret_cast<const std::uint8_t*>(
        imageBase + voiceActivityOffset);
    const auto active = std::count_if(activity, activity + voiceActivityBytes,
                                      [](auto value) { return value != 0; });
    std::printf("SG active voice flags=%zu/%zu", active, voiceActivityBytes);
    for (std::size_t index = 0; index < voiceActivityBytes; ++index)
        std::printf(" %02x", activity[index]);
    std::printf("\n");
}

void reportAllocatedVoiceTable()
{
    if (voiceStateBase == 0) {
        std::printf("SG allocated voice table unavailable\n");
        return;
    }
    std::size_t active = 0;
    std::printf("SG allocated voice records:");
    for (std::size_t index = 0; index < sgVoiceCount; ++index) {
        const auto state = *reinterpret_cast<const std::uint8_t*>(
            voiceStateBase + index * sgVoiceRecordSize);
        active += voiceActivationCounts[index] != 0 && state != 0;
        if (voiceActivationCounts[index] != 0)
            std::printf(" %zu:%zu/final-%02x", index,
                        voiceActivationCounts[index], state);
    }
    std::printf(" active=%zu/%zu\n", active, sgVoiceCount);
}

void reportAcceptedChannels()
{
    std::printf("SG accepted MIDI channels:");
    for (std::uint32_t channel = 0; channel < 16; ++channel) {
        if (callUnary(imageBase + acceptsMidiChannelOffset, channel) == 0)
            std::printf(" %u", channel + 1);
    }
    std::printf("\n");
}

void reportCurrentRoute()
{
    const auto currentIndexPointerAddress =
        *reinterpret_cast<const std::uint32_t*>(
            imageBase + acceptsMidiChannelOffset + 0x52);
    const auto routeTablePointerAddress =
        *reinterpret_cast<const std::uint32_t*>(
            imageBase + acceptsMidiChannelOffset + 0x5f);
    const auto currentIndexPointer =
        *reinterpret_cast<const std::uint32_t*>(currentIndexPointerAddress);
    const auto routeTable =
        *reinterpret_cast<const std::uint32_t*>(routeTablePointerAddress);
    const auto index = *reinterpret_cast<const std::uint8_t*>(
        currentIndexPointer);
    const auto* route = reinterpret_cast<const std::uint8_t*>(
        routeTable + index * 0x78);
    std::printf("SG current route index=%u channel=%u mode=%u enabled=%u "
                "note-range=%u-%u velocity-range=%u-%u\n",
                index, route[4] + 1, route[5], route[0x2e], route[0x0f],
                route[0x10], route[0x66], route[0x67]);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2 || argc > 4) {
            std::fprintf(stderr,
                         "usage: SgNativeProbe <sxgsgknl.vxd> "
                         "[events.sgte.txt] [output.wav]\n");
            return 64;
        }
        const auto image = hybrid::loadLeImage(argv[1], imageBase);
        auto* allocation = VirtualAlloc(reinterpret_cast<void*>(imageBase),
                                        image.size(), MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE);
        if (allocation != reinterpret_cast<void*>(imageBase))
            throw std::runtime_error("cannot reserve SG image base");
        std::copy(image.begin(), image.end(), static_cast<std::uint8_t*>(allocation));
        // The VxD stores a 0x24 selector above this 0x2000 plane stride.
        *reinterpret_cast<std::uint32_t*>(
            imageBase + outputPlaneSelectorOffset) = 0x2000;
        *reinterpret_cast<std::uint8_t*>(imageBase + noteAllocatorOffset) = 0xcc;
        *reinterpret_cast<std::uint8_t*>(imageBase + voiceActivationOffset) = 0xcc;
        *reinterpret_cast<std::uint8_t*>(
            imageBase + voicePreparationCompleteOffset) = 0xcc;
        *reinterpret_cast<std::uint8_t*>(imageBase + sgEnableOffset) = 0xcc;
        *reinterpret_cast<std::uint8_t*>(imageBase + laneCoefficientOffset) = 0xcc;
        *reinterpret_cast<std::uint8_t*>(imageBase + laneStartOffset) = 0xcc;
        FlushInstructionCache(GetCurrentProcess(), allocation, image.size());
        AddVectoredExceptionHandler(1, reportException);
        const auto dispatcher = imageBase + controlDispatcherOffset;
        for (std::uint32_t operation = 0; operation <= 2; ++operation) {
            const auto result = callControl(dispatcher, operation);
            std::printf("control %u returned 0x%08x\n", operation, result);
            std::fflush(stdout);
        }
        const auto dynamicResult = callControl(dispatcher, 27);
        std::printf("control 27 returned 0x%08x\n", dynamicResult);
        std::printf("SG allocations=%zu bytes=%zu\n", allocationCount,
                    allocatedBytes);
        const auto parserPointer = reinterpret_cast<std::uint32_t*>(
            imageBase + midiParserPointerOffset);
        std::printf("MIDI parser after control 27=0x%08x\n", *parserPointer);
        const auto resetResult = callSg(imageBase + sgDispatcherOffset,
                                        5, 0, 0);
        std::printf("SG reset returned 0x%08x\n", resetResult);
        const auto openResult = callUnary(imageBase + vfmOpenOffset, 44'100);
        std::printf("vfmOpen returned 0x%08x\n", openResult);
        std::printf("SG render mode after vfmOpen=%u\n",
                    *reinterpret_cast<const std::uint16_t*>(
                        imageBase + renderModeOffset));
        std::printf("MIDI parser after vfmOpen=0x%08x\n", *parserPointer);
        callSg(imageBase + sgDispatcherOffset, 17, 5, 0);
        const auto messageOpen = callSg(imageBase + sgDispatcherOffset,
                                        17, 0x20, 0);
        callUnary(imageBase + sgEnableOffset, 1);
        std::printf("MIDI parser after message open=0x%08x\n", *parserPointer);
        std::vector<std::int16_t> output;
        std::uint32_t tickResult = 0;
        if (argc >= 3) {
            replayTrace(imageBase + sgDispatcherOffset, argv[2], output);
        } else {
            const auto noteOn = callSg(imageBase + sgDispatcherOffset,
                                       17, 0x60, 0x00643c90);
            std::printf("SG message open=0x%08x note-on=0x%08x\n",
                        messageOpen, noteOn);
            output.resize(512, -1);
            callNoArguments(imageBase + processEventsOffset);
            tickResult = callTick(imageBase + vfmTickOffset,
                                  output.data(), output.size(), 1.0f);
            callSg(imageBase + sgDispatcherOffset, 17, 0x60, 0x00003c80);
        }
        reportAcceptedChannels();
        reportCurrentRoute();
        reportVoiceActivity();
        std::printf("SG final render mode=%u\n",
                    *reinterpret_cast<const std::uint16_t*>(
                        imageBase + renderModeOffset));
        std::printf("SG note allocator entries=%zu voice activations=%zu "
                    "voice preparations completed=%zu\n", noteAllocatorEntries,
                    voiceActivations, voicePreparationsCompleted);
        std::printf("SG lane coefficients=%zu nonzero=%zu max=%g\n",
                    laneCoefficientCalls, nonzeroLaneCoefficients,
                    maximumLaneCoefficient);
        std::printf("SG lane starts=%zu enabled=%zu first descriptor:",
                    laneStarts, enabledLaneStarts);
        for (const auto value : firstLaneStartDescriptor)
            std::printf(" %02x", value);
        std::printf("\n");
        std::printf("SG maximum pending renderer command lanes=%zu\n",
                    maximumPendingRenderCommands);
        std::printf("SG host render calls=%zu completed=%zu "
                    "nonzero plane samples=%zu activity mask=0x%08x "
                    "active lanes before/after=%zu/%zu\n", hostRenderCalls,
                    completedHostRenders, hostRenderNonzeroSamples,
                    observedRenderActivityMask, maximumActivityBeforeHostRender,
                    maximumActivityAfterHostRender);
        std::printf("SG pending lanes after accepted note=%zu flags:",
                    maximumPendingCommandsAfterAcceptedNote);
        for (const auto flag : firstAcceptedNoteCommandFlags)
            std::printf(" %02x", flag);
        std::printf("\n");
        reportAllocatedVoiceTable();
        const auto nonzero = std::count_if(output.begin(), output.end(),
                                           [](auto sample) { return sample != 0; });
        if (argc >= 3) {
            std::printf("SG native host output nonzero=%zu/%zu\n", nonzero,
                        output.size());
        } else {
            std::printf("vfmTick returned 0x%08x nonzero=%zu/%zu\n", tickResult,
                        nonzero, output.size());
        }
        if (argc == 4)
            writeWave(argv[3], output);
        callUnary(imageBase + sgEnableOffset, 0);
        std::printf("SG enable calls=%zu disable calls=%zu\n", sgEnableCalls,
                    sgDisableCalls);
        const auto messageClose = callSg(imageBase + sgDispatcherOffset,
                                         17, 0x40, 0);
        std::printf("SG message close=0x%08x\n", messageClose);
        const auto closeResult = callNoArguments(imageBase + vfmCloseOffset);
        std::printf("vfmClose returned 0x%08x\n", closeResult);
        const auto shutdownResult = callControl(dispatcher, 28);
        std::printf("control 28 returned 0x%08x\n", shutdownResult);
        std::printf("SG frees=%zu outstanding=%zu\n", freeCount,
                    allocationCount - freeCount);
        std::fflush(stdout);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
