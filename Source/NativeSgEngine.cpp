#include "NativeSgEngine.h"

#include "LeImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hybrid {
namespace {

constexpr std::uintptr_t imageBase = 0x20000000;
constexpr std::uintptr_t controlDispatcherOffset = 0x21e;
constexpr std::uintptr_t sgDispatcherOffset = 0x1419f8;
constexpr std::uintptr_t vfmOpenOffset = 0x143699;
constexpr std::uintptr_t vfmCloseOffset = 0x1437d5;
constexpr std::uintptr_t processEventsOffset = 0x142109;
constexpr std::uintptr_t acceptsMidiChannelOffset = 0x15064e;
constexpr std::uintptr_t sgEnableOffset = 0x14911d;
constexpr std::uintptr_t renderHostBridgeOffset = 0x1460f9;
constexpr std::uintptr_t outputPlaneSelectorOffset = 0x153bfd;
constexpr std::uintptr_t fpuGuardStartOffset = 0x143573;
constexpr std::uintptr_t fpuGuardEndOffset = 0x1435df;
constexpr std::uintptr_t ringCliOffset = 0x1422ef;
constexpr std::uintptr_t ringStiOffset = 0x14230e;
constexpr std::uint32_t vmmHeapAllocate = 0x0001804f;
constexpr std::uint32_t vmmHeapFree = 0x00018051;
constexpr std::size_t outputPlaneStrideSamples = 0x1000;
constexpr std::uint32_t nativeQuantumFrames = 256;

std::uint32_t callControl(std::uint32_t operation)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>(
            imageBase + controlDispatcherOffset)(operation, 0, 0, 0);
}

std::uint32_t callSg(std::uint32_t operation, std::uint32_t parameter1,
                     std::uint32_t parameter2)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t)>(
            imageBase + sgDispatcherOffset)(operation, parameter1, parameter2);
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
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        HostState*, RenderRequest*)>(imageBase + renderHostBridgeOffset)(
            &state, &request);
}

} // namespace

class NativeSgEngine::Impl {
public:
    Impl(const std::filesystem::path& vxdPath, std::uint32_t requestedRate)
    {
        try {
            const auto image = loadLeImage(vxdPath, imageBase);
            allocation = VirtualAlloc(reinterpret_cast<void*>(imageBase),
                                      image.size(), MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
            if (allocation != reinterpret_cast<void*>(imageBase))
                throw std::runtime_error("cannot reserve native SG image base");
            imageBytes = image.size();
            std::copy(image.begin(), image.end(),
                      static_cast<std::uint8_t*>(allocation));
            // Remove the obsolete selector byte above Yamaha's 0x2000 stride.
            *reinterpret_cast<std::uint32_t*>(
                imageBase + outputPlaneSelectorOffset) = 0x2000;
            FlushInstructionCache(GetCurrentProcess(), allocation, imageBytes);
            active = this;
            exceptionHandler = AddVectoredExceptionHandler(1, handleException);
            if (exceptionHandler == nullptr)
                throw std::runtime_error("cannot install native SG exception bridge");
            for (std::uint32_t operation = 0; operation <= 2; ++operation) {
                if (callControl(operation) != 1)
                    throw std::runtime_error("native SG control initialization failed");
            }
            if (callControl(27) != 1)
                throw std::runtime_error("native SG dynamic initialization failed");
            controlsInitialized = true;
            callSg(5, 0, 0);
            open(requestedRate);
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void open(std::uint32_t requestedRate)
    {
        if (requestedRate == 0)
            throw std::runtime_error("native SG sample rate is zero");
        if (callUnary(imageBase + vfmOpenOffset, requestedRate) != 0)
            throw std::runtime_error("native SG audio open failed");
        callSg(17, 5, 0);
        callSg(17, 0x20, 0);
        callUnary(imageBase + sgEnableOffset, 1);
        sampleRate = requestedRate;
        audioOpen = true;
    }

    void close() noexcept
    {
        if (!audioOpen)
            return;
        callUnary(imageBase + sgEnableOffset, 0);
        callSg(17, 0x40, 0);
        callNoArguments(imageBase + vfmCloseOffset);
        audioOpen = false;
    }

    void cleanup() noexcept
    {
        if (active == this) {
            close();
            if (controlsInitialized)
                callControl(28);
        }
        controlsInitialized = false;
        if (exceptionHandler != nullptr)
            RemoveVectoredExceptionHandler(exceptionHandler);
        exceptionHandler = nullptr;
        if (active == this)
            active = nullptr;
        if (allocation != nullptr)
            VirtualFree(allocation, 0, MEM_RELEASE);
        allocation = nullptr;
    }

    static LONG WINAPI handleException(EXCEPTION_POINTERS* details)
    {
        if (active == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
        auto* context = details->ContextRecord;
        const auto* instruction = reinterpret_cast<const std::uint8_t*>(
            context->Eip);
        const auto inFpuGuard = context->Eip >= imageBase + fpuGuardStartOffset
            && context->Eip < imageBase + fpuGuardEndOffset;
        const auto ringGuard = context->Eip == imageBase + ringCliOffset
            || context->Eip == imageBase + ringStiOffset;
        if (details->ExceptionRecord->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION
            && (inFpuGuard || ringGuard)) {
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
        if (details->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            && instruction[0] == 0xcd && instruction[1] == 0x20) {
            std::uint32_t service {};
            std::memcpy(&service, instruction + 2, sizeof(service));
            const auto* stack = reinterpret_cast<const std::uint32_t*>(
                context->Esp);
            if (service == vmmHeapAllocate) {
                auto* memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         stack[1]);
                context->Eax = static_cast<DWORD>(
                    reinterpret_cast<std::uintptr_t>(memory));
                context->Eip = stack[0];
                context->Esp += sizeof(std::uint32_t);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (service == vmmHeapFree) {
                context->Eax = HeapFree(GetProcessHeap(), 0,
                                        reinterpret_cast<void*>(stack[1]));
                context->Eip = stack[0];
                context->Esp += sizeof(std::uint32_t);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void sendShort(std::uint32_t packedMessage)
    {
        callSg(17, 0x60, packedMessage);
        callNoArguments(imageBase + processEventsOffset);
    }

    void sendSysex(std::span<const std::uint8_t> bytes)
    {
        struct LongMessage {
            const std::uint8_t* data;
            std::uint32_t size;
        } message {bytes.data(), static_cast<std::uint32_t>(bytes.size())};
        callSg(17, 0x61, static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(&message)));
        callNoArguments(imageBase + processEventsOffset);
    }

    void render(std::uint32_t frames)
    {
        if (frames > ipc::maxFrames)
            throw std::runtime_error("native SG render exceeds IPC capacity");
        std::fill(audio.begin(), audio.end(), 0);
        std::uint32_t outputOffset = 0;
        while (outputOffset < frames) {
            const auto count = std::min(nativeQuantumFrames,
                                        frames - outputOffset);
            callNoArguments(imageBase + processEventsOffset);
            std::fill(nativePlanes.begin(), nativePlanes.end(), 0);
            callHostRender(nativePlanes.data(), count);
            for (std::size_t plane = 0; plane < ipc::planeCount; ++plane) {
                for (std::uint32_t frame = 0; frame < count; ++frame) {
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        audio[plane * ipc::maxStereoSamples
                              + (outputOffset + frame) * 2 + channel]
                            = nativePlanes[plane * outputPlaneStrideSamples
                                           + frame * 2 + channel];
                    }
                }
            }
            outputOffset += count;
        }
    }

    void setSampleRate(std::uint32_t requestedRate)
    {
        if (requestedRate == sampleRate)
            return;
        close();
        open(requestedRate);
    }

    std::uint32_t routeMask() const
    {
        std::uint32_t mask = 0;
        for (std::uint32_t channel = 0; channel < 16; ++channel) {
            if (callUnary(imageBase + acceptsMidiChannelOffset, channel) == 0)
                mask |= std::uint32_t {1} << channel;
        }
        return mask;
    }

    void* allocation {};
    std::size_t imageBytes {};
    void* exceptionHandler {};
    std::uint32_t sampleRate {};
    bool controlsInitialized {};
    bool audioOpen {};
    std::array<std::int16_t,
               ipc::maxStereoSamples * ipc::planeCount> audio {};
    std::array<std::int16_t,
               outputPlaneStrideSamples * ipc::planeCount> nativePlanes {};
    static inline Impl* active {};
};

NativeSgEngine::NativeSgEngine(const std::filesystem::path& vxdPath,
                               std::uint32_t sampleRate)
    : impl(std::make_unique<Impl>(vxdPath, sampleRate))
{
}

NativeSgEngine::~NativeSgEngine() = default;

void NativeSgEngine::sendShort(std::uint32_t packedMessage)
{
    impl->sendShort(packedMessage);
}

void NativeSgEngine::sendSysex(std::span<const std::uint8_t> bytes)
{
    impl->sendSysex(bytes);
}

void NativeSgEngine::setSampleRate(std::uint32_t sampleRate)
{
    impl->setSampleRate(sampleRate);
}

void NativeSgEngine::render(std::uint32_t frames)
{
    impl->render(frames);
}

std::uint32_t NativeSgEngine::routeMask() const
{
    return impl->routeMask();
}

std::span<const std::int16_t> NativeSgEngine::plane(
    std::size_t index, std::uint32_t frames) const
{
    if (index >= ipc::planeCount || frames > ipc::maxFrames)
        return {};
    return {impl->audio.data() + index * ipc::maxStereoSamples,
            static_cast<std::size_t>(frames) * 2};
}

} // namespace hybrid
